#include "../../../fx/runtime/GpuParticleContract.hlsli"

cbuffer GpuParticleControl : register(b0) {
    uint particleCapacity;
    uint activeCount;
    uint authoredFrames;
    uint authorityEpoch;
    uint birthCommandCount;
    uint retireCommandCount;
    uint aliveIndexCapacity;
    uint visibleIndexCapacity;
    uint templateMaterialMapCount;
    uint materialBinCount;
    uint commandPadding;
};

StructuredBuffer<GpuParticleBirthCommand> birthCommands : register(t0);
StructuredBuffer<GpuParticleRetireCommand> retireCommands : register(t1);
StructuredBuffer<uint> templateMaterialBins : register(t2);
StructuredBuffer<uint> visibilityAuthorityGenerations : register(t3);
RWStructuredBuffer<GpuParticleState> particleStates : register(u0);
RWByteAddressBuffer particleCounters : register(u1);
RWBuffer<uint> aliveParticleIndices : register(u2);
RWBuffer<uint> visibleParticleIndices : register(u3);
RWBuffer<uint> materialBinCounts : register(u4);
RWBuffer<uint> materialBinOffsets : register(u5);
RWBuffer<uint> materialBinCursors : register(u6);
RWBuffer<uint> materialParticleIndices : register(u7);
struct ParticleDrawIndexedArguments {
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};
RWStructuredBuffer<ParticleDrawIndexedArguments> materialIndirectArgs
    : register(u8);

[numthreads(64, 1, 1)]
void ResetCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint index = dispatchThreadId.x;
    if (index < particleCapacity) {
        GpuParticleState state = particleStates[index];
        state.positionAndAge = 0.0;
        state.previousAndLifetime = 0.0;
        state.velocityAndGravity = 0.0;
        state.driftAndVelocityDamping = float4(0.0, 0.0, 0.0, 1.0);
        state.sizeDynamicsAndAngle = 0.0;
        state.angularDynamicsAndAlpha = 0.0;
        state.colorAndWindRandomness = 0.0;
        state.emitterOriginAndReserved = 0.0;
        state.laterColorKeyTintAndReserved = 0.0;
        state.identityAndFlags = 0u;
        state.authorityTokens = 0u;
        [unroll]
        for (uint vectorIndex = 0u; vectorIndex < 2u; ++vectorIndex) {
            state.alphaKeyValues[vectorIndex] = 0.0;
            state.alphaKeyFrames[vectorIndex] = 0u;
            state.colorKeyFrames[vectorIndex] = 0u;
        }
        [unroll]
        for (uint key = 0u; key < 8u; ++key) {
            state.colorKeyValues[key] = 0.0;
        }
        particleStates[index] = state;
    }
    if (index == 0u) {
        particleCounters.Store(0u, 0u);   // alive
        particleCounters.Store(4u, 0u);   // rejected/overflow
        particleCounters.Store(8u, authorityEpoch);
        particleCounters.Store(12u, GPU_PARTICLE_CONTRACT_VERSION);
        particleCounters.Store(16u, 0u);  // visible
        particleCounters.Store(20u, 0u);  // visible overflow
        particleCounters.Store(32u, 0u);  // visible membership signature A
        particleCounters.Store(36u, 0u);  // visible membership signature B
        particleCounters.Store(24u, 0u);  // material-binned
        particleCounters.Store(28u, 0u);  // material mapping/overflow
    }
}

[numthreads(64, 1, 1)]
void ApplyRetireCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint commandIndex = dispatchThreadId.x;
    if (commandIndex >= retireCommandCount) return;
    const GpuParticleRetireCommand command = retireCommands[commandIndex];
    if (command.destinationIndex >= particleCapacity ||
        command.authorityEpoch != authorityEpoch) return;
    GpuParticleState state = particleStates[command.destinationIndex];
    if (state.authorityTokens.x == command.destinationIndex &&
        state.authorityTokens.y == command.particleGeneration) {
        state.identityAndFlags.w &= ~GPU_PARTICLE_STATE_ALIVE;
        particleStates[command.destinationIndex] = state;
    }
}

[numthreads(64, 1, 1)]
void ApplyBirthCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint commandIndex = dispatchThreadId.x;
    if (commandIndex >= birthCommandCount) return;
    const GpuParticleBirthCommand command = birthCommands[commandIndex];
    if (command.destinationIndex >= particleCapacity ||
        command.authorityEpoch != authorityEpoch) return;
    particleStates[command.destinationIndex] = command.initialState;
}

float particleUpAngle(float directionX, float directionY) {
    const float length = sqrt(
        directionX * directionX + directionY * directionY);
    float angle = 0.0;
    if (length > 0.0) {
        const float dot = directionY;
        if (dot == 0.0) {
            angle = directionX > 0.0 ? 6.28318530718 : 3.14159265359;
        } else {
            const float theta = acos(clamp(dot / length, -1.0, 1.0));
            angle = (directionX > 0.0 ? theta : -theta) + 3.14159265359;
        }
    }
    return angle;
}

float alphaKeyValue(in GpuParticleState state, uint key) {
    return state.alphaKeyValues[key >> 2u][key & 3u];
}

uint alphaKeyFrame(in GpuParticleState state, uint key) {
    return state.alphaKeyFrames[key >> 2u][key & 3u];
}

uint colorKeyFrame(in GpuParticleState state, uint key) {
    return state.colorKeyFrames[key >> 2u][key & 3u];
}

float evaluateAlpha(in GpuParticleState state, float ageFrames) {
    float previousValue = alphaKeyValue(state, 0u);
    uint previousFrame = alphaKeyFrame(state, 0u);
    [loop]
    for (uint key = 1u; key < 8u; ++key) {
        const uint targetFrame = alphaKeyFrame(state, key);
        if (targetFrame == 0u) break;
        const float targetValue = alphaKeyValue(state, key);
        if (ageFrames < (float)targetFrame) {
            const uint frameSpan = targetFrame > previousFrame
                ? targetFrame - previousFrame : 0u;
            if (frameSpan == 0u) return targetValue;
            const float amount = saturate(
                (ageFrames - (float)previousFrame) / (float)frameSpan);
            return lerp(previousValue, targetValue, amount);
        }
        previousValue = targetValue;
        previousFrame = targetFrame;
    }
    return previousValue;
}

float3 evaluateColor(in GpuParticleState state, float ageFrames) {
    float3 previousValue = state.colorKeyValues[0].xyz;
    uint previousFrame = colorKeyFrame(state, 0u);
    [loop]
    for (uint key = 1u; key < 8u; ++key) {
        const uint targetFrame = colorKeyFrame(state, key);
        if (targetFrame == 0u) break;
        const float3 targetValue = state.colorKeyValues[key].xyz;
        if (ageFrames < (float)targetFrame) {
            const uint frameSpan = targetFrame > previousFrame
                ? targetFrame - previousFrame : 0u;
            if (frameSpan == 0u) return targetValue;
            const float amount = saturate(
                (ageFrames - (float)previousFrame) / (float)frameSpan);
            return lerp(previousValue, targetValue, amount);
        }
        previousValue = targetValue;
        previousFrame = targetFrame;
    }
    return previousValue;
}

bool hasFutureColorKey(in GpuParticleState state, float ageFrames) {
    [loop]
    for (uint key = 1u; key < 8u; ++key) {
        const uint frame = colorKeyFrame(state, key);
        if (frame == 0u) break;
        if ((float)frame > ageFrames) return true;
    }
    return false;
}

// First reference kernel. Birth admission remains CPU-owned; this stage only
// mutates already-admitted GPU state and never emits gameplay/readback data.
// CPU authority samples all authored alpha ranges once at birth.  This kernel
// performs only deterministic key interpolation and color-scale evolution.
[numthreads(64, 1, 1)]
void IntegrateCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint index = dispatchThreadId.x;
    if (index >= min(activeCount, particleCapacity)) return;

    GpuParticleState state = particleStates[index];
    if ((state.identityAndFlags.w & GPU_PARTICLE_STATE_ALIVE) == 0u) return;

    [loop]
    for (uint frame = 0u; frame < authoredFrames; ++frame) {
        state.previousAndLifetime.xyz = state.positionAndAge.xyz;
        state.velocityAndGravity.z += state.velocityAndGravity.w;
        state.velocityAndGravity.xyz *= state.driftAndVelocityDamping.w;
        state.positionAndAge.xyz +=
            state.velocityAndGravity.xyz + state.driftAndVelocityDamping.xyz;

        state.sizeDynamicsAndAngle.w += state.angularDynamicsAndAlpha.x;
        state.angularDynamicsAndAlpha.x *= state.angularDynamicsAndAlpha.y;
        if ((state.identityAndFlags.w &
             GPU_PARTICLE_STATE_UP_TOWARDS_EMITTER) != 0u) {
            const float2 direction = state.positionAndAge.xy -
                state.emitterOriginAndReserved.xy;
            if (direction.x != 0.0 || direction.y != 0.0) {
                state.sizeDynamicsAndAngle.w =
                    particleUpAngle(direction.x, direction.y);
            }
        }

        state.sizeDynamicsAndAngle.x += state.sizeDynamicsAndAngle.y;
        state.sizeDynamicsAndAngle.y *= state.sizeDynamicsAndAngle.z;
        state.positionAndAge.w += 1.0;

        state.angularDynamicsAndAlpha.w = saturate(
            evaluateAlpha(state, state.positionAndAge.w));
        const float3 baseColor = evaluateColor(
            state, state.positionAndAge.w);
        const float colorAdjustment =
            state.angularDynamicsAndAlpha.z * state.positionAndAge.w;
        state.colorAndWindRandomness.xyz = saturate(
            baseColor + colorAdjustment.xxx);

        const bool finiteLifetime = state.previousAndLifetime.w > 0.0;
        const bool lifetimeExpired = finiteLifetime &&
            state.positionAndAge.w >= state.previousAndLifetime.w;
        const bool alphaTest = (state.identityAndFlags.w &
            GPU_PARTICLE_STATE_ALPHA_TEST) != 0u;
        const bool additiveInvisible = !alphaTest &&
            !hasFutureColorKey(state, state.positionAndAge.w) &&
            all(state.colorAndWindRandomness.xyz < 0.01);
        if (lifetimeExpired || additiveInvisible) {
            state.identityAndFlags.w &= ~GPU_PARTICLE_STATE_ALIVE;
            break;
        }
    }
    particleStates[index] = state;
}

// Reset is intentionally a distinct dispatch from CompactAliveCS. There is
// no cross-group barrier inside a dispatch, so combining the reset and scan
// would let another group append before thread zero clears the counters.
[numthreads(1, 1, 1)]
void ResetAliveCompactCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    particleCounters.Store(0u, 0u); // total alive states scanned
    particleCounters.Store(4u, 0u); // alive states beyond output capacity
}

// Parallel append order is unspecified and must not become gameplay
// authority. For a sealed state snapshot the bounded list membership and
// counters are deterministic: every alive sparse slot is appended exactly
// once, and entries beyond aliveIndexCapacity increment overflow.
[numthreads(64, 1, 1)]
void CompactAliveCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint slotIndex = dispatchThreadId.x;
    if (slotIndex >= particleCapacity) return;
    const GpuParticleState state = particleStates[slotIndex];
    if ((state.identityAndFlags.w & GPU_PARTICLE_STATE_ALIVE) == 0u) return;

    uint outputIndex = 0u;
    particleCounters.InterlockedAdd(0u, 1u, outputIndex);
    if (outputIndex < aliveIndexCapacity) {
        aliveParticleIndices[outputIndex] = slotIndex;
    } else {
        particleCounters.InterlockedAdd(4u, 1u);
    }
}

[numthreads(1, 1, 1)]
void ResetVisibleCompactCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    particleCounters.Store(16u, 0u); // visible compatible states
    particleCounters.Store(20u, 0u); // visible states beyond output capacity
    particleCounters.Store(32u, 0u); // membership signature A
    particleCounters.Store(36u, 0u); // membership signature B
}

uint visibleSignatureA(uint slotIndex, uint generation) {
    uint value = slotIndex ^ (generation * 0x9e3779b9u);
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

uint visibleSignatureB(uint slotIndex, uint generation) {
    uint value = generation ^ (slotIndex * 0x85ebca6bu);
    value ^= value >> 15u;
    value *= 0xc2b2ae35u;
    value ^= value >> 13u;
    return value;
}

// This pass consumes the GPU-resident alive list rather than rescanning the
// sparse state array. It is still a shadow/reference list: material bins and
// indirect draw ownership are connected by later R9-13 stages.
[numthreads(64, 1, 1)]
void CompactVisibleCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint aliveOrdinal = dispatchThreadId.x;
    const uint totalAlive = particleCounters.Load(0u);
    const uint aliveOverflow = particleCounters.Load(4u);
    const uint aliveCount = min(
        totalAlive - min(totalAlive, aliveOverflow), aliveIndexCapacity);
    if (aliveOrdinal >= aliveCount) return;

    const uint slotIndex = aliveParticleIndices[aliveOrdinal];
    if (slotIndex >= particleCapacity) return;
    const GpuParticleState state = particleStates[slotIndex];
    if (visibilityAuthorityGenerations[slotIndex] == 0u ||
        visibilityAuthorityGenerations[slotIndex] !=
            state.authorityTokens.y) {
        return;
    }
    const float size = state.sizeDynamicsAndAngle.x;
    const bool finiteState = size == size &&
        abs(size) <= 3.402823466e+38 &&
        all(state.positionAndAge.xyz == state.positionAndAge.xyz) &&
        all(abs(state.positionAndAge.xyz) <= 3.402823466e+38) &&
        all(state.previousAndLifetime.xyz ==
            state.previousAndLifetime.xyz) &&
        all(abs(state.previousAndLifetime.xyz) <= 3.402823466e+38) &&
        all(state.colorAndWindRandomness.xyz ==
            state.colorAndWindRandomness.xyz) &&
        all(abs(state.colorAndWindRandomness.xyz) <= 3.402823466e+38) &&
        state.angularDynamicsAndAlpha.w == state.angularDynamicsAndAlpha.w &&
        abs(state.angularDynamicsAndAlpha.w) <= 3.402823466e+38;
    const bool alphaTest = (state.identityAndFlags.w &
        GPU_PARTICLE_STATE_ALPHA_TEST) != 0u;
    const bool colorVisible = alphaTest ||
        any(state.colorAndWindRandomness.xyz > 1.0e-5);
    if ((state.identityAndFlags.w & GPU_PARTICLE_STATE_ALIVE) == 0u ||
        !finiteState || size <= 0.0 || !colorVisible) {
        return;
    }

    uint ignored = 0u;
    particleCounters.InterlockedXor(
        32u, visibleSignatureA(slotIndex, state.authorityTokens.y), ignored);
    particleCounters.InterlockedXor(
        36u, visibleSignatureB(slotIndex, state.authorityTokens.y), ignored);

    uint outputIndex = 0u;
    particleCounters.InterlockedAdd(16u, 1u, outputIndex);
    if (outputIndex < visibleIndexCapacity) {
        visibleParticleIndices[outputIndex] = slotIndex;
    } else {
        particleCounters.InterlockedAdd(20u, 1u);
    }
}

[numthreads(64, 1, 1)]
void ResetMaterialBinsCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint binIndex = dispatchThreadId.x;
    if (binIndex < materialBinCount) {
        materialBinCounts[binIndex] = 0u;
        materialBinOffsets[binIndex] = 0u;
        materialBinCursors[binIndex] = 0u;
    }
    if (binIndex == 0u) {
        particleCounters.Store(24u, 0u);
        particleCounters.Store(28u, 0u);
    }
}

uint writtenVisibleCount() {
    const uint totalVisible = particleCounters.Load(16u);
    const uint visibleOverflow = particleCounters.Load(20u);
    return min(totalVisible - min(totalVisible, visibleOverflow),
               visibleIndexCapacity);
}

[numthreads(64, 1, 1)]
void CountMaterialBinsCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint visibleOrdinal = dispatchThreadId.x;
    if (visibleOrdinal >= writtenVisibleCount()) return;
    const uint slotIndex = visibleParticleIndices[visibleOrdinal];
    if (slotIndex >= particleCapacity) {
        particleCounters.InterlockedAdd(28u, 1u);
        return;
    }
    const uint templateId = particleStates[slotIndex].identityAndFlags.x;
    if (templateId >= templateMaterialMapCount) {
        particleCounters.InterlockedAdd(28u, 1u);
        return;
    }
    const uint binIndex = templateMaterialBins[templateId];
    if (binIndex >= materialBinCount) {
        particleCounters.InterlockedAdd(28u, 1u);
        return;
    }
    InterlockedAdd(materialBinCounts[binIndex], 1u);
    particleCounters.InterlockedAdd(24u, 1u);
}

// Bin count is bounded by the particle hard capacity. A serial prefix pass is
// deliberately used for the first implementation: catalog publication is
// infrequent and the visible count/scatter remain parallel. A hierarchical
// scan is only justified after R10 profiling proves this pass material.
[numthreads(1, 1, 1)]
void PrefixMaterialBinsCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint prefix = 0u;
    [loop]
    for (uint binIndex = 0u; binIndex < materialBinCount; ++binIndex) {
        materialBinOffsets[binIndex] = prefix;
        materialBinCursors[binIndex] = 0u;
        ParticleDrawIndexedArguments arguments;
        arguments.indexCountPerInstance = 6u;
        arguments.instanceCount = materialBinCounts[binIndex];
        arguments.startIndexLocation = 0u;
        arguments.baseVertexLocation = 0;
        arguments.startInstanceLocation = prefix;
        materialIndirectArgs[binIndex] = arguments;
        prefix += materialBinCounts[binIndex];
    }
}

[numthreads(64, 1, 1)]
void ScatterMaterialBinsCS(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint visibleOrdinal = dispatchThreadId.x;
    if (visibleOrdinal >= writtenVisibleCount()) return;
    const uint slotIndex = visibleParticleIndices[visibleOrdinal];
    if (slotIndex >= particleCapacity) return;
    const uint templateId = particleStates[slotIndex].identityAndFlags.x;
    if (templateId >= templateMaterialMapCount) return;
    const uint binIndex = templateMaterialBins[templateId];
    if (binIndex >= materialBinCount) return;
    uint localIndex = 0u;
    InterlockedAdd(materialBinCursors[binIndex], 1u, localIndex);
    const uint outputIndex = materialBinOffsets[binIndex] + localIndex;
    if (outputIndex < particleCapacity) {
        materialParticleIndices[outputIndex] = slotIndex;
    } else {
        particleCounters.InterlockedAdd(28u, 1u);
    }
}
