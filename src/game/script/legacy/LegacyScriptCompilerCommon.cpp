#include "LegacyScriptCompilerInternal.h"
#include "LegacyScriptOpcodeNames.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>
#include <variant>

namespace engine::script::legacy::detail
{

// ScriptAction stores KIND_OF_PARAM as the Zero Hour KindOf ordinal. This
// order mirrors GeneralsMD KindOf.cpp with RTS_GENERALS enabled and the
// disabled ALLOW_SURRENDER entries omitted.
[[nodiscard]] std::optional<container::StringView>
legacyZeroHourKindOfName(int32_t ordinal) noexcept
{
    static constexpr container::StringView names[] = {
        "OBSTACLE", "SELECTABLE", "IMMOBILE", "CAN_ATTACK",
        "STICK_TO_TERRAIN_SLOPE", "CAN_CAST_REFLECTIONS", "SHRUBBERY",
        "STRUCTURE", "INFANTRY", "VEHICLE", "AIRCRAFT", "HUGE_VEHICLE",
        "DOZER", "HARVESTER", "COMMANDCENTER", "LINEBUILD", "SALVAGER",
        "WEAPON_SALVAGER", "TRANSPORT", "BRIDGE", "LANDMARK_BRIDGE",
        "BRIDGE_TOWER", "PROJECTILE", "PRELOAD", "NO_GARRISON",
        "WAVEGUIDE", "WAVE_EFFECT", "NO_COLLIDE", "REPAIR_PAD", "HEAL_PAD",
        "STEALTH_GARRISON", "CASH_GENERATOR", "AIRFIELD", "DRAWABLE_ONLY",
        "MP_COUNT_FOR_VICTORY", "REBUILD_HOLE", "SCORE", "SCORE_CREATE",
        "SCORE_DESTROY", "NO_HEAL_ICON", "CAN_RAPPEL", "PARACHUTABLE",
        "CAN_BE_REPULSED", "MOB_NEXUS", "IGNORED_IN_GUI", "CRATE",
        "CAPTURABLE", "CLEARED_BY_BUILD", "SMALL_MISSILE", "ALWAYS_VISIBLE",
        "UNATTACKABLE", "MINE", "CLEANUP_HAZARD", "PORTABLE_STRUCTURE",
        "ALWAYS_SELECTABLE", "ATTACK_NEEDS_LINE_OF_SIGHT",
        "WALK_ON_TOP_OF_WALL", "DEFENSIVE_WALL", "FS_POWER", "FS_FACTORY",
        "FS_BASE_DEFENSE", "FS_TECHNOLOGY", "AIRCRAFT_PATH_AROUND",
        "LOW_OVERLAPPABLE", "FORCEATTACKABLE", "AUTO_RALLYPOINT",
        "TECH_BUILDING", "POWERED", "PRODUCED_AT_HELIPAD", "DRONE",
        "CAN_SEE_THROUGH_STRUCTURE", "BALLISTIC_MISSILE", "CLICK_THROUGH",
        "SUPPLY_SOURCE_ON_PREVIEW", "PARACHUTE",
        "GARRISONABLE_UNTIL_DESTROYED", "BOAT", "IMMUNE_TO_CAPTURE", "HULK",
        "SHOW_PORTRAIT_WHEN_CONTROLLED", "SPAWNS_ARE_THE_WEAPONS",
        "CANNOT_BUILD_NEAR_SUPPLIES", "SUPPLY_SOURCE", "REVEAL_TO_ALL",
        "DISGUISER", "INERT", "HERO", "IGNORES_SELECT_ALL",
        "DONT_AUTO_CRUSH_INFANTRY", "CLIFF_JUMPER", "FS_SUPPLY_DROPZONE",
        "FS_SUPERWEAPON", "FS_BLACK_MARKET", "FS_SUPPLY_CENTER",
        "FS_STRATEGY_CENTER", "MONEY_HACKER", "ARMOR_SALVAGER",
        "REVEALS_ENEMY_PATHS", "BOOBY_TRAP", "FS_FAKE", "FS_INTERNET_CENTER",
        "BLAST_CRATER", "PROP", "OPTIMIZED_TREE", "FS_ADVANCED_TECH",
        "FS_BARRACKS", "FS_WARFACTORY", "FS_AIRFIELD", "AIRCRAFT_CARRIER",
        "NO_SELECT", "REJECT_UNMANNED", "CANNOT_RETALIATE",
        "TECH_BASE_DEFENSE", "EMP_HARDENED", "DEMOTRAP",
        "CONSERVATIVE_BUILDING", "IGNORE_DOCKING_BONES",
    };
    return ordinal >= 0 && static_cast<size_t>(ordinal) < std::size(names)
        ? std::optional<container::StringView>{names[ordinal]}
        : std::nullopt;
}


size_t ScriptNameHash::operator()(container::StringView value) const noexcept
{
    return std::hash<container::StringView>{}(value);
}

size_t ScriptNameHash::operator()(const container::String& value) const noexcept
{
    return (*this)(container::StringView{value});
}

bool ScriptNameEqual::operator()(container::StringView left,
                                 container::StringView right) const noexcept
{
    return left == right;
}

void CompileContext::diagnostic(LegacyScriptCompileDiagnosticSeverity severity,
                                container::String message,
                                LegacySourceRange source)
{
    result.diagnostics.push_back(
        {.severity = severity, .message = std::move(message), .source = source});
}

[[nodiscard]] container::String uppercaseAscii(container::StringView value)
{
    container::String result(value);
    for (char& character : result)
    {
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - ('a' - 'A'));
        }
    }
    return result;
}

// `<This Team>` / `<This Object>` are not ordinary serialized names. RefCode
// resolves them through the explicit stable-ID ScriptInvocationContext.  The
// raw source check remains useful for output-name fields, which must never be
// reinterpreted as a selector.
[[nodiscard]] bool isDynamicScriptContextSelector(container::StringView value) noexcept
{
    const container::String upper = uppercaseAscii(value);
    return upper == "<THIS TEAM>" || upper == "<THIS OBJECT>";
}

[[nodiscard]] ScriptObjectSelector objectSelector(container::String value)
{
    return uppercaseAscii(value) == "<THIS OBJECT>"
        ? ScriptObjectSelector::thisObject()
        : ScriptObjectSelector::named(std::move(value));
}

[[nodiscard]] ScriptTeamSelector teamSelector(container::String value)
{
    return uppercaseAscii(value) == "<THIS TEAM>"
        ? ScriptTeamSelector::thisTeam()
        : ScriptTeamSelector::scenarioTeam(std::move(value));
}

// Kept as a compatibility hook because every SIDE-bearing compiler branch
// calls rejectUnsupportedDynamicPlayerSelector(). Current-enemy resolution
// now belongs to the confirmed runtime/world bridge, so no authored player
// selector remains unsupported at compile time.
[[nodiscard]] bool isUnsupportedDynamicPlayerSelector(container::StringView value) noexcept
{
    static_cast<void>(value);
    return false;
}

bool rejectDynamicScriptContextSelector(CompileContext& context,
                                        container::StringView scriptName,
                                        container::StringView actionName,
                                        container::StringView selector,
                                        const LegacySourceRange& source)
{
    if (!isDynamicScriptContextSelector(selector)) return false;
    const container::String upperSelector = uppercaseAscii(selector);
    const container::String upperAction = uppercaseAscii(actionName);
    const bool persistentOrOutputObjectName =
        upperSelector == "<THIS OBJECT>" &&
        (upperAction == "UNIT_SPAWN_NAMED_LOCATION_ORIENTATION" ||
         upperAction == "CREATE_NAMED_ON_TEAM_AT_WAYPOINT" ||
         upperAction == "NAMED_SET_TOPPLE_DIRECTION");
    if (!persistentOrOutputObjectName)
        return false;
    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "script '" + container::String(scriptName) + "' action '" + container::String(actionName) +
            "' uses dynamic selector '" + container::String(selector) +
            "' in an output/persistent object-name field",
        source);
    return true;
}

bool rejectUnsupportedDynamicPlayerSelector(CompileContext& context,
                                            container::StringView scriptName,
                                            container::StringView instructionName,
                                            container::StringView selector,
                                            const LegacySourceRange& source)
{
    if (!isUnsupportedDynamicPlayerSelector(selector)) return false;
    static_cast<void>(context);
    static_cast<void>(scriptName);
    static_cast<void>(instructionName);
    static_cast<void>(selector);
    static_cast<void>(source);
    return false;
}

[[nodiscard]] std::optional<container::String> sidePlayerAlias(const LegacySideSource& side)
{
    // This is the original SidesList `playerName` dictionary field. Keep it
    // as an authored alias here; resolving it to a session PlayerId belongs to
    // Scenario application, not the immutable map compiler.
    for (const LegacyDictionaryEntry& property : side.properties)
    {
        if (uppercaseAscii(property.key.resolvedName) != "PLAYERNAME")
            continue;
        const auto* value = std::get_if<container::String>(&property.value);
        if (!value)
            return std::nullopt;
        // SidesList uses an empty playerName as its neutral-player sentinel
        // (see RefCode SidesList::addPlayerByTemplate). Preserve that
        // semantic rather than treating the ScriptList as context-less.
        if (value->empty())
            return container::String{"Neutral"};
        return *value;
    }
    return std::nullopt;
}

[[nodiscard]] const LegacyDictionaryEntry* findDictionaryProperty(
    const container::Vector<LegacyDictionaryEntry>& properties,
    container::StringView name)
{
    const container::String wanted = uppercaseAscii(name);
    auto found = std::find_if(
        properties.begin(), properties.end(),
        [&wanted](const LegacyDictionaryEntry& property)
        {
            return uppercaseAscii(property.key.resolvedName) == wanted;
        });
    return found == properties.end() ? nullptr : &*found;
}

[[nodiscard]] container::String teamStringProperty(
    const LegacyTeamSource& team, container::StringView name,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource)
{
    const LegacyDictionaryEntry* property =
        findDictionaryProperty(team.properties, name);
    if (!property)
        return {};
    if (const auto* value = std::get_if<container::String>(&property->value))
        return *value;
    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "legacy Team '" + container::String(teamName) + "' field '" +
            container::String(name) + "' is not an ASCII string; ignored",
        property->serialized.size != 0 ? property->serialized : fallbackSource);
    return {};
}

[[nodiscard]] float teamRealProperty(
    const LegacyTeamSource& team, container::StringView name,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource)
{
    const LegacyDictionaryEntry* property =
        findDictionaryProperty(team.properties, name);
    if (!property)
        return 0.0f;
    if (const auto* value = std::get_if<float>(&property->value))
        return *value;
    if (const auto* value = std::get_if<int32_t>(&property->value))
        return static_cast<float>(*value);
    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "legacy Team '" + container::String(teamName) + "' field '" +
            container::String(name) + "' is not a real value; using zero",
        property->serialized.size != 0 ? property->serialized : fallbackSource);
    return 0.0f;
}

[[nodiscard]] bool teamBoolProperty(
    const LegacyTeamSource& team, container::StringView name,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource)
{
    const LegacyDictionaryEntry* property =
        findDictionaryProperty(team.properties, name);
    if (!property) return false;
    if (const auto* value = std::get_if<bool>(&property->value))
        return *value;
    if (const auto* value = std::get_if<int32_t>(&property->value))
        return *value != 0;
    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "legacy Team '" + container::String(teamName) + "' field '" +
            container::String(name) + "' is not a Boolean; using false",
        property->serialized.size != 0 ? property->serialized : fallbackSource);
    return false;
}

[[nodiscard]] container::String instructionName(const LegacyScriptInstructionSource& instruction,
                                           bool action,
                                           CompileContext& context,
                                           container::StringView scriptName)
{
    // NameKeys are authoritative where present: map/mod data may intentionally
    // remap an old ordinal.  The complete RefCode ordinal table is only the
    // compatibility path for pre-v2 actions / pre-v4 conditions.
    const container::StringView fallback = action
        ? legacyScriptActionNameFromOrdinal(instruction.opcode)
        : legacyScriptConditionNameFromOrdinal(instruction.opcode);
    if (instruction.nameKey && !instruction.nameKey->resolvedName.empty())
    {
        const container::String resolved = uppercaseAscii(instruction.nameKey->resolvedName);
        if (!fallback.empty() && resolved != fallback)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Info,
                               "script '" + container::String(scriptName) + "' remapped legacy " +
                                   (action ? "action" : "condition") + " ordinal " +
                                   std::to_string(instruction.opcode) + " through NameKey '" +
                                   instruction.nameKey->resolvedName + "'",
                               instruction.serialized);
        }
        return resolved;
    }
    return container::String(fallback);
}

[[nodiscard]] const LegacyScriptParameter* parameterAt(const LegacyScriptInstructionSource& instruction,
                                                       size_t index,
                                                       CompileContext& context,
                                                       container::StringView scriptName,
                                                       container::StringView instructionLabel)
{
    if (index < instruction.parameters.size())
        return &instruction.parameters[index];
    context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                       "script '" + container::String(scriptName) + "' " + container::String(instructionLabel) +
                           " is missing parameter " + std::to_string(index),
                       instruction.serialized);
    return nullptr;
}

[[nodiscard]] std::optional<container::String> textParameter(const LegacyScriptInstructionSource& instruction,
                                                       size_t index,
                                                       CompileContext& context,
                                                       container::StringView scriptName,
                                                       container::StringView instructionLabel)
{
    const LegacyScriptParameter* parameter = parameterAt(instruction, index, context, scriptName, instructionLabel);
    if (!parameter || parameter->text.empty())
    {
        if (parameter)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' " + container::String(instructionLabel) +
                                   " has an empty string parameter " + std::to_string(index),
                               instruction.serialized);
        }
        return std::nullopt;
    }
    return parameter->text;
}

// The map reveal actions deliberately use an empty SIDE as an authored
// sentinel for "all human players".  Keep that distinct from a missing
// parameter: the generic text helper correctly rejects empty identifiers,
// whereas this helper preserves the legacy side-selector form.
[[nodiscard]] std::optional<container::String> textParameterAllowEmpty(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel)
{
    const LegacyScriptParameter* parameter =
        parameterAt(instruction, index, context, scriptName, instructionLabel);
    return parameter ? std::optional<container::String>{parameter->text} : std::nullopt;
}

[[nodiscard]] std::optional<int32_t> integerParameter(const LegacyScriptInstructionSource& instruction,
                                                      size_t index,
                                                      CompileContext& context,
                                                      container::StringView scriptName,
                                                      container::StringView instructionLabel)
{
    const LegacyScriptParameter* parameter = parameterAt(instruction, index, context, scriptName, instructionLabel);
    return parameter ? std::optional<int32_t>{parameter->integerValue} : std::nullopt;
}

[[nodiscard]] std::optional<float> realParameter(const LegacyScriptInstructionSource& instruction,
                                                 size_t index,
                                                 CompileContext& context,
                                                 container::StringView scriptName,
                                                 container::StringView instructionLabel)
{
    const LegacyScriptParameter* parameter = parameterAt(instruction, index, context, scriptName, instructionLabel);
    if (!parameter || !std::isfinite(parameter->realValue))
    {
        if (parameter)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' " + container::String(instructionLabel) +
                                   " has a non-finite real parameter " + std::to_string(index),
                               instruction.serialized);
        }
        return std::nullopt;
    }
    return parameter->realValue;
}

[[nodiscard]] std::optional<math::vec3> coordinateParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel)
{
    const LegacyScriptParameter* parameter =
        parameterAt(instruction, index, context, scriptName, instructionLabel);
    if (!parameter || !parameter->isCoordinate ||
        !std::isfinite(parameter->coordinate[0]) ||
        !std::isfinite(parameter->coordinate[1]) ||
        !std::isfinite(parameter->coordinate[2]))
    {
        if (parameter)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' " +
                                   container::String(instructionLabel) +
                                   " has an invalid Coord3D parameter " +
                                   std::to_string(index),
                               instruction.serialized);
        }
        return std::nullopt;
    }
    return math::vec3{parameter->coordinate[0], parameter->coordinate[1], parameter->coordinate[2]};
}

[[nodiscard]] std::optional<ScriptComparison> comparisonParameter(const LegacyScriptInstructionSource& instruction,
                                                                  size_t index,
                                                                  CompileContext& context,
                                                                  container::StringView scriptName,
                                                                  container::StringView instructionLabel)
{
    const std::optional<int32_t> value = integerParameter(instruction, index, context, scriptName, instructionLabel);
    if (!value || *value < 0 || *value > static_cast<int32_t>(ScriptComparison::NotEqual))
    {
        if (value)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' " + container::String(instructionLabel) +
                                   " has an invalid comparison value",
                               instruction.serialized);
        }
        return std::nullopt;
    }
    return static_cast<ScriptComparison>(*value);
}

[[nodiscard]] std::optional<ScriptPlayerRelationship> playerRelationshipParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel)
{
    const std::optional<int32_t> value = integerParameter(
        instruction, index, context, scriptName, instructionLabel);
    if (!value)
        return std::nullopt;
    // RefCode's Relationship enum is ENEMIES=0, NEUTRAL=1, ALLIES=2. It
    // must not be static_cast into modern PlayerRelationship, whose order is
    // different; the typed script enum intentionally preserves this wire map.
    switch (*value)
    {
    case 0: return ScriptPlayerRelationship::Enemies;
    case 1: return ScriptPlayerRelationship::Neutral;
    case 2: return ScriptPlayerRelationship::Allies;
    default:
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) + "' " +
                               container::String(instructionLabel) +
                               " has an invalid player relationship value",
                           instruction.serialized);
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ScriptScienceAvailability> scienceAvailabilityParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel)
{
    const std::optional<container::String> value = textParameter(
        instruction, index, context, scriptName, instructionLabel);
    if (!value)
        return std::nullopt;
    const container::String upper = uppercaseAscii(*value);
    if (upper == "AVAILABLE") return ScriptScienceAvailability::Available;
    if (upper == "DISABLED") return ScriptScienceAvailability::Disabled;
    if (upper == "HIDDEN") return ScriptScienceAvailability::Hidden;
    context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                       "script '" + container::String(scriptName) + "' " +
                           container::String(instructionLabel) +
                           " has an unknown science availability '" + *value + "'",
                       instruction.serialized);
    return std::nullopt;
}

[[nodiscard]] std::optional<uint8_t> teamAreaSurfacesParameter(
    const LegacyScriptInstructionSource& instruction, CompileContext& context,
    container::StringView scriptName, container::StringView instructionLabel)
{
    // Before condition chunk v2 the third SURFACES_ALLOWED parameter was not
    // serialized. RefCode supplies 3 (ground|air) while loading such maps.
    if (instruction.parameters.size() < 3 && instruction.sourceVersion < 2)
        return uint8_t{3};
    const std::optional<int32_t> value = integerParameter(
        instruction, 2, context, scriptName, instructionLabel);
    if (!value || *value < 1 || *value > 3)
    {
        if (value)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' " +
                                   container::String(instructionLabel) +
                                   " has an invalid SURFACES_ALLOWED value",
                               instruction.serialized);
        }
        return std::nullopt;
    }
    return static_cast<uint8_t>(*value);
}

[[nodiscard]] std::optional<int32_t> secondsToTicks(float seconds,
                                                    CompileContext& context,
                                                    container::StringView scriptName,
                                                    LegacySourceRange source)
{
    if (!std::isfinite(seconds) || seconds < 0.0f)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) + "' has an invalid seconds duration",
                           source);
        return std::nullopt;
    }
    const double raw =
        std::ceil(static_cast<double>(seconds) * static_cast<double>(context.options.logicFramesPerSecond));
    if (raw > static_cast<double>(std::numeric_limits<int32_t>::max()))
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) + "' duration exceeds ScriptRuntime counter range",
                           source);
        return std::nullopt;
    }
    return static_cast<int32_t>(raw);
}

// Script timer setters and adjustments accept signed seconds. In particular,
// RefCode negates SUB_FROM_MSEC_TIMER before applying its CEIL conversion.
// Camera durations deliberately keep using secondsToTicks() above because a
// negative transition duration is invalid there.
[[nodiscard]] std::optional<int32_t> signedSecondsToTicks(float seconds,
                                                           CompileContext& context,
                                                           container::StringView scriptName,
                                                           LegacySourceRange source)
{
    if (!std::isfinite(seconds))
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) + "' has a non-finite timer duration",
                           source);
        return std::nullopt;
    }
    // Keep the exact float-domain evaluation from ScriptEngine:
    // ceilf(ConvertDurationFromMsecsToFrames(seconds * 1000)). A promoted
    // double `seconds * fps` would round common authored durations one frame
    // differently before CEIL.
    const float milliseconds = seconds * 1000.0f;
    const float framesPerMillisecond =
        static_cast<float>(context.options.logicFramesPerSecond) / 1000.0f;
    const float frames = milliseconds * framesPerMillisecond;
    const float rounded = std::ceil(frames);
    constexpr float minimum = -2147483648.0f;
    constexpr float maximumExclusive = 2147483648.0f;
    if (!std::isfinite(rounded) || rounded < minimum || rounded >= maximumExclusive)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) +
                               "' timer duration exceeds ScriptRuntime counter range",
                           source);
        return std::nullopt;
    }
    return static_cast<int32_t>(rounded);
}

// SET_RANDOM_MSEC_TIMER uses GameLogicRandomValue, not its Real overload.
// The original C++ call therefore truncates the authored Real endpoints to
// Int before it consumes a random value. Model the conversion explicitly and
// reject only values outside the defined modern int32 conversion domain.
[[nodiscard]] std::optional<int32_t> randomTimerSecondsEndpoint(float seconds,
                                                                  CompileContext& context,
                                                                  container::StringView scriptName,
                                                                  LegacySourceRange source)
{
    constexpr float minimum = -2147483648.0f;
    constexpr float maximumExclusive = 2147483648.0f;
    if (!std::isfinite(seconds) || seconds < minimum || seconds >= maximumExclusive)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) +
                               "' random millisecond timer endpoint is outside legacy Int range",
                           source);
        return std::nullopt;
    }
    return static_cast<int32_t>(seconds);
}


// All legacy camera operations use seconds in map data, while the modern
// director advances only from confirmed simulation ticks. Convert the three
// fields together so each compiler branch carries the exact same rounding and
// validation contract.
[[nodiscard]] std::optional<LegacyCameraTiming> cameraTimingParameters(
    const LegacyScriptInstructionSource& instruction, size_t durationIndex,
    size_t easeInIndex, size_t easeOutIndex, CompileContext& context,
    container::StringView scriptName, container::StringView instructionLabel)
{
    // RefCode's legacy ScriptAction loader upgrades the old three-parameter
    // forms of these camera moves to the current five-parameter shape by
    // appending REAL 0.0 ease-in and ease-out values. Preserve that exact,
    // deterministic load-time compatibility without accepting other truncated
    // camera actions or masking malformed authored parameters.
    const bool legacyCameraMoveWithoutEase = instruction.parameters.size() == 3 &&
        easeInIndex == 3 && easeOutIndex == 4 &&
        (instructionLabel == "MOVE_CAMERA_TO" ||
         instructionLabel == "MOVE_CAMERA_ALONG_WAYPOINT_PATH");
    const auto duration = realParameter(instruction, durationIndex, context, scriptName,
                                        instructionLabel);
    const std::optional<float> easeIn = legacyCameraMoveWithoutEase
        ? std::optional<float>{0.0f}
        : realParameter(instruction, easeInIndex, context, scriptName, instructionLabel);
    const std::optional<float> easeOut = legacyCameraMoveWithoutEase
        ? std::optional<float>{0.0f}
        : realParameter(instruction, easeOutIndex, context, scriptName, instructionLabel);
    if (!duration || !easeIn || !easeOut)
        return std::nullopt;

    const auto durationTicks = secondsToTicks(*duration, context, scriptName,
                                              instruction.serialized);
    const auto easeInTicks = secondsToTicks(*easeIn, context, scriptName,
                                            instruction.serialized);
    const auto easeOutTicks = secondsToTicks(*easeOut, context, scriptName,
                                             instruction.serialized);
    if (!durationTicks || !easeInTicks || !easeOutTicks)
        return std::nullopt;
    return LegacyCameraTiming{
        .durationTicks = static_cast<uint32_t>(*durationTicks),
        .easeInTicks = static_cast<uint32_t>(*easeInTicks),
        .easeOutTicks = static_cast<uint32_t>(*easeOutTicks),
    };
}

[[nodiscard]] int32_t saturatedNegate(int32_t value) noexcept
{
    // Script counters have modern saturating arithmetic.  Preserve that
    // contract at the compiler boundary too: negating INT32_MIN directly is
    // undefined, while subtracting that legacy magnitude must simply become
    // the largest representable positive adjustment.
    return value == std::numeric_limits<int32_t>::min()
        ? std::numeric_limits<int32_t>::max()
        : -value;
}

[[nodiscard]] uint32_t delaySecondsToTicks(int32_t seconds, const LegacyScriptCompileOptions& options)
{
    if (seconds <= 0 || options.logicFramesPerSecond == 0)
        return 0;
    const uint64_t value = static_cast<uint64_t>(seconds) * static_cast<uint64_t>(options.logicFramesPerSecond);
    return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] const NamedScript* findScriptByName(const CompileContext& context, container::StringView name) noexcept
{
    const auto found = context.scriptNameIndex.find(name);
    return found == context.scriptNameIndex.end() ? nullptr : &context.scriptsBySourceOrder[found->second];
}

void setScriptRunnable(CompileContext& context, ScriptId id, bool runnable) noexcept
{
    const auto found = std::find_if(
        context.scriptsBySourceOrder.begin(),
        context.scriptsBySourceOrder.end(),
        [id](const NamedScript& script) { return script.id == id; });
    if (found != context.scriptsBySourceOrder.end())
        found->runnable = runnable;
}

[[nodiscard]] const NamedGroup* findGroupByName(const CompileContext& context, container::StringView name) noexcept
{
    const auto found = context.groupNameIndex.find(name);
    return found == context.groupNameIndex.end() ? nullptr
                                                 : &context.groupsBySourceOrder[found->second];
}

} // namespace engine::script::legacy::detail
