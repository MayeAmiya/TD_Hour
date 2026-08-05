#pragma once

#include "game/object/ai/contracts/AIStateServices.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

enum class ObjectMovementEventKind : uint8_t {
    Started,
    Completed,
    Blocked,
};

struct ObjectMovementEvent final {
    ObjectMovementEventKind kind = ObjectMovementEventKind::Started;
    ObjectId object = INVALID_OBJECT_ID;
    math::q32_32 targetX{};
    math::q32_32 targetY{};
    math::q32_32 targetZ{};
    uint64_t confirmedTick = 0;
    uint8_t orderSource = 0xff;
    uint8_t systemPurpose = 0xff;
    // Movement audio is selected at the confirmed transition into Moving.
    // Do not let presentation re-read a later Body state: damage may be
    // applied later in the same frame or while the sound is already live.
    bool damagedAtStart = false;
};

struct ObjectAIMovementCommand final {
    ai::MovementCommand command;
    uint64_t pathRevision = 0;
    ai::AIMovementMode mode = ai::AIMovementMode::Normal;
    bool panicking = false;
};

} // namespace engine
