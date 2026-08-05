#pragma once

#include <cstdint>

#include "game/object/ai/contracts/AIStateCommands.h"

namespace engine::ai
{

enum class AIFacingFeedbackStatus : uint8_t
{
    None,
    Pending,
    Completed,
    TargetLost,
    Unsupported,
};

struct AIFacingFeedback final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId request;
    AIFacingFeedbackStatus status = AIFacingFeedbackStatus::None;
    uint64_t confirmedTick = 0;
    AIAsyncOrderIdentity orderIdentity;
};

// Ports are borrowed only for one executor call. They are never retained by
// the snapshot-friendly state data or runtime.
struct AIStateServicePorts final
{
    AIStateCommandBuffer* commands = nullptr;
    const AIFacingFeedback* facingFeedback = nullptr;
};

} // namespace engine::ai
