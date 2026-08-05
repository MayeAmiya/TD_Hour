#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "CombatProfile.h"
#include "ObjectModuleCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/ContentDiagnostics.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include "ThingRecipeDetail.h"

namespace game::detail {

std::optional<ObjectGeometryType> parseGeometryType(container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    if (lower == "sphere") return ObjectGeometryType::Sphere;
    if (lower == "cylinder") return ObjectGeometryType::Cylinder;
    if (lower == "box") return ObjectGeometryType::Box;
    return std::nullopt;
}

std::optional<ObjectRadarPriority> parseRadarPriority(
    container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    if (lower == "invalid") return ObjectRadarPriority::Invalid;
    if (lower == "not_on_radar" || lower == "notonradar")
        return ObjectRadarPriority::NotOnRadar;
    if (lower == "structure") return ObjectRadarPriority::Structure;
    if (lower == "unit") return ObjectRadarPriority::Unit;
    if (lower == "local_unit_only" || lower == "localunitonly")
        return ObjectRadarPriority::LocalUnitOnly;
    return std::nullopt;
}

[[nodiscard]] std::optional<ObjectEditorSorting> parseEditorSorting(
    container::StringView value) {
    const container::String lower = lowerAscii(container::String(
        firstToken(value)));
    if (lower == "none") return ObjectEditorSorting::None;
    if (lower == "structure") return ObjectEditorSorting::Structure;
    if (lower == "infantry") return ObjectEditorSorting::Infantry;
    if (lower == "vehicle") return ObjectEditorSorting::Vehicle;
    if (lower == "shrubbery") return ObjectEditorSorting::Shrubbery;
    if (lower == "misc_man_made") return ObjectEditorSorting::MiscManMade;
    if (lower == "misc_natural") return ObjectEditorSorting::MiscNatural;
    if (lower == "debris") return ObjectEditorSorting::Debris;
    if (lower == "system") return ObjectEditorSorting::System;
    if (lower == "audio") return ObjectEditorSorting::Audio;
    if (lower == "test") return ObjectEditorSorting::Test;
    if (lower == "for_review") return ObjectEditorSorting::ForReview;
    if (lower == "road") return ObjectEditorSorting::Road;
    if (lower == "waypoint") return ObjectEditorSorting::Waypoint;
    return std::nullopt;
}

[[nodiscard]] std::optional<uint32_t> parseEditorDisplayColor(
    container::StringView value) {
    const container::Vector<container::StringView> tokens =
        splitWhitespace(value);
    if (tokens.size() < 3 || tokens.size() > 4) return std::nullopt;
    constexpr container::Array<char, 4> names{'r', 'g', 'b', 'a'};
    container::Array<uint32_t, 4> channels{0u, 0u, 0u, 255u};
    for (size_t index = 0; index < tokens.size(); ++index) {
        const container::StringView token = tokens[index];
        const size_t colon = token.find(':');
        if (colon != 1 || static_cast<char>(
                std::tolower(static_cast<unsigned char>(token.front()))) !=
                names[index]) {
            return std::nullopt;
        }
        const int32_t channel = parseSigned(token.substr(colon + 1));
        if (channel < 0 || channel > 255) return std::nullopt;
        channels[index] = static_cast<uint32_t>(channel);
    }
    return (channels[3] << 24u) | (channels[0] << 16u) |
           (channels[1] << 8u) | channels[2];
}

std::optional<ObjectBuildabilityStatus> parseBuildabilityStatus(
    container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    if (lower == "yes") return ObjectBuildabilityStatus::Yes;
    if (lower == "ignore_prerequisites" || lower == "ignoreprerequisites")
        return ObjectBuildabilityStatus::IgnorePrerequisites;
    if (lower == "no") return ObjectBuildabilityStatus::No;
    if (lower == "only_by_ai" || lower == "onlybyai")
        return ObjectBuildabilityStatus::OnlyByAi;
    return std::nullopt;
}

std::optional<ObjectBuildCompletion> parseBuildCompletion(
    container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    if (lower == "appears_at_rally_point" ||
        lower == "appearsatrallypoint") {
        return ObjectBuildCompletion::AppearsAtRallyPoint;
    }
    if (lower == "placed_by_player" || lower == "placedbyplayer")
        return ObjectBuildCompletion::PlacedByPlayer;
    return std::nullopt;
}

[[nodiscard]] std::optional<uint8_t> parseThingShadowMask(
    container::StringView value) {
    uint8_t mask = 0;
    bool foundToken = false;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n,|+", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n,|+", cursor);
        const container::String token = lowerAscii(container::String{
            value.substr(cursor, end == container::StringView::npos
                ? value.size() - cursor : end - cursor)});
        foundToken = true;
        if (token == "none" || token == "shadow_none") {
            mask = 0;
        } else if (token == "shadow_decal") {
            mask |= thingShadowBit(ThingShadowFlag::Decal);
        } else if (token == "shadow_volume") {
            mask |= thingShadowBit(ThingShadowFlag::Volume);
        } else if (token == "shadow_projection") {
            mask |= thingShadowBit(ThingShadowFlag::Projection);
        } else if (token == "shadow_dynamic_projection") {
            mask |= thingShadowBit(ThingShadowFlag::DynamicProjection);
        } else if (token == "shadow_directional_projection") {
            mask |= thingShadowBit(ThingShadowFlag::DirectionalProjection);
        } else if (token == "shadow_alpha_decal") {
            mask |= thingShadowBit(ThingShadowFlag::AlphaDecal);
        } else if (token == "shadow_additive_decal") {
            mask |= thingShadowBit(ThingShadowFlag::AdditiveDecal);
        } else {
            return std::nullopt;
        }
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return foundToken ? std::optional<uint8_t>{mask} : std::nullopt;
}

std::optional<LocomotorSetSlot> parseLocomotorSetSlot(container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    if (lower == "set_normal") return LocomotorSetSlot::Normal;
    if (lower == "set_normal_upgraded") return LocomotorSetSlot::NormalUpgraded;
    if (lower == "set_freefall") return LocomotorSetSlot::Freefall;
    if (lower == "set_wander") return LocomotorSetSlot::Wander;
    if (lower == "set_panic") return LocomotorSetSlot::Panic;
    if (lower == "set_taxiing") return LocomotorSetSlot::Taxiing;
    if (lower == "set_supersonic") return LocomotorSetSlot::Supersonic;
    if (lower == "set_sluggish") return LocomotorSetSlot::Sluggish;
    return std::nullopt;
}

container::Vector<container::StringView> splitWhitespace(container::StringView value) {
    container::Vector<container::StringView> tokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t", cursor);
        tokens.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return tokens;
}

void appendLocomotorBinding(ThingAuthoringTemplate& templateData, container::StringView value) {
    const container::Vector<container::StringView> tokens = splitWhitespace(value);
    if (tokens.empty()) return;
    LocomotorSetSlot slot = LocomotorSetSlot::Normal;
    size_t firstTemplate = 0;
    if (const std::optional<LocomotorSetSlot> parsedSlot = parseLocomotorSetSlot(tokens.front())) {
        slot = *parsedSlot;
        firstTemplate = 1;
    }
    if (firstTemplate >= tokens.size()) return;
    auto set = std::find_if(templateData.locomotorSets.begin(), templateData.locomotorSets.end(),
        [slot](const LocomotorSetDefinition& candidate) { return candidate.slot == slot; });
    if (set == templateData.locomotorSets.end()) {
        templateData.locomotorSets.push_back({.slot = slot});
        set = std::prev(templateData.locomotorSets.end());
    }
    for (size_t index = firstTemplate; index < tokens.size(); ++index) {
        if (tokens[index].empty()) continue;
        const container::String name(tokens[index]);
        set->templates.push_back(name);
        templateData.locomotors.push_back(name);
        if (templateData.locomotor.empty()) templateData.locomotor = name;
    }
}

std::optional<ObjectBodyKind> bodyKindFor(const ModuleData& module) {
    // Most actual Objects use `Body = ActiveBody ModuleTag_01`; some old
    // content writes the concrete body block directly.  In both forms, only
    // the first module-name token identifies the behaviour class.
    container::StringView className = module.moduleClass;
    if (className.empty()) className = module.type;
    if (module.moduleClass.empty() &&
        (module.type == "Body" || module.type == "Behavior" || module.type == "Module")) {
        className = firstToken(module.tag);
    }
    const container::String lower = lowerAscii(container::String(className));
    if (lower == "activebody") return ObjectBodyKind::Active;
    if (lower == "structurebody") return ObjectBodyKind::Structure;
    if (lower == "immortalbody") return ObjectBodyKind::Immortal;
    if (lower == "highlanderbody") return ObjectBodyKind::Highlander;
    if (lower == "inactivebody") return ObjectBodyKind::Inactive;
    if (lower == "undeadbody") return ObjectBodyKind::Undead;
    if (lower == "hivestructurebody") return ObjectBodyKind::HiveStructure;
    return std::nullopt;
}

void applyBodyModule(ObjectBodyAuthoringTemplate& body, ObjectBodyKind kind, const ModuleData& module) {
    // ActiveBodyModuleData initializes every health value to zero. An
    // explicit Body block therefore must never inherit the default template's
    // InactiveBody values (or the early loader's synthetic 100/100 fallback)
    // merely because a field was omitted in authored content. This reset is
    // also required by ObjectReskin/ReplaceModule-style recipe application.
    body.maximumHealth = 0.0f;
    body.initialHealth = 0.0f;
    body.subdualDamageCap = 0.0f;
    body.subdualDamageHealIntervalMilliseconds = 0;
    body.subdualDamageHealAmount = 0.0f;
    body.undeadSecondLifeMaximumHealth = 1.0f;
    body.hivePropagateDamageTypesMask = 0;
    body.hiveSwallowDamageTypesMask = 0;
    body.kind = kind;
    body.fromBodyModule = true;
    // `Body = ActiveBody ModuleTag_X` carries the class and tag in the
    // value, whereas the direct legacy form `ActiveBody ModuleTag_X` already
    // has the class in `module.type`.  Do not drop the first character/token
    // of a direct form's ModuleTag.
    body.moduleTag = !module.moduleTag.empty()
        ? module.moduleTag
        : (module.type == "Body" || module.type == "Behavior" || module.type == "Module"
            ? container::String(tokensAfterFirst(module.tag))
            : module.tag);
    if (const container::String* value = firstValue(module, "MaxHealth")) {
        body.maximumHealth = parseFloat(*value);
    }
    if (const container::String* value = firstValue(module, "InitialHealth")) {
        body.initialHealth = parseFloat(*value);
    }
    if (const container::String* value = firstValue(module, "SubdualDamageCap")) {
        body.subdualDamageCap = parseFloat(*value);
    }
    if (const container::String* value = firstValue(module, "SubdualDamageHealRate")) {
        body.subdualDamageHealIntervalMilliseconds = parseUnsigned(*value);
    }
    if (const container::String* value = firstValue(module, "SubdualDamageHealAmount")) {
        body.subdualDamageHealAmount = parseFloat(*value);
    }
    if (const container::String* value = firstValue(module, "SecondLifeMaxHealth")) {
        body.undeadSecondLifeMaximumHealth = parseFloat(*value);
    }
    if (const container::String* value = firstValue(module, "PropagateDamageTypesToSlavesWhenExisting")) {
        if (const std::optional<uint64_t> mask = parseDamageTypeMask(*value)) {
            body.hivePropagateDamageTypesMask = *mask;
        }
    }
    if (const container::String* value = firstValue(module, "SwallowDamageTypesIfSlavesNotExisting")) {
        if (const std::optional<uint64_t> mask = parseDamageTypeMask(*value)) {
            body.hiveSwallowDamageTypesMask = *mask;
        }
    }
    body.normalize();
}


[[nodiscard]] bool isReskinScalar(container::StringView key) noexcept {
    return key == "Geometry" || key == "GeometryMajorRadius" ||
           key == "GeometryMinorRadius" || key == "GeometryHeight" ||
           key == "GeometryIsSmall" || key == "FenceWidth" ||
           key == "FenceXOffset" || key == "MaxSimultaneousOfType" ||
           key == "MaxSimultaneousLinkKey";
}

void applyObjectField(ThingAuthoringTemplate& templateData, container::StringView key, container::StringView value,
                      TemplateRecipeParseState& state, bool reskin) {
    if (reskin && !isReskinScalar(key)) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "ObjectReskin cannot override field '" + container::String(key) + "'",
        });
        return;
    }

    if (key == "DisplayName") templateData.displayName = value;
    else if (key == "EditorSorting") {
        if (const auto parsed = parseEditorSorting(value)) {
            templateData.editorSorting = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "EditorSorting has unknown value '" +
                    container::String(value) + "'",
            });
        }
    }
    else if (key == "IsForbidden") templateData.forbidden = parseBool(value);
    else if (key == "DisplayColor") {
        if (const auto parsed = parseEditorDisplayColor(value)) {
            templateData.editorDisplayColor = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "DisplayColor requires R:0..255 G:0..255 "
                           "B:0..255 and optional A:0..255",
            });
        }
    }
    else if (key == "ButtonImage") templateData.buttonImage = value;
    else if (key == "SelectPortrait") templateData.selectedPortraitImage = value;
    else if (key == "UpgradeCameo1") templateData.upgradeCameos[0] = value;
    else if (key == "UpgradeCameo2") templateData.upgradeCameos[1] = value;
    else if (key == "UpgradeCameo3") templateData.upgradeCameos[2] = value;
    else if (key == "UpgradeCameo4") templateData.upgradeCameos[3] = value;
    else if (key == "UpgradeCameo5") templateData.upgradeCameos[4] = value;
    else if (key == "CommandSet") {
        // Shipped ZH content contains `CommandSet = = Name`.  The legacy INI
        // scanner treats the second '=' as harmless assignment punctuation,
        // not as part of the referenced command-set name.  Normalize that
        // punctuation once at the authored-data boundary so the frozen
        // simulation catalog contains the real stable name.
        value = container::trimAsciiView(value);
        while (!value.empty() && value.front() == '=') {
            value.remove_prefix(1);
            value = container::trimAsciiView(value);
        }
        templateData.commandSet = value;
    }
    else if (key == "SoundAmbient") templateData.soundAmbient = value;
    else if (key == "SoundAmbientDamaged") templateData.soundAmbientDamaged = value;
    else if (key == "SoundAmbientReallyDamaged") templateData.soundAmbientReallyDamaged = value;
    else if (key == "SoundAmbientRubble") templateData.soundAmbientRubble = value;
    else if (key == "SoundOnDamaged") templateData.soundOnDamaged = value;
    else if (key == "SoundOnReallyDamaged") templateData.soundOnReallyDamaged = value;
    else if (key == "VoiceFear") templateData.voiceFear = value;
    else if (key == "VoiceEject") templateData.voiceEject = value;
    else if (key == "SoundEject") templateData.soundEject = value;
    // Per-unit voice/sound response family. Each key here was confirmed to be
    // authored at Object scope in both shipped INI trees; the semantic names
    // that only ever appear inside a UnitSpecificSounds block are handled by
    // ThingTemplate::perUnitSound() and must NOT be added as branches here.
    else if (key == "VoiceSelect") templateData.voiceSelect = value;
    else if (key == "VoiceSelectElite") templateData.voiceSelectElite = value;
    else if (key == "VoiceGroupSelect") templateData.voiceGroupSelect = value;
    else if (key == "VoiceMove") templateData.voiceMove = value;
    else if (key == "VoiceAttack") templateData.voiceAttack = value;
    else if (key == "VoiceAttackAir") templateData.voiceAttackAir = value;
    else if (key == "VoiceGuard") templateData.voiceGuard = value;
    else if (key == "VoiceTaskComplete") templateData.voiceTaskComplete = value;
    else if (key == "VoiceTaskUnable") templateData.voiceTaskUnable = value;
    else if (key == "VoiceMeetEnemy") templateData.voiceMeetEnemy = value;
    else if (key == "VoiceCreated") templateData.voiceCreated = value;
    else if (key == "VoiceDefect") templateData.voiceDefect = value;
    else if (key == "SoundCreated") templateData.soundCreated = value;
    else if (key == "SoundDie") templateData.soundDie = value;
    else if (key == "SoundDieFire") templateData.soundDieFire = value;
    else if (key == "SoundDieToxin") templateData.soundDieToxin = value;
    else if (key == "SoundEnter") templateData.soundEnter = value;
    else if (key == "SoundExit") templateData.soundExit = value;
    else if (key == "SoundStealthOn") templateData.soundStealthOn = value;
    else if (key == "SoundStealthOff") templateData.soundStealthOff = value;
    else if (key == "SoundFallingFromPlane") {
        templateData.soundFallingFromPlane = value;
    }
    else if (key == "SoundMoveLoop") templateData.soundMoveLoop = value;
    else if (key == "SoundMoveLoopDamaged") {
        templateData.soundMoveLoopDamaged = value;
    }
    else if (key == "SoundPromotedVeteran") {
        templateData.soundPromotedVeteran = value;
    }
    else if (key == "SoundPromotedElite") {
        templateData.soundPromotedElite = value;
    }
    else if (key == "SoundPromotedHero") {
        templateData.soundPromotedHero = value;
    }
    // Dual-scope keys: readers use ThingTemplate::resolved*() so the
    // UnitSpecificSounds spelling still binds when the Object field is absent.
    else if (key == "SoundMoveStart") templateData.soundMoveStart = value;
    else if (key == "SoundMoveStartDamaged") {
        templateData.soundMoveStartDamaged = value;
    }
    else if (key == "VoiceEnter") templateData.voiceEnter = value;
    else if (key == "VoiceGarrison") templateData.voiceGarrison = value;
    else if (key == "MaxHealth") templateData.maxHealth = parseFloat(value);
    else if (key == "StartingHealth" || key == "InitialHealth") templateData.startingHealth = parseFloat(value);
    else if (key == "BuildCost") templateData.buildCost = parseFloat(value);
    else if (key == "RefundValue") templateData.refundValue = parseUnsigned(value);
    else if (key == "BuildTime") {
        const float seconds = std::max(0.0f, parseFloat(value));
        templateData.buildTime = static_cast<int>(seconds);
        templateData.buildTimeSeconds = math::q32_32{seconds};
    }
    else if (key == "EnergyProduction") templateData.energyProduction = parseSigned(value);
    else if (key == "EnergyBonus") templateData.energyBonus = parseSigned(value);
    else if (key == "Sight" || key == "VisionRange") templateData.sight = parseFloat(value);
    else if (key == "ShroudClearingRange") templateData.shroudClearingRange = parseFloat(value);
    else if (key == "ShroudRevealToAllRange") templateData.shroudRevealToAllRange = parseFloat(value);
    else if (key == "CrusherPriority") templateData.crusherPriority = parseFloat(value);
    else if (key == "CrusherLevel") templateData.crusherLevel = static_cast<uint8_t>(
        std::clamp(parseSigned(value), 0, 255));
    else if (key == "CrushableLevel") templateData.crushableLevel = static_cast<uint8_t>(
        std::clamp(parseSigned(value), 0, 255));
    else if (key == "IsTrainable") templateData.isTrainable = parseBool(value);
    else if (key == "EnterGuard") templateData.enterGuard = parseBool(value);
    else if (key == "HijackGuard") templateData.hijackGuard = parseBool(value);
    else if (key == "BuildCompletion") {
        if (const std::optional<ObjectBuildCompletion> parsed =
                parseBuildCompletion(value)) {
            templateData.buildCompletion = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "BuildCompletion has unknown value '" +
                    container::String(value) + "'",
            });
        }
    }
    else if (key == "Buildable") {
        if (const std::optional<ObjectBuildabilityStatus> parsed =
                parseBuildabilityStatus(value)) {
            templateData.buildability = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "Buildable has unknown value '" +
                    container::String(value) + "'",
            });
        }
    }
    else if (key == "BuildVariations") {
        templateData.buildVariations = parseNameList(value);
    }
    else if (key == "ExperienceValue") {
        if (const auto parsed = parseVeterancyIntList(value)) {
            templateData.experienceValue = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ExperienceValue requires exactly four signed integers",
            });
        }
    }
    else if (key == "ExperienceRequired") {
        if (const auto parsed = parseVeterancyIntList(value)) {
            templateData.experienceRequired = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ExperienceRequired requires exactly four signed integers",
            });
        }
    }
    else if (key == "IsRebuildable") templateData.isRebuildable = parseBool(value);
    else if (key == "IsSelectable") templateData.isSelectable = parseBool(value);
    else if (key == "RadarPriority") {
        if (const std::optional<ObjectRadarPriority> parsed =
                parseRadarPriority(value)) {
            templateData.radarPriority = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "RadarPriority has unknown value '" +
                    container::String(value) + "'",
            });
        }
    }
    else if (key == "IsAutoRappelable") templateData.isAutoRappelable = parseBool(value);
    else if (key == "TransportSlotCount") templateData.transportSlotCount = parseUnsigned(value);
    else if (key == "IsPrerequisite") templateData.isPrerequisite = parseBool(value);
    else if (key == "IsBridge") templateData.isBridge = parseBool(value);
    else if (key == "Armor") templateData.armorName = value;
    else if (key == "Locomotor") {
        // The field is a LocomotorSet, not an append-only list inherited
        // from DefaultThingTemplate. A new authored field replaces the
        // inherited set as RefCode's parser does.
        if (!state.authoredLocomotor) {
            templateData.locomotor.clear();
            templateData.locomotors.clear();
            templateData.locomotorSets.clear();
            state.authoredLocomotor = true;
        }
        appendLocomotorBinding(templateData, value);
    }
    else if (key == "Draw") templateData.drawModule = value;
    else if (key == "Scale") {
        templateData.assetScale = math::q32_32{parseFloat(value)};
    }
    else if (key == "InstanceScaleFuzziness") {
        templateData.instanceScaleFuzziness =
            math::q32_32{std::max(0.0f, parseFloat(value))};
    }
    else if (key == "ModelConditionState") templateData.modelConditionState = value;
    else if (key == "KindOf") {
        if (!applyLegacyBitMaskText(templateData.kindOf, value)) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "KindOf may not mix ordinary tokens with +/- edits on one line",
            });
        }
    }
    else if (key == "Side") templateData.defaultOwningSide = value;
    else if (key == "FactionName") {
        // `FactionName` is absent from all shipped content; `Side` is the
        // authored spelling. Keep the alias pointed at the same destination so
        // third-party data written against it still resolves one owning side.
        templateData.factionName = value;
        templateData.defaultOwningSide = value;
    }
    else if (key == "Geometry") {
        if (const std::optional<ObjectGeometryType> geometry = parseGeometryType(value)) {
            templateData.geometry.type = *geometry;
        }
    }
    else if (key == "GeometryMajorRadius") templateData.geometry.majorRadius = parseFloat(value);
    else if (key == "GeometryMinorRadius") templateData.geometry.minorRadius = parseFloat(value);
    else if (key == "GeometryHeight") templateData.geometry.height = parseFloat(value);
    else if (key == "GeometryIsSmall") templateData.geometry.isSmall = parseBool(value);
    else if (key == "StructureRubbleHeight") {
        templateData.structureRubbleHeight = std::max(0.0f, parseFloat(value));
    }
    else if (key == "PlacementViewAngle") {
        templateData.placementViewAngleRadians = parseFloat(value) *
            (3.14159265358979323846f / 180.0f);
    }
    else if (key == "FactoryExitWidth") {
        templateData.factoryExitWidth = parseFloat(value);
    }
    else if (key == "FactoryExtraBibWidth") {
        templateData.factoryExtraBibWidth = parseFloat(value);
    }
    else if (key == "Shadow") {
        if (const std::optional<uint8_t> parsed = parseThingShadowMask(value)) {
            templateData.shadow.typeMask = *parsed;
        } else {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "Shadow contains an unknown or empty shadow flag list",
            });
        }
    }
    else if (key == "ShadowSizeX") templateData.shadow.sizeX = parseFloat(value);
    else if (key == "ShadowSizeY") templateData.shadow.sizeY = parseFloat(value);
    else if (key == "ShadowOffsetX") templateData.shadow.offsetX = parseFloat(value);
    else if (key == "ShadowOffsetY") templateData.shadow.offsetY = parseFloat(value);
    else if (key == "ShadowTexture") templateData.shadow.texture = value;
    else if (key == "FenceWidth") templateData.fenceWidth = parseFloat(value);
    else if (key == "FenceXOffset") templateData.fenceXOffset = parseFloat(value);
    else if (key == "MaxSimultaneousOfType") templateData.maxSimultaneousOfType = parseUnsigned(value);
    else if (key == "MaxSimultaneousLinkKey") templateData.maxSimultaneousLinkKey = value;
    else if (key == "Weapon") templateData.weapons.emplace_back(value);
    else {
        // RefCode raises INI_UNKNOWN_TOKEN for an unmatched Object field.
        // Some shipped Object-scope keys are still unparsed here, so a hard failure
        // would refuse to load all authored content. Warn instead, and report
        // through the process collector rather than `state.diagnostics`: the
        // collector folds the several thousand authored occurrences into one
        // entry per key name, and the archetype's own diagnostic list stays
        // independent of recipe-compile scheduling.
        processContentDiagnostics().warn({
            .source = "data/ini/Object",
            .block = "Object",
            .module = "ThingFactory",
            .field = container::String(key),
            .adoptedValue = "field ignored",
            .reason = "Object field is not recognized by the object recipe "
                      "compiler; the authored value has no effect",
        });
    }
}

} // namespace game::detail
