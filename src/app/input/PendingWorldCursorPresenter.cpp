#include "app/input/PendingWorldCursorPresenter.h"

#include "app/host/PresentationCoordinator.h"
#include "app/runtime/GameUiProjection.h"
#include "app/ui/ingame/InGameGuiSubsystem.h"
#include "core/container/string_utils.h"
#include "core/math/wwmath/base/wwmath_core.h"
#include "game/selection/PendingWorldCommandMode.h"

#include <SDL3/SDL_mouse.h>

#include <cmath>
#include <limits>
#include <optional>

namespace app::input {
namespace {

[[nodiscard]] container::StringView defaultCursorName(
    engine::selection::PendingWorldCursorKind kind) noexcept {
    using Kind = engine::selection::PendingWorldCursorKind;
    switch (kind) {
    case Kind::AttackMove: return "AttackMove";
    // InGameUI uses FORCE_ATTACK_GROUND together with the GUARD_AREA ring.
    case Kind::Guard: return "ForceAttackGround";
    case Kind::SetRallyPoint: return "SetRallyPoint";
    case Kind::IntentionalContact: return "EnterFriendly";
    case Kind::SpecialPower:
    case Kind::FireWeapon:
    case Kind::CombatDrop: return "Target";
    case Kind::None: return {};
    }
    return {};
}

[[nodiscard]] uint32_t radiusColor(container::StringView type) noexcept {
    if (container::asciiEqualIgnoreCase(type, "FRIENDLY_SPECIALPOWER"))
        return 0xff00ff00u;
    if (container::asciiEqualIgnoreCase(type, "GUARD_AREA") ||
        container::asciiEqualIgnoreCase(type, "EMERGENCY_REPAIR") ||
        container::asciiEqualIgnoreCase(type, "SPYDRONE") ||
        container::asciiEqualIgnoreCase(type, "RADAR") ||
        container::asciiEqualIgnoreCase(type, "AMBULANCE") ||
        container::asciiEqualIgnoreCase(type, "CLEARMINES")) {
        return 0xfff0f0f0u;
    }
    if (container::asciiEqualIgnoreCase(type, "A10STRIKE") ||
        container::asciiEqualIgnoreCase(type, "ARTILLERYBARRAGE") ||
        container::asciiEqualIgnoreCase(type, "NAPALMSTRIKE") ||
        container::asciiEqualIgnoreCase(type, "CLUSTERMINES")) {
        return 0xffff9c00u;
    }
    return 0xffff2020u;
}

void synchronizeRadius(
    const engine::selection::PendingWorldCommandMode& mode,
    InGameGuiSubsystem& inGameGui,
    PresentationCoordinator& presentation) {
    if (mode.cursor.radiusWorld <= math::q32_32{}) {
        inGameGui.clearRadiusCursorPolyline();
        return;
    }
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    static_cast<void>(SDL_GetMouseState(&pointerX, &pointerY));
    std::optional<engine::render::RenderVector> center =
        presentation.radarWorldAt(pointerX, pointerY);
    if (!center) center = presentation.terrainWorldAt(pointerX, pointerY);
    if (!center) {
        inGameGui.clearRadiusCursorPolyline();
        return;
    }
    const float radius = mode.cursor.radiusWorld.to_float();
    if (!std::isfinite(radius) || radius <= 0.0f) {
        inGameGui.clearRadiusCursorPolyline();
        return;
    }
    container::Array<math::vec2, 48> points;
    size_t visiblePoints = 0;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (size_t index = 0; index < points.size(); ++index) {
        const float angle = math::TWO_PI * static_cast<float>(index) /
            static_cast<float>(points.size());
        const engine::render::RenderVector world = *center +
            engine::render::RenderVector{
                std::cos(angle) * radius,
                std::sin(angle) * radius,
                0.12f,
            };
        const std::optional<math::vec2> projected =
            presentation.projectWorldToVirtual(world);
        if (projected) {
            points[index] = *projected;
            ++visiblePoints;
        } else {
            points[index] = {nan, nan};
        }
    }
    if (visiblePoints < 2u) {
        inGameGui.clearRadiusCursorPolyline();
        return;
    }
    inGameGui.setRadiusCursorPolyline(
        points, radiusColor(mode.cursor.radiusResource));
}

} // namespace

void PendingWorldCursorPresenter::synchronize(
    const runtime::GameUiProjection& projection,
    InGameGuiSubsystem& inGameGui,
    PresentationCoordinator& presentation,
    bool forceAttack,
    bool waypointMode,
    bool worldCursorAllowed) {
    const auto& mode = projection.pendingWorldCommand;
    const auto& popup = projection.scriptUi.popup;
    const bool modalVisible = inGameGui.layer().hasOverlay() ||
        (popup.active && popup.stamp.presentationEpoch ==
            projection.scriptUi.presentationEpoch);
    if (modalVisible || !worldCursorAllowed || !mode.active()) {
        inGameGui.clearRadiusCursorPolyline();
        if (projection.hasSession) {
            container::StringView resource = "Normal";
            if (!modalVisible && worldCursorAllowed && !mode.active()) {
                if (waypointMode && projection.commandUi.hasSelection) {
                    resource = "Waypoint";
                } else if (forceAttack && projection.commandUi.hasSelection) {
                    resource = projection.hoveredObjectForceAttackableBySelection
                        ? container::StringView{"ForceAttackObj"}
                        : container::StringView{"ForceAttackGround"};
                } else {
                    switch (projection.worldCursorHint) {
                    case runtime::WorldCursorHint::Move:
                        resource = "Move";
                        break;
                    case runtime::WorldCursorHint::AttackObject:
                        resource = "AttackObj";
                        break;
                    case runtime::WorldCursorHint::EnterFriendly:
                        resource = "EnterFriendly";
                        break;
                    case runtime::WorldCursorHint::EnterAggressive:
                        resource = "EnterAggressive";
                        break;
                    case runtime::WorldCursorHint::GetHealed:
                        resource = "GetHealed";
                        break;
                    case runtime::WorldCursorHint::DoRepair:
                        resource = "DoRepair";
                        break;
                    case runtime::WorldCursorHint::ResumeConstruction:
                        resource = "ResumeConstruction";
                        break;
                    case runtime::WorldCursorHint::GetRepaired:
                        resource = "GetRepaired";
                        break;
                    case runtime::WorldCursorHint::Invalid:
                        resource = "GenericInvalid";
                        break;
                    case runtime::WorldCursorHint::Normal:
                        break;
                    }
                }
            }
            if (!m_cursor.matches(projection.sessionRevision, resource)) {
                m_cursor.apply(projection.sessionRevision, resource,
                               m_authoredCursors);
            }
        } else {
            m_cursor.restore();
        }
        return;
    }

    float pointerX = 0.0f;
    float pointerY = 0.0f;
    static_cast<void>(SDL_GetMouseState(&pointerX, &pointerY));
    const PresentedWorldInputTarget hit = presentation.worldInputTargetAt(
        pointerX, pointerY, mode.allowShrubberyTarget,
        mode.allowMineTarget, forceAttack);
    const bool objectHit = static_cast<bool>(hit.object);
    const bool validObject = objectHit &&
        projection.hoveredObject == hit.object &&
        projection.pendingHoveredObjectValid;
    bool validTarget = false;
    switch (mode.targetKind) {
    case engine::selection::PendingWorldTargetKind::Object:
        validTarget = validObject;
        break;
    case engine::selection::PendingWorldTargetKind::Position:
        validTarget = static_cast<bool>(hit.position);
        break;
    case engine::selection::PendingWorldTargetKind::ObjectOrPosition:
    case engine::selection::PendingWorldTargetKind::Contextual:
        // A hit object with a forbidden relation is not terrain fallback.
        validTarget = objectHit ? validObject
                                : static_cast<bool>(hit.position);
        break;
    case engine::selection::PendingWorldTargetKind::None:
        break;
    }
    synchronizeRadius(mode, inGameGui, presentation);
    container::StringView resource = validTarget
        ? container::StringView{mode.cursor.validResource}
        : container::StringView{mode.cursor.invalidResource};
    if (resource.empty()) {
        resource = validTarget
            ? defaultCursorName(mode.cursor.kind)
            : container::StringView{"GenericInvalid"};
    }
    if (!m_cursor.matches(mode.revision, resource)) {
        m_cursor.apply(mode.revision, resource, m_authoredCursors);
    }
}

void PendingWorldCursorPresenter::restore(
    InGameGuiSubsystem& inGameGui) noexcept {
    m_cursor.restore();
    inGameGui.clearRadiusCursorPolyline();
}

} // namespace app::input
