#pragma once

#include "ThingModuleRecipe.h"
#include "ThingModelRecipe.h"
#include "core/container/string_utils.h"
#include "game/base/DamageTypes.h"
#include "presentation/render/TrackMarksVisualSettings.h"
#include "math/fixed/q32_32.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace game {

enum class ObjectGeometryType : uint8_t {
    Sphere,
    Cylinder,
    Box,
};

struct ObjectGeometryTemplate {
    ObjectGeometryType type = ObjectGeometryType::Sphere;
    // ThingTemplate constructs GeometryInfo with isSmall=FALSE.  Templates
    // that omit GeometryIsSmall must therefore remain eligible for legacy
    // structure terrain flattening; defaulting this to true silently skipped
    // every such construction footprint.
    bool isSmall = false;
    math::q32_32 majorRadiusFixed{int32_t{1}};
    math::q32_32 minorRadiusFixed{int32_t{1}};
    math::q32_32 heightFixed{int32_t{1}};
    math::q32_32 boundingCircleRadiusFixed{int32_t{1}};
    math::q32_32 boundingSphereRadiusFixed{int32_t{1}};

};

struct ObjectGeometryAuthoringTemplate final : ObjectGeometryTemplate {
    float majorRadius = 1.0f;
    float minorRadius = 1.0f;
    float height = 1.0f;
    float boundingCircleRadius = 1.0f;
    float boundingSphereRadius = 1.0f;

    void normalize() noexcept {
        majorRadius = std::max(0.0f, majorRadius);
        minorRadius = std::max(0.0f, minorRadius);
        height = std::max(0.0f, height);
        switch (type) {
        case ObjectGeometryType::Sphere:
            minorRadius = majorRadius;
            height = majorRadius;
            boundingCircleRadius = majorRadius;
            boundingSphereRadius = majorRadius;
            break;
        case ObjectGeometryType::Cylinder:
            minorRadius = majorRadius;
            boundingCircleRadius = majorRadius;
            boundingSphereRadius = std::max(majorRadius, height * 0.5f);
            break;
        case ObjectGeometryType::Box:
            boundingCircleRadius = std::sqrt(majorRadius * majorRadius +
                                             minorRadius * minorRadius);
            boundingSphereRadius = std::sqrt(majorRadius * majorRadius +
                                             minorRadius * minorRadius +
                                             (height * 0.5f) * (height * 0.5f));
            break;
        }
        majorRadiusFixed = math::q32_32{majorRadius};
        minorRadiusFixed = math::q32_32{minorRadius};
        heightFixed = math::q32_32{height};
        boundingCircleRadiusFixed =
            math::q32_32{boundingCircleRadius};
        boundingSphereRadiusFixed =
            math::q32_32{boundingSphereRadius};
    }
};

// Bit-for-bit projection of RefCode's ShadowType. These are flags rather
// than a modern mutually-exclusive enum because authored INI can retain the
// dynamic/directional projection modifiers beside its primary shadow kind.
// The renderer still chooses exactly one primary path: VOLUME is geometry in
// the directional shadow pass; decal/projection kinds use the terrain
// projector; NONE reaches neither path.
enum class ThingShadowFlag : uint8_t {
    None = 0x00,
    Decal = 0x01,
    Volume = 0x02,
    Projection = 0x04,
    DynamicProjection = 0x08,
    DirectionalProjection = 0x10,
    AlphaDecal = 0x20,
    AdditiveDecal = 0x40,
};

[[nodiscard]] constexpr uint8_t thingShadowBit(ThingShadowFlag flag) noexcept {
    return static_cast<uint8_t>(flag);
}

struct ThingShadowTemplate final {
    uint8_t typeMask = thingShadowBit(ThingShadowFlag::None);
    container::String texture;
    // Complete world-space width/height, matching ShadowTypeInfo rather than
    // the half-extents used internally by the ground-projector quad.
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    // Object-local planar world offset.
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    [[nodiscard]] constexpr bool has(ThingShadowFlag flag) const noexcept {
        return (typeMask & thingShadowBit(flag)) != 0;
    }

    [[nodiscard]] constexpr bool castsDirectionalShadow() const noexcept {
        return has(ThingShadowFlag::Volume);
    }

    [[nodiscard]] constexpr bool usesGroundProjector() const noexcept {
        if (castsDirectionalShadow()) return false;
        constexpr uint8_t projectorKinds =
            thingShadowBit(ThingShadowFlag::Decal) |
            thingShadowBit(ThingShadowFlag::Projection) |
            thingShadowBit(ThingShadowFlag::AlphaDecal) |
            thingShadowBit(ThingShadowFlag::AdditiveDecal);
        return (typeMask & projectorKinds) != 0;
    }
};

// This is the immutable, typed counterpart of the old BodyModule class
// selection.  It does not recreate the virtual hierarchy: runtime behaviour
// is represented by data flags that HealthSystem can process in ObjectId
// order.  The uncommon body kinds remain explicit so later damage/Die module
// work does not need to reinterpret an opaque module-name string.
enum class ObjectBodyKind : uint8_t {
    Active,
    Structure,
    Immortal,
    Highlander,
    Inactive,
    Undead,
    HiveStructure,
};

struct ObjectBodyTemplate {
    // The reference Object base template supplies InactiveBody. Templates
    // without an explicit/inherited Body therefore remain non-destructible;
    // treating an omitted Body as an implicit ActiveBody would make scenery
    // and helper objects incorrectly killable.
    ObjectBodyKind kind = ObjectBodyKind::Inactive;
    // The authored ModuleTag remains a content identity/diagnostic key; it
    // is not used for virtual dispatch in the ECS runtime.
    container::String moduleTag;
    // Authoring data is milliseconds. The confirmed session converts it to
    // ticks using its explicit logic rate when subdual behaviour is added.
    uint32_t subdualDamageHealIntervalMilliseconds = 0;
    // UndeadBody intercepts its first ordinary death and starts this second
    // life. HiveStructureBody preserves its damage-routing masks for the
    // later Spawn/contain implementation.
    math::q32_32 maximumHealthFixed{int32_t{100}};
    math::q32_32 initialHealthFixed{int32_t{100}};
    math::q32_32 subdualDamageCapFixed{};
    math::q32_32 subdualDamageHealAmountFixed{};
    math::q32_32 undeadSecondLifeMaximumHealthFixed{int32_t{1}};
    uint64_t hivePropagateDamageTypesMask = 0;
    uint64_t hiveSwallowDamageTypesMask = 0;
    bool fromBodyModule = false;

    [[nodiscard]] constexpr bool acceptsDamage() const noexcept {
        return kind != ObjectBodyKind::Inactive;
    }
    [[nodiscard]] constexpr bool clampsToOneHealth() const noexcept {
        return kind == ObjectBodyKind::Immortal;
    }
    [[nodiscard]] constexpr bool onlyUnresistableCanKill() const noexcept {
        return kind == ObjectBodyKind::Highlander;
    }

};

struct ObjectBodyAuthoringTemplate final : ObjectBodyTemplate {
    float maximumHealth = 100.0f;
    float initialHealth = 100.0f;
    float subdualDamageCap = 0.0f;
    float subdualDamageHealAmount = 0.0f;
    float undeadSecondLifeMaximumHealth = 1.0f;

    void normalize() noexcept {
        // ActiveBody's constructor assigns the authored Real values directly;
        // in particular it does not truncate or clamp InitialHealth to
        // MaxHealth. Preserve that content semantics here. The runtime
        // HealthSystem owns the later clamp performed by a health change.
        if (!std::isfinite(maximumHealth) || maximumHealth < 0.0f) maximumHealth = 0.0f;
        if (!std::isfinite(initialHealth)) initialHealth = 0.0f;
        if (!std::isfinite(subdualDamageCap)) subdualDamageCap = 0.0f;
        if (!std::isfinite(subdualDamageHealAmount)) subdualDamageHealAmount = 0.0f;
        if (!std::isfinite(undeadSecondLifeMaximumHealth) || undeadSecondLifeMaximumHealth < 0.0f) {
            undeadSecondLifeMaximumHealth = 0.0f;
        }
        maximumHealthFixed = math::q32_32{maximumHealth};
        initialHealthFixed = math::q32_32{initialHealth};
        subdualDamageCapFixed = math::q32_32{subdualDamageCap};
        subdualDamageHealAmountFixed =
            math::q32_32{subdualDamageHealAmount};
        undeadSecondLifeMaximumHealthFixed =
            math::q32_32{undeadSecondLifeMaximumHealth};
    }
};

// AIUpdateModuleData stores locomotors in named slots (SET_NORMAL, SET_PANIC
// and so on), each of which may contain several templates selected by legal
// surface. Keep that authored structure instead of flattening `Locomotor =
// SET_NORMAL Foo` into one unusable string.
enum class LocomotorSetSlot : uint8_t {
    Normal,
    NormalUpgraded,
    Freefall,
    Wander,
    Panic,
    Taxiing,
    Supersonic,
    Sluggish,
};

struct LocomotorSetDefinition final {
    LocomotorSetSlot slot = LocomotorSetSlot::Normal;
    container::Vector<container::String> templates;
};

// Exact ThingTemplate::RadarPriority vocabulary. INVALID and NOT_ON_RADAR
// are distinct authored values even though neither produces a normal blip.
enum class ObjectRadarPriority : uint8_t {
    Invalid,
    NotOnRadar,
    Structure,
    Unit,
    LocalUnitOnly,
};

// Exact ThingSort.h editor buckets. They are immutable content metadata and
// never participate in a simulation tick, but retaining the typed value keeps
// world-builder/content tooling from having to reparse Object.ini.
enum class ObjectEditorSorting : uint8_t {
    None,
    Structure,
    Infantry,
    Vehicle,
    Shrubbery,
    MiscManMade,
    MiscNatural,
    Debris,
    System,
    Audio,
    Test,
    ForReview,
    Road,
    Waypoint,
};

// Exact legacy BuildableStatus vocabulary. The immutable value is authored
// by Object INI; a running GameSession may overlay it through map scripts
// without mutating process-global content.
enum class ObjectBuildabilityStatus : uint8_t {
    Yes = 0,
    IgnorePrerequisites = 1,
    No = 2,
    OnlyByAi = 3,
};

// ThingTemplate::BuildCompletion decides whether extra owned build
// facilities accelerate a product.  Keep the authored distinction in the
// frozen recipe; ProductionUpdate must not infer it from KindOf or exit type.
enum class ObjectBuildCompletion : uint8_t {
    AppearsAtRallyPoint,
    PlacedByPlayer,
};

struct ThingTemplate {
    container::String name;
    container::String displayName;
    ObjectEditorSorting editorSorting = ObjectEditorSorting::None;
    // Packed ARGB, matching the engine-wide UI/render color convention.
    // DisplayColor is editor metadata; it is not an ownership/team tint.
    uint32_t editorDisplayColor = 0xff646464u;
    bool forbidden = false;
    // Object inventory buttons use the contained object's authored cameo,
    // independently of any CommandButton template.
    container::String buttonImage;
    container::String selectedPortraitImage;
    // Names of Upgrade definitions, not image assets. Presentation resolves
    // each through UpgradeCatalog so one authored upgrade has one cameo.
    container::Array<container::String, 5> upgradeCameos{};
    // CommandSet is presentation/input content rather than an ECS behavior,
    // but scripts can replace individual slots before an object is selected.
    // Keep the authored reference on the immutable template so a running
    // session can resolve those overrides without consulting a mutable INI
    // store.
    container::String commandSet;

    // The reference ThingTemplate owns the ambient AudioEvent family rather
    // than leaving it on a Drawable. Keep the authored names in frozen
    // content so a script can resolve the current damage-state variant
    // without touching reloadable INI data or a legacy audio pointer.
    container::String soundAmbient;
    container::String soundAmbientDamaged;
    container::String soundAmbientReallyDamaged;
    container::String soundAmbientRubble;
    // ActiveBody publishes these one-shot cues at the confirmed health
    // transaction boundary. They remain authored AudioEvent names so the
    // simulation can emit value events without retaining audio resources.
    container::String soundOnDamaged;
    container::String soundOnReallyDamaged;
    container::String voiceFear;
    // EjectPilotDie publishes these two per-unit events from the confirmed
    // death transaction. They remain authored AudioEvent names; no audio
    // resource or device handle is retained by the immutable archetype.
    container::String voiceEject;
    container::String soundEject;

    // Per-unit voice/sound response family: the cues that make a unit audibly
    // acknowledge selection, orders and movement. RefCode keeps them in
    // ThingTemplate::m_audioarray[TTAUDIO_*] and binds them with
    // INI::parseDynamicAudioEventRTS (ThingTemplate.cpp:199-236).
    //
    // Scope is the thing to get right, and it was verified by scanning both
    // shipped trees separately rather than assumed. Counts below are
    // ZeroHour/Generals-1 occurrences at Object scope. Keys authored only
    // inside a UnitSpecificSounds block (VoiceCreate, TurretMoveStart,
    // TurretMoveLoop, UnderConstruction, VoiceCrush, TruckLandingSound,
    // TruckPowerslideSound, VoiceGetHealed, VoiceEnterHostile, VoiceUnload,
    // and the already-fixed VoiceEject/SoundEject) deliberately get NO field
    // here: they are reached through perUnitSound(). Declaring an
    // Object-scope field for one of those reproduces exactly the defect
    // VoiceEject/SoundEject had, where the field parsed but never bound.
    container::String voiceSelect;          // Object scope 621/166
    container::String voiceGroupSelect;     // Object scope  11/8
    container::String voiceMove;            // Object scope 387/112
    container::String voiceAttack;          // Object scope 381/108
    container::String voiceGuard;           // Object scope 295/77
    container::String voiceTaskComplete;    // Object scope  54/14
    container::String voiceAttackAir;       // Object scope  51/15
    container::String soundEnter;           // Object scope  65/11
    container::String soundExit;            // Object scope  61/11
    container::String soundStealthOn;       // Object scope  47/13
    container::String soundStealthOff;      // Object scope  47/13
    container::String soundFallingFromPlane;// Object scope   8/3
    container::String soundMoveLoop;        // Object scope   6/6
    // RefCode treats the damaged loop as a separate fallback from the
    // damaged start one-shot.  It is not a variant the audio backend may
    // derive from SoundMoveLoop at runtime.
    container::String soundMoveLoopDamaged;
    // Object::onVeterancyLevelChanged chooses one of these per-template cues
    // before the global UI feedback sound.  Keep the identities frozen with
    // the recipe rather than replacing them with one global presentation key.
    container::String soundPromotedVeteran;
    container::String soundPromotedElite;
    container::String soundPromotedHero;
    // Authored exactly once per tree, in Default/Object.ini's
    // DefaultThingTemplate, i.e. only as a `NoSound` placeholder. Parsed so
    // the key is not silently dropped, but no shipped unit relies on them.
    // Note VoiceCreated is a DIFFERENT key from the UnitSpecificSounds
    // VoiceCreate (320 ZH occurrences) that content actually uses.
    container::String voiceSelectElite;
    container::String voiceCreated;
    container::String voiceDefect;
    container::String voiceTaskUnable;
    container::String voiceMeetEnemy;
    container::String soundCreated;
    // Generals-1 only, and dead even there: these three keys appear 290/59/59
    // times in the Generals INI tree, ZERO times in Zero Hour, and RefCode's
    // Zero Hour source parses no field named SoundDie at all (Zero Hour moved
    // death audio onto the Die-module FXList path). They are parsed here so
    // Generals-1 content stops being silently dropped; a Zero Hour session
    // will simply never see a non-empty value.
    container::String soundDie;
    container::String soundDieFire;
    container::String soundDieToxin;
    // Authored at BOTH scopes, so a reader must fall back to
    // UnitSpecificSounds when the Object-scope field is empty. Object/USS
    // counts (ZH+Generals combined): SoundMoveStart 395/2,
    // SoundMoveStartDamaged 218/2, VoiceEnter 8/337, VoiceGarrison 7/179.
    // For VoiceEnter and VoiceGarrison the block form is overwhelmingly the
    // real one, so the fallback is the primary path rather than an edge case.
    container::String soundMoveStart;
    container::String soundMoveStartDamaged;
    container::String voiceEnter;
    container::String voiceGarrison;

    // Core
    ObjectBodyTemplate body;
    ObjectGeometryTemplate geometry;
    // Zero retains the legacy "use GameData.DefaultStructureRubbleHeight"
    // contract. ActiveBody changes only the Z extent when a structure first
    // enters RUBBLE; the authored XY footprint remains stable.
    math::q32_32 structureRubbleHeightFixed{};
    // Authored top-level IsBridge.  KindOf LANDMARK_BRIDGE covers stock
    // content, but old maps/mods may rely on this flag without that token.
    bool isBridge = false;
    // Local building-placement presentation values. PlacementViewAngle is
    // converted from authored degrees once; the two factory widths remain
    // world units and extend the original terrain bib along object-local +X.
    math::q32_32 placementViewAngleRadiansFixed{};
    math::q32_32 factoryExitWidthFixed{};
    math::q32_32 factoryExtraBibWidthFixed{};
    ThingShadowTemplate shadow;
    bool isPrerequisite = false;
    // Derived after the complete Object INI universe is loaded.  A template
    // is a build facility when another template names it in an Object
    // prerequisite alternative, or when it is a COMMANDCENTER.  This is
    // deliberately distinct from the authored IsPrerequisite flag above.
    bool isBuildFacility = false;
    // Each inner vector is one `Object = A B` alternative group from the
    // legacy Prerequisites block. Retained for final content derivation and
    // authoritative production-prerequisite admission.
    container::Vector<container::Vector<container::String>>
        prerequisiteObjectAlternatives;
    // Every Science entry in a Prerequisites block is required.  This is
    // separate from the CommandButton science list: both gates exist in the
    // legacy production path and authoritative admission must enforce both.
    container::Vector<container::String> prerequisiteSciences;
    math::q32_32 buildCostFixed{};
    uint32_t refundValue = 0;
    math::q32_32 buildTimeSeconds{};
    ObjectBuildCompletion buildCompletion =
        ObjectBuildCompletion::AppearsAtRallyPoint;
    // Legacy Energy uses a signed convention: positive values produce power;
    // negative values consume it. They remain immutable recipe values; the
    // per-player aggregate is maintained by ObjectEnergySystem.
    int32_t energyProduction = 0;
    // Only PowerPlantUpgrade/Overcharge may activate this additional positive
    // production. Store it now so those future modules never reparse INI.
    int32_t energyBonus = 0;
    // These three ranges are intentionally distinct in RefCode. VisionRange
    // is used by gameplay acquisition/detection. ShroudClearingRange owns the
    // allied looker footprint and inherits VisionRange only when its authored
    // sentinel remains -1. ShroudRevealToAllRange is a second, usually much
    // smaller footprint published to enemies/neutrals while the object is
    // visible. A zero default must not create sight for un-authored objects.
    math::q32_32 sightRangeFixed{};
    math::q32_32 shroudClearingRangeFixed{};
    math::q32_32 shroudRevealToAllRangeFixed{};
    uint8_t crusherLevel = 0;
    uint8_t crushableLevel = 0;
    // RefCode defaults this to false. It controls ExperienceTracker only;
    // production eligibility is a separate concern and must never reuse it.
    bool isTrainable = false;
    // Legacy AIGuard policy: an EnterGuard scans for enterable objects;
    // HijackGuard narrows that path to enemy objects which can be hijacked.
    bool enterGuard = false;
    bool hijackGuard = false;
    ObjectBuildabilityStatus buildability = ObjectBuildabilityStatus::Yes;
    // Direct legacy BuildVariations edges are intentionally not made
    // transitive. ThingTemplate::isEquivalentTo checks either side's list.
    container::Vector<container::String> buildVariations;
    // Empty for ordinary Objects. ObjectReskin stores the ultimate ordinary
    // source name so parent/reskin/sibling equivalence is O(1).
    container::String legacyReskinRootName;
    container::Array<int32_t, 4> experienceValue{};
    container::Array<int32_t, 4> experienceRequired{};
    bool isRebuildable = true;
    bool isSelectable = true;
    ObjectRadarPriority radarPriority = ObjectRadarPriority::Invalid;
    bool isAutoRappelable = false;
    // Number of transport slots consumed by this object. RefCode treats zero
    // as explicitly non-transportable; stock infantry normally authors one
    // while large vehicles consume several slots.
    uint32_t transportSlotCount = 0;
    // These are the non-Draw scalar fields the original ObjectReskin table
    // is allowed to change. They remain immutable recipe data until fence /
    // production-limit systems consume them.
    math::q32_32 fenceWidthFixed{};
    math::q32_32 fenceXOffsetFixed{};
    uint32_t maxSimultaneousOfType = 0;
    container::String maxSimultaneousLinkKey;

    // UnitSpecificSounds is an authored semantic-name table. Keeping the
    // resolved audio event names here lets simulation plans freeze the two
    // sticky-bomb cues without retaining an INI block or legacy template.
    container::TreeMap<container::String, container::String>
        unitSpecificSounds;

    // UnitSpecificFX mirrors the legacy per-unit semantic FX table.  Keep
    // the resolved FXList names in immutable recipe data so confirmed
    // gameplay producers (notably CombatDropKillFX) never retain an INI
    // block or query reloadable content.
    container::TreeMap<container::String, container::String>
        unitSpecificFx;

    // Armor
    container::String armorName;

    // Drawing
    container::String drawModule;
    container::String modelConditionState;
    // Canonical W3D prototype selected from the template's default draw
    // condition. This remains game-data metadata; rendering consumes it only
    // after GameRenderExtraction has copied it into a pure render snapshot.
    container::String defaultW3dModel;
    // Uniform authored Thing Scale is authoritative content. Keep it fixed
    // in the recipe and convert only at the detached render boundary.
    math::q32_32 assetScale{int32_t{1}};
    // World Builder bakes per-instance fuzz into map object scale. Runtime
    // keeps the authored value for tooling/content fidelity but must not roll
    // a new random scale when a map loads (W3DTreeDraw explicitly passes 0).
    math::q32_32 instanceScaleFuzziness{};
    container::Vector<ModelConditionVisualRule> modelConditionVisuals;
    container::Vector<ModelConditionTransitionRule> modelConditionTransitions;
    container::Vector<ModelDrawVisualChannel> drawVisualChannels;
    ModelConditionMask ignoredModelConditions;
    // One record per final Draw module which authored TrackMarks. Vehicle
    // subclasses inherit the field from W3DModelDraw, so compilation keys off
    // the authored property rather than an incomplete class-name whitelist.
    container::Vector<TrackMarksVisualDescriptor> trackMarksVisuals;

    // Movement
    // RefCode permits a LocomotorSet, not merely a single string.  The first
    // entry is retained in `locomotor` for old callers while systems select a
    // typed locomotor by legal surface from `locomotors`.
    container::String locomotor;
    container::Vector<container::String> locomotors;
    container::Vector<LocomotorSetDefinition> locomotorSets;

    // Behavior modules
    container::Vector<ModuleData> modules;

    // Weapons (from update module)
    container::Vector<container::String> weapons;

    // Faction / kind
    container::String kindOf;
    // Authored `Side`. RefCode calls this ThingTemplate::m_defaultOwningSide:
    // the side whose player naturally owns this template, used to pick the
    // command center that is naturally ours rather than captured
    // (Player.cpp:1211) and to match skirmish build-list entries and starting
    // units to a player (GameLogic.cpp:3488). Empty is a deliberate wildcard,
    // not "no side": RefCode accepts an unspecified side for any player.
    container::String defaultOwningSide;
    // Legacy alias of the field above. `FactionName` never appears in shipped
    // content; it is retained only so third-party data that used it keeps
    // resolving to one default owning side.
    container::String factionName;

    bool loaded = false;

    // RefCode ThingTemplate::getDefaultOwningSide() comparisons. The authored
    // spelling always matches PlayerTemplate's `Side`, but compare without
    // case so a mod cannot lose its starting base to a capitalization typo.
    [[nodiscard]] bool defaultOwningSideMatches(
        container::StringView side) const noexcept {
        if (defaultOwningSide.empty()) return true;
        return container::asciiEqualIgnoreCase(defaultOwningSide, side);
    }

    // RefCode ThingTemplate::getPerUnitSound(). The authored semantic name is
    // matched without case because UnitSpecificSounds keys are hand-written
    // per object and shipped content is not consistent about capitalization.
    // Returns an empty view when the object authored no such cue; callers
    // must treat that as "stay silent", never as a missing-asset error.
    [[nodiscard]] container::StringView perUnitSound(
        container::StringView semanticName) const noexcept {
        if (semanticName.empty()) return {};
        for (const auto& [authoredName, eventName] : unitSpecificSounds) {
            if (container::asciiEqualIgnoreCase(authoredName, semanticName))
                return eventName;
        }
        return {};
    }

    // Resolution order for the keys shipped content authors at both scopes.
    // The Object-scope field wins when present because that is the form
    // RefCode's INI::parseDynamicAudioEventRTS binds; UnitSpecificSounds is
    // the fallback so an object that only wrote the block form is still
    // audible.
    [[nodiscard]] container::StringView resolveUnitSound(
        const container::String& objectScopeValue,
        container::StringView semanticName) const noexcept {
        if (!objectScopeValue.empty())
            return container::StringView{objectScopeValue};
        return perUnitSound(semanticName);
    }

    // Named resolvers for the dual-scope keys, so no call site has to
    // remember which semantic spelling the block form uses.
    [[nodiscard]] container::StringView resolvedSoundMoveStart()
        const noexcept {
        return resolveUnitSound(soundMoveStart, "SoundMoveStart");
    }
    [[nodiscard]] container::StringView resolvedSoundMoveStartDamaged()
        const noexcept {
        return resolveUnitSound(
            soundMoveStartDamaged, "SoundMoveStartDamaged");
    }
    [[nodiscard]] container::StringView resolvedVoiceEnter() const noexcept {
        return resolveUnitSound(voiceEnter, "VoiceEnter");
    }
    [[nodiscard]] container::StringView resolvedVoiceGarrison()
        const noexcept {
        return resolveUnitSound(voiceGarrison, "VoiceGarrison");
    }
    // These are three deliberately separate production/create paths.  Do not
    // collapse them merely because their names are near-identical:
    //
    // * Object-scope VoiceCreated is ProductionUpdate's per-product cue;
    // * USS VoiceCreate is ProductionUpdate's first-product-in-batch cue;
    // * USS VoiceCreated is BuildAssistant's instant-created-unit cue.
    //
    // Their callers own the corresponding lifecycle/production provenance.
    // A resolver which falls back between them loses that provenance and
    // causes duplicate or missing acknowledgements for multi-unit builds.
    [[nodiscard]] container::StringView productionBatchVoiceCreate()
        const noexcept {
        return perUnitSound("VoiceCreate");
    }
    [[nodiscard]] container::StringView instantBuildVoiceCreated()
        const noexcept {
        return perUnitSound("VoiceCreated");
    }
};

// Reloadable Object.ini authoring record. These compatibility Reals exist
// only while ordinary/override/reskin streams are being merged. The factory
// slices the finalized value to ThingTemplate when publishing an archetype,
// so session/runtime code cannot access these top-level float mirrors.
struct ThingAuthoringTemplate final : ThingTemplate {
    ObjectBodyAuthoringTemplate body;
    ObjectGeometryAuthoringTemplate geometry;
    float maxHealth = 100.0f;
    float startingHealth = 100.0f;
    float structureRubbleHeight = 0.0f;
    float placementViewAngleRadians = 0.0f;
    float factoryExitWidth = 0.0f;
    float factoryExtraBibWidth = 0.0f;
    float buildCost = 0.0f;
    int buildTime = 0;
    float sight = 0.0f;
    float shroudClearingRange = -1.0f;
    float shroudRevealToAllRange = 0.0f;
    float crusherPriority = 0.0f;
    float fenceWidth = 0.0f;
    float fenceXOffset = 0.0f;
};

[[nodiscard]] bool legacyThingTemplatesEquivalent(
    const ThingTemplate& left, const ThingTemplate& right) noexcept;

} // namespace game
