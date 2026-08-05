#pragma once

#include <cstddef>
#include <cstdint>

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

namespace engine
{

class ObjectLifecycle;
class PlayerRegistry;

// Confirmed game/environment facts which affect every Drawable in RefCode.
// They are frozen by GameSession before this producer runs; the renderer never
// consults map, weather, order, weapon, contain, or lifecycle state directly.
struct ObjectModelConditionEnvironment final
{
    PlayerId localPlayer = INVALID_PLAYER_ID;
    const PlayerRegistry* players = nullptr;
    bool forceModelsToFollowTimeOfDay = true;
    bool forceModelsToFollowWeather = true;
    bool night = false;
    bool snowy = false;
};

struct ObjectModelConditionAuthorityReport final
{
    size_t visitedObjects = 0;
    size_t changedObjects = 0;
};

struct ObjectModelConditionAuthorityState final {
    PlayerId localPlayer = INVALID_PLAYER_ID;
    bool forceModelsToFollowTimeOfDay = true;
    bool forceModelsToFollowWeather = true;
    bool night = false;
    bool snowy = false;
    bool initialized = false;
};

// Gameplay modules publish door transitions into a per-object staging
// component.  They must not write RenderModelComponent::modelConditionFlags
// directly: the authority composes all sources once at the end of the
// confirmed tick.  Unspecified withdraws only this source's contribution.
void publishObjectModelConditionDoor(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectModelConditionDoorSource source,
    size_t doorSlot,
    ObjectModelConditionDoorPhase phase,
    uint64_t confirmedTick,
    uint64_t sequence = 0) noexcept;

// Applies a source-local clear/set transition. Both masks become part of that
// source's declared family, while only setMask remains selected. Contributions
// are composed after typed core producers, so two sources selecting the same
// bit combine without either source clearing the other.
void publishObjectModelConditionContribution(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectModelConditionContributionSource source,
    const game::ModelConditionMask& clearMask,
    const game::ModelConditionMask& setMask,
    uint64_t confirmedTick,
    uint64_t sequence = 0) noexcept;

// Rebuilds the condition bits owned by the typed core producers plus staged
// Upgrade, Fire, and Tactical contributions. Independent body-damage,
// steering, bridge/destruction and one-shot presentation families remain
// under their dedicated owners. Scripts currently publish status/gameplay
// facts rather than writing ModelCondition bits directly. Call once at the
// end of a confirmed tick, after gameplay producers have committed and before
// visual animation clocks are advanced.
[[nodiscard]] ObjectModelConditionAuthorityReport updateAuthoritativeObjectModelConditions(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectModelConditionAuthorityState& state,
    const ObjectModelConditionEnvironment& environment,
    uint64_t confirmedTick);

} // namespace engine
