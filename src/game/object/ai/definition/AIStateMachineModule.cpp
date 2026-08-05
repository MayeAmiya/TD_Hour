#include "game/object/ai/definition/AIStateMachineModule.h"

namespace engine::ai
{

// Keeping one production translation unit forces the complete public module
// facade and every routed state family through the normal engine build. The
// simulation phase will be connected only after the real service adapters are
// available.
static_assert(AI_STATE_DESCRIPTORS.size() == AI_STATE_MACHINE_LEGACY_STATE_COUNT);

} // namespace engine::ai
