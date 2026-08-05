#pragma once

#include "app/input/PendingWorldCursorState.h"
#include "engine/input/AuthoredCursorRuntime.h"

class InGameGuiSubsystem;

namespace app {
class PresentationCoordinator;

namespace runtime {
struct GameUiProjection;
}

namespace input {

// Main-thread owner for pending-target cursor presentation. It combines the
// authored SDL cursor and radius overlay without participating in command
// admission or pointer-capture state.
class PendingWorldCursorPresenter final {
public:
    void synchronize(
        const runtime::GameUiProjection& projection,
        InGameGuiSubsystem& inGameGui,
        PresentationCoordinator& presentation,
        bool forceAttack,
        bool waypointMode,
        bool worldCursorAllowed);
    void restore(InGameGuiSubsystem& inGameGui) noexcept;

private:
    engine::input::AuthoredCursorRuntime m_authoredCursors;
    PendingWorldCursorState m_cursor;
};

} // namespace input
} // namespace app
