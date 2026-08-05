#pragma once

#include "game/script/runtime/ScriptTypes.h"

namespace engine::script
{

enum class ScriptTargetKind : uint8_t
{
    Script,
    Group,
};

struct ScriptTarget final
{
    ScriptTargetKind kind = ScriptTargetKind::Script;
    ScriptId script = INVALID_SCRIPT_ID;
    ScriptGroupId group = INVALID_SCRIPT_GROUP_ID;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return kind == ScriptTargetKind::Script ? static_cast<bool>(script) : static_cast<bool>(group);
    }

    [[nodiscard]] static constexpr ScriptTarget scriptTarget(ScriptId id) noexcept
    {
        return {.kind = ScriptTargetKind::Script, .script = id};
    }

    [[nodiscard]] static constexpr ScriptTarget groupTarget(ScriptGroupId id) noexcept
    {
        return {.kind = ScriptTargetKind::Group, .group = id};
    }
};

// Immutable bindings for the lifecycle hooks authored on one legacy
// SidesList Team prototype.  Targets are resolved while the Program is built,
// so the runtime never performs string lookup when a Team event fires.  An
// invalid target represents an empty/NONE or rejected legacy field.
struct ScriptTeamHookDefinition final
{
    static constexpr size_t kGenericScriptCount = 16;

    container::String teamName;
    // TeamFactory::createInactiveTeam executes the Action side of
    // teamProductionCondition when teamExecutesActionsOnCreate is authored.
    // It is Script-only and deliberately bypasses that Script's conditions.
    ScriptTarget productionCreateActions;
    ScriptTarget onCreate;
    ScriptTarget onIdle;
    ScriptTarget onEnemySighted;
    ScriptTarget onAllClear;
    ScriptTarget onDestroyed;
    ScriptTarget onUnitDestroyed;
    // RefCode resolves these through findScriptByName() and duplicates the
    // Script per Team instance. They are Script-only targets; Group IDs are
    // never valid here. Slot position is authored and therefore retained.
    container::Array<ScriptTarget, kGenericScriptCount> genericScripts{};
    // Parsed as a legacy Real, then quantized once while ScriptProgram is
    // compiled. Team lifecycle code consumes only this frozen ratio.
    math::q32_32 destroyedThreshold{};
};

// Immutable binding for BuildListInfo::objectScriptAttachment.  It is keyed
// by authored Side/BuildList position because names are not unique.  The
// future Player/Skirmish BuildList completion producer will supply ThisObject
// and select this target; ScriptRuntime does not poll or infer construction.
struct ScriptObjectHookDefinition final
{
    uint32_t sourceSideOrdinal = INVALID_LEGACY_SIDE_ORDINAL;
    uint32_t sourceBuildListOrdinal = INVALID_LEGACY_BUILD_LIST_ORDINAL;
    container::String structureName;
    ScriptTarget onBuilt;
};

} // namespace engine::script
