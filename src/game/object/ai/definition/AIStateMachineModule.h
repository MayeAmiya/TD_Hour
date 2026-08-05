#pragma once

// Production entry point for the object AI state-machine module. Consumers
// should include this facade instead of depending on the directory layout of
// individual state families.
#include "game/object/ai/runtime/AIStateSoAMultiwaveExecutor.h"
#include "game/object/ai/runtime/AIStateSoAParity.h"
#include "game/object/ai/runtime/AIStateSoASlotRegistry.h"
#include "game/object/ai/runtime/AIStateSoASnapshot.h"
#include "game/object/ai/runtime/AIProductionStateRoute.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

inline constexpr uint16_t AI_STATE_MACHINE_LEGACY_STATE_COUNT =
    static_cast<uint16_t>(AIStateId::Count);

static_assert(AI_STATE_MACHINE_LEGACY_STATE_COUNT == 44);

} // namespace engine::ai
