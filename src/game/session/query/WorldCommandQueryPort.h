#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>

namespace engine {

class ObjectLifecycle;
class ObjectOwnershipIndex;
class ObjectSimulationContainmentDomain;
class PlayerRegistry;
class GameContentSnapshot;
namespace ai {
class ObjectAIRuntime;
}

namespace selection {

enum class PendingWorldTargetRelation : uint8_t;

enum class WorldContextTargetAction : uint8_t {
    Ground,
    Attack,
    Enter,
    Reserved,
};

// Presentation-only classification of the local acknowledgement selected by
// RefCode CommandXlat for a weapon order. It carries no template pointer
// across the query boundary and never enters the command payload.
enum class WorldCommandWeaponVoiceKind : uint8_t {
    None,
    ClearBuilding,
    Subdue,
    Disarm,
    SnipePilot,
    Melee,
    FlameLocation,
    PoisonLocation,
    FireRocketPods,
};

// Read-only confirmed-world capability used while compiling local pointer
// gestures into deterministic commands.  ECS, ownership and player storage
// remain private; callers receive only semantic decisions or copied IDs.
class WorldCommandQueryPort final {
public:
    WorldCommandQueryPort(
        const ecs::registry& registry,
        const PlayerRegistry& players,
        const ObjectOwnershipIndex& ownership,
        const ObjectLifecycle& objects,
        const ObjectSimulationContainmentDomain& containment,
        const GameContentSnapshot& content,
        const ai::ObjectAIRuntime& objectAI) noexcept
        : m_registry(&registry),
          m_players(&players),
          m_ownership(&ownership),
          m_objects(&objects),
          m_containment(&containment),
          m_content(&content),
          m_objectAI(&objectAI) {}

    [[nodiscard]] bool isCommandPlayer(PlayerId player) const noexcept;
    [[nodiscard]] bool isControlledLiveObject(
        PlayerId player, ObjectId object) const noexcept;
    [[nodiscard]] bool relationAllowed(
        PendingWorldTargetRelation allowed,
        PlayerId localPlayer, ObjectId target) const noexcept;
    [[nodiscard]] WorldContextTargetAction contextualTarget(
        PlayerId localPlayer, ObjectId target) const noexcept;
    // KINDOF_UNATTACKABLE victims are rejected by RefCode
    // WeaponSet::getAbleToAttackSpecificObject before its forced-attack
    // exception, so neither a contextual click nor Ctrl+force-fire may compile
    // an Attack against them. The confirmed simulation enforces the same rule;
    // this query only keeps the local command from being authored at all.
    [[nodiscard]] bool isAttackableTarget(ObjectId target) const noexcept;
    [[nodiscard]] bool isAttackableTargetForPlayer(
        PlayerId player, ObjectId target) const noexcept;
    [[nodiscard]] bool actorCanMove(ObjectId actor) const noexcept;
    // Uses the exact confirmed containment admission predicate.  Contextual
    // targeting is actor-dependent: an infantryman may enter a hostile
    // garrison while a tank must retain its normal attack context.
    [[nodiscard]] bool actorCanEnterContainer(
        ObjectId actor, ObjectId container) const noexcept;
    [[nodiscard]] bool actorCanAttack(ObjectId actor) const noexcept;
    [[nodiscard]] bool actorCanAttackTarget(
        ObjectId actor, ObjectId target) const noexcept;
    // Ctrl+force-fire bypasses ownership and alliance, but retains RefCode's
    // hard victim gates and still requires a compatible weapon consumer.
    [[nodiscard]] bool actorCanForceAttackTarget(
        ObjectId actor, ObjectId target) const noexcept;
    // Mirrors CommandXlat's DamageType/WeaponSlot acknowledgement ladder for
    // an object or ground weapon order. `requestedWeaponSlot` is present only
    // for a typed FIRE_WEAPON CommandButton; an ordinary attack uses the
    // actor's current weapon. `commandButtonWeapon` preserves the original
    // extra gate on KillPilot and Melee voices.
    [[nodiscard]] WorldCommandWeaponVoiceKind weaponOrderVoice(
        ObjectId actor, std::optional<uint8_t> requestedWeaponSlot,
        ObjectId target, bool commandButtonWeapon) const noexcept;
    // CommandXlat gives the GLA worker a distinct Move acknowledgement only
    // after the owning player has completed Upgrade_GLAWorkerShoes.  This is a
    // read-only command-presentation fact; confirmed movement still consumes
    // the normal typed Move/AttackMove order.
    [[nodiscard]] bool usesWorkerShoesMoveVoice(ObjectId actor) const noexcept;

private:
    const ecs::registry* m_registry = nullptr;
    const PlayerRegistry* m_players = nullptr;
    const ObjectOwnershipIndex* m_ownership = nullptr;
    const ObjectLifecycle* m_objects = nullptr;
    const ObjectSimulationContainmentDomain* m_containment = nullptr;
    const GameContentSnapshot* m_content = nullptr;
    const ai::ObjectAIRuntime* m_objectAI = nullptr;
};

} // namespace selection
} // namespace engine
