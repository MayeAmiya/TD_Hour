#include "game/object/ai/runtime/AIStateSoAMultiwaveExecutor.h"

namespace engine::ai
{

bool AIStateSoAMultiwaveInput::MoveChildScratch::aligned(size_t count) const noexcept
{
    return scheduled.size() == count &&
           moveTargetValid.size() == count &&
           resolvedMoveTarget.size() == count &&
           results.size() == count;
}

bool AIStateSoAMultiwaveInput::MoveChildScratch::empty() const noexcept
{
    return scheduled.empty() && moveTargetValid.empty() &&
           resolvedMoveTarget.empty() && results.empty();
}

AIMoveStateSoAKernelInput AIStateSoAMultiwaveInput::MoveChildScratch::bind(
    const AIMoveStateSoAKernelInput& base) const noexcept
{
    AIMoveStateSoAKernelInput child = base;
    child.activeState = AIStateId::MoveTo;
    child.childMode = true;
    child.scheduled = scheduled;
    child.moveTargetValid = moveTargetValid;
    child.resolvedMoveTarget = resolvedMoveTarget;
    child.results = results;
    return child;
}

} // namespace engine::ai
