#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "core/math/wwmath/base/wwmath.h"
#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>

namespace game {
enum class ObjectKindOf : uint8_t;
}

namespace engine {

class GameContentSnapshot;
class GameSessionAIDomain;
class ObjectLifecycle;
class ObjectOwnershipIndex;

namespace selection {

struct LocalSelectionObjectSnapshot final {
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    container::String type;
    // Authored GameText label copied from the immutable ThingTemplate. The
    // query layer never localizes it: the main-thread UI owns the active
    // StringTable/map-string layer and resolves this detached value there.
    container::String displayNameLabel;
    bool selectable = false;
    bool live = false;
    bool local = false;
    bool structure = false;
    // KINDOF_HEAL_PAD is a distinct Enter acknowledgement in CommandXlat:
    // it chooses VoiceGetHealed before the generic container/structure cases.
    // This copied fact keeps that client-only decision behind the read-only
    // selection port.
    bool healPad = false;
    bool carBomb = false;
};

// One authored per-unit speech cue, named the way RefCode's CommandXlat names
// them. Selection and order acknowledgement are per-client presentation, so
// these are resolved through this read-only port and never enter simulation
// state.
enum class LocalUnitVoiceCue : uint8_t {
    Select,
    GroupSelect,
    Move,
    MoveUpgraded,
    Attack,
    AttackAir,
    Guard,
    Supply,
    Repair,
    BuildResponse,
    WeaponPrimaryMode,
    WeaponSecondaryMode,
    WeaponTertiaryMode,
    Bombard,
    CombatDrop,
    HackInternet,
    Salvage,
    ClearBuilding,
    Subdue,
    Disarm,
    SnipePilot,
    Melee,
    FlameLocation,
    PoisonLocation,
    FireRocketPods,
    // Resolved through ThingTemplate's dual-scope helpers because shipped
    // content authors these inside UnitSpecificSounds far more often than at
    // Object scope.
    Enter,
    Garrison,
    // UnitSpecificSounds-only semantic names.
    EnterHostile,
    Crush,
    Unload,
    GetHealed,
};

// Read-only confirmed-world capability for local selection policy.  It copies
// every retained fact and never exposes ECS entities, component pointers,
// content archetypes, ownership storage or AI state to the caller.
class LocalSelectionQueryPort final {
public:
    LocalSelectionQueryPort(
        const ecs::registry& registry,
        const ObjectLifecycle& objects,
        const ObjectOwnershipIndex& ownership,
        const GameContentSnapshot& content,
        const GameSessionAIDomain& ai) noexcept
        : m_registry(&registry),
          m_objects(&objects),
          m_ownership(&ownership),
          m_content(&content),
          m_ai(&ai) {}

    [[nodiscard]] LocalSelectionObjectSnapshot inspect(
        PlayerId localPlayer, ObjectId object) const noexcept;
    // Copies out one authored AudioEvent name. Returning the resolved name by
    // value keeps the archetype, the ECS entity and every audio handle behind
    // this port, exactly as inspect() does for the rest of the facts. An
    // object that authored no such cue yields an empty string, which callers
    // must treat as "stay silent".
    [[nodiscard]] container::String voiceCue(
        ObjectId object, LocalUnitVoiceCue cue) const;
    [[nodiscard]] bool matchesType(
        ObjectId object, container::StringView sought) const noexcept;
    [[nodiscard]] container::Vector<ObjectId> allObjects() const;
    [[nodiscard]] bool hasKind(
        ObjectId object, ::game::ObjectKindOf sought) const noexcept;
    // Read-only local equivalent of the crusher-level branch in
    // Object::canCrushOrSquish.  It exists solely to choose the local
    // CommandXlat acknowledgement before a ForceMove is submitted; confirmed
    // collision and squish admission remain entirely in simulation.
    [[nodiscard]] bool canCrushTarget(
        ObjectId crusher, ObjectId target) const noexcept;
    [[nodiscard]] bool isIdleAiObject(ObjectId object) const noexcept;
    [[nodiscard]] std::optional<math::vec3> cameraTarget(
        ObjectId object) const noexcept;

private:
    const ecs::registry* m_registry = nullptr;
    const ObjectLifecycle* m_objects = nullptr;
    const ObjectOwnershipIndex* m_ownership = nullptr;
    const GameContentSnapshot* m_content = nullptr;
    const GameSessionAIDomain* m_ai = nullptr;
};

} // namespace selection
} // namespace engine
