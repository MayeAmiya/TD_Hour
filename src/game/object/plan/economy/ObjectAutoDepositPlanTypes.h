#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

namespace game {

struct ThingTemplate;

struct ObjectAutoDepositBoost final {
    container::String upgrade;
    int32_t amount = 0;
};

// Immutable projection of one final AutoDepositUpdate declaration. Durations
// remain authored milliseconds and are converted once when the ECS runtime is
// assembled, preserving INI::parseDurationUnsignedInt's round-up rule.
struct ObjectAutoDepositParameters final {
    uint32_t authoredOrder = 0;
    uint32_t depositTimingMilliseconds = 0;
    int32_t depositAmount = 0;
    int32_t initialCaptureBonus = 0;
    bool actualMoney = true;
    container::Vector<ObjectAutoDepositBoost> upgradedBoosts;
};

struct ObjectAutoDepositPlan final {
    container::Vector<ObjectAutoDepositParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectAutoDepositPlan>
compileObjectAutoDepositPlan(const ThingTemplate& templateData);

} // namespace game

