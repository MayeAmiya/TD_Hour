#ifndef ENGINE_FX_GPU_PARTICLE_CONTRACT_HLSLI
#define ENGINE_FX_GPU_PARTICLE_CONTRACT_HLSLI

// One source defines both the C++ upload layout and the HLSL StructuredBuffer
// layout.  Keep all members as complete 16-byte vectors: this avoids float3
// packing ambiguity and makes every offset mechanically assertable in C++.
#define ENGINE_FX_GPU_PARTICLE_CONTRACT_VERSION 3

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace engine::fx::gpu_particle {

inline constexpr uint32_t kContractVersion =
    ENGINE_FX_GPU_PARTICLE_CONTRACT_VERSION;

enum StateFlag : uint32_t {
    StateAlive = 1u << 0u,
    StateGroundAligned = 1u << 1u,
    StateParticleUpTowardsEmitter = 1u << 2u,
    StateAlphaTest = 1u << 3u,
};

// GPU-owned integration/render state only.  CPU handles, emitter relations,
// priority chains and admission metadata are intentionally absent.  The two
// authority tokens are opaque diagnostics/reconciliation identities; shaders
// must never interpret them as gameplay state.
struct alignas(16) GpuParticleState final {
    float positionAndAge[4]{};                 // xyz, ageFrames
    float previousAndLifetime[4]{};            // xyz, lifetimeFrames
    float velocityAndGravity[4]{};             // xyz, gravityPerAuthoredFrame
    float driftAndVelocityDamping[4]{};        // xyz, velocityDamping
    float sizeDynamicsAndAngle[4]{};           // size, sizeRate, damping, angle
    float angularDynamicsAndAlpha[4]{};        // rate, damping, colorScale, alpha
    float colorAndWindRandomness[4]{};         // rgb, reserved for later wind
    float emitterOriginAndReserved[4]{};       // birth origin xyz, reserved
    float laterColorKeyTintAndReserved[4]{};   // rgb, reserved
    uint32_t identityAndFlags[4]{};             // template, seed lo/hi, StateFlag
    uint32_t authorityTokens[4]{};              // particle lo/hi, emitter lo/hi
    // Alpha ranges are sampled once by CPU authority with the retail random
    // streams.  The GPU must interpolate these values, never resample them.
    float alphaKeyValues[2][4]{};               // keys 0..3, 4..7
    uint32_t alphaKeyFrames[2][4]{};            // authored frames 0..7
    // RGB key zero is untouched; keys 1..7 already include the emitter's
    // later-key tint, matching ParticleRuntime::evaluateColor().
    float colorKeyValues[8][4]{};               // rgb, reserved
    uint32_t colorKeyFrames[2][4]{};            // authored frames 0..7
};

// CPU creates one command only after admission and relation handling.  A
// command initializes a selected GPU slot; it cannot create emitters, evict a
// CPU particle, spawn a child system, or acknowledge gameplay completion.
struct alignas(16) GpuParticleBirthCommand final {
    GpuParticleState initialState{};
    uint32_t destinationIndex = 0;
    uint32_t authorityEpoch = 0;
    uint32_t commandSequence = 0;
    uint32_t reserved = 0;
};

// CPU authority emits this when a compatible particle expires or is evicted
// before its authored lifetime. Generation matching prevents a stale retire
// from clearing a newer particle that reused the same stable slot.
struct alignas(16) GpuParticleRetireCommand final {
    uint32_t destinationIndex = 0;
    uint32_t particleGeneration = 0;
    uint32_t authorityEpoch = 0;
    uint32_t commandSequence = 0;
};

static_assert(std::is_standard_layout_v<GpuParticleState>);
static_assert(std::is_trivially_copyable_v<GpuParticleState>);
static_assert(alignof(GpuParticleState) == 16);
static_assert(sizeof(GpuParticleState) == 400);
static_assert(offsetof(GpuParticleState, positionAndAge) == 0);
static_assert(offsetof(GpuParticleState, previousAndLifetime) == 16);
static_assert(offsetof(GpuParticleState, velocityAndGravity) == 32);
static_assert(offsetof(GpuParticleState, driftAndVelocityDamping) == 48);
static_assert(offsetof(GpuParticleState, sizeDynamicsAndAngle) == 64);
static_assert(offsetof(GpuParticleState, angularDynamicsAndAlpha) == 80);
static_assert(offsetof(GpuParticleState, colorAndWindRandomness) == 96);
static_assert(offsetof(GpuParticleState, emitterOriginAndReserved) == 112);
static_assert(offsetof(GpuParticleState, laterColorKeyTintAndReserved) == 128);
static_assert(offsetof(GpuParticleState, identityAndFlags) == 144);
static_assert(offsetof(GpuParticleState, authorityTokens) == 160);
static_assert(offsetof(GpuParticleState, alphaKeyValues) == 176);
static_assert(offsetof(GpuParticleState, alphaKeyFrames) == 208);
static_assert(offsetof(GpuParticleState, colorKeyValues) == 240);
static_assert(offsetof(GpuParticleState, colorKeyFrames) == 368);

static_assert(std::is_standard_layout_v<GpuParticleBirthCommand>);
static_assert(std::is_trivially_copyable_v<GpuParticleBirthCommand>);
static_assert(alignof(GpuParticleBirthCommand) == 16);
static_assert(sizeof(GpuParticleBirthCommand) == 416);
static_assert(offsetof(GpuParticleBirthCommand, initialState) == 0);
static_assert(offsetof(GpuParticleBirthCommand, destinationIndex) == 400);
static_assert(offsetof(GpuParticleBirthCommand, authorityEpoch) == 404);
static_assert(offsetof(GpuParticleBirthCommand, commandSequence) == 408);
static_assert(offsetof(GpuParticleBirthCommand, reserved) == 412);
static_assert(std::is_standard_layout_v<GpuParticleRetireCommand>);
static_assert(std::is_trivially_copyable_v<GpuParticleRetireCommand>);
static_assert(alignof(GpuParticleRetireCommand) == 16);
static_assert(sizeof(GpuParticleRetireCommand) == 16);
static_assert(offsetof(GpuParticleRetireCommand, destinationIndex) == 0);
static_assert(offsetof(GpuParticleRetireCommand, particleGeneration) == 4);
static_assert(offsetof(GpuParticleRetireCommand, authorityEpoch) == 8);
static_assert(offsetof(GpuParticleRetireCommand, commandSequence) == 12);

} // namespace engine::fx::gpu_particle

#else

static const uint GPU_PARTICLE_CONTRACT_VERSION =
    ENGINE_FX_GPU_PARTICLE_CONTRACT_VERSION;
static const uint GPU_PARTICLE_STATE_ALIVE = 1u << 0u;
static const uint GPU_PARTICLE_STATE_GROUND_ALIGNED = 1u << 1u;
static const uint GPU_PARTICLE_STATE_UP_TOWARDS_EMITTER = 1u << 2u;
static const uint GPU_PARTICLE_STATE_ALPHA_TEST = 1u << 3u;

struct GpuParticleState
{
    float4 positionAndAge;
    float4 previousAndLifetime;
    float4 velocityAndGravity;
    float4 driftAndVelocityDamping;
    float4 sizeDynamicsAndAngle;
    float4 angularDynamicsAndAlpha;
    float4 colorAndWindRandomness;
    float4 emitterOriginAndReserved;
    float4 laterColorKeyTintAndReserved;
    uint4 identityAndFlags;
    uint4 authorityTokens;
    float4 alphaKeyValues[2];
    uint4 alphaKeyFrames[2];
    float4 colorKeyValues[8];
    uint4 colorKeyFrames[2];
};

struct GpuParticleBirthCommand
{
    GpuParticleState initialState;
    uint destinationIndex;
    uint authorityEpoch;
    uint commandSequence;
    uint reserved;
};

struct GpuParticleRetireCommand
{
    uint destinationIndex;
    uint particleGeneration;
    uint authorityEpoch;
    uint commandSequence;
};

#endif

#undef ENGINE_FX_GPU_PARTICLE_CONTRACT_VERSION
#endif
