#include "../../../fx/runtime/GpuParticleContract.hlsli"

// Independent graphics contract for the future GPU particle draw path.
//
// Root signature wiring:
//   b0  GpuParticleBillboardConstants (vertex)
//   t0  StructuredBuffer<GpuParticleState>
//   t1  StructuredBuffer<uint> grouped slot indices
//   t2  material texture (pixel)
//   s0  material sampler (pixel)
//
// ExecuteIndirect supplies each bin prefix through StartInstanceLocation.
// D3D12 adds that value to SV_InstanceID, so the shader indexes the grouped
// list directly and must not add the prefix a second time.

cbuffer GpuParticleBillboardConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    uint particleCapacity;
    float interpolationAlpha;
    uint2 reserved;
    float2 playableMinimum;
    float2 playableMaximum;
    uint playableBoundsEnabled;
    uint3 playablePadding;
};

StructuredBuffer<GpuParticleState> particleStates : register(t0);
StructuredBuffer<uint> groupedSlotIndices : register(t1);
Texture2D sourceTexture : register(t2);
SamplerState sourceSampler : register(s0);

struct GpuParticleBillboardVertexInput
{
    float2 corner : POSITION;
    float2 uv : TEXCOORD0;
    uint instanceId : SV_InstanceID;
};

struct GpuParticleBillboardVertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 worldPosition : POSITION0;
};

GpuParticleBillboardVertexOutput GpuParticleBillboardVS(
    GpuParticleBillboardVertexInput input)
{
    GpuParticleBillboardVertexOutput output;
    const uint groupedIndex = input.instanceId;
    if (groupedIndex >= particleCapacity)
    {
        output.position = float4(0.0, 0.0, 0.0, 1.0);
        output.uv = input.uv;
        output.color = 0.0;
        output.worldPosition = 0.0;
        return output;
    }

    const uint stateIndex = groupedSlotIndices[groupedIndex];
    if (stateIndex >= particleCapacity)
    {
        output.position = float4(0.0, 0.0, 0.0, 1.0);
        output.uv = input.uv;
        output.color = 0.0;
        output.worldPosition = 0.0;
        return output;
    }

    const GpuParticleState state = particleStates[stateIndex];
    const float cosineAngle = cos(state.sizeDynamicsAndAngle.w);
    const float sineAngle = sin(state.sizeDynamicsAndAngle.w);
    const float2 rotated = float2(
        input.corner.x * cosineAngle - input.corner.y * sineAngle,
        input.corner.x * sineAngle + input.corner.y * cosineAngle) *
        state.sizeDynamicsAndAngle.x;
    const bool groundAligned = (state.identityAndFlags.w &
        GPU_PARTICLE_STATE_GROUND_ALIGNED) != 0u;
    const float3 worldOffset = groundAligned
        ? float3(rotated.x, rotated.y, 0.0)
        : cameraRight.xyz * rotated.x + cameraUp.xyz * rotated.y;

    const float3 center = lerp(
        state.previousAndLifetime.xyz, state.positionAndAge.xyz,
        saturate(interpolationAlpha));
    const float3 worldPosition = center + worldOffset;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.uv = input.uv;
    output.color = float4(
        saturate(state.colorAndWindRandomness.xyz),
        saturate(state.angularDynamicsAndAlpha.w));
    output.worldPosition = worldPosition;
    return output;
}

void clipToPlayableBounds(float3 worldPosition)
{
    if (playableBoundsEnabled != 0u &&
        (any(worldPosition.xy < playableMinimum) ||
         any(worldPosition.xy > playableMaximum)))
    {
        clip(-1.0);
    }
}

float4 GpuParticleBillboardPSAdditive(
    GpuParticleBillboardVertexOutput input) : SV_TARGET
{
    clipToPlayableBounds(input.worldPosition);
    return sourceTexture.Sample(sourceSampler, input.uv) * input.color;
}

float4 GpuParticleBillboardPSAlphaTest(
    GpuParticleBillboardVertexOutput input) : SV_TARGET
{
    clipToPlayableBounds(input.worldPosition);
    const float4 sampleColor = sourceTexture.Sample(sourceSampler, input.uv);
    const float4 modulatedColor = sampleColor * input.color;
    clip(modulatedColor.a - (96.0 / 255.0));
    return modulatedColor;
}
