#pragma once

#include "core/container/container_types.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {
struct ThingTemplate;
namespace terrain { class TerrainLogic; }

struct ObjectWaveGuideRule final {
    uint32_t authoredOrder = 0;
    uint32_t waveDelayMilliseconds = 0;
    math::q32_32 ySize{};
    math::q32_32 linearWaveSpacing{};
    math::q32_32 waveBendMagnitude{};
    math::q32_32 waterVelocity{};
    math::q32_32 preferredHeight{};
    math::q32_32 shorelineEffectDistance{};
    math::q32_32 damageRadius{};
    math::q32_32 damageAmount{};
    math::q32_32 toppleForce{};
    int32_t randomSplashSoundFrequency = 0;
    math::q32_32 bridgeParticleAngleFudgeRadians{};
    container::String randomSplashSound;
    container::String bridgeParticle;
    container::String loopingSound;
    container::Vector<engine::LogicFixedVec3> localShapePoints;
};

struct ObjectWaveGuidePlan final {
    container::Vector<ObjectWaveGuideRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectWaveGuidePlan>
compileObjectWaveGuidePlan(const ThingTemplate& templateData);
} // namespace game
