#include "core/container/string_utils.h"
#include "game/object/simulation/special/ObjectOclUpdate.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * fps;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

void advanceSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

[[nodiscard]] LogicFixedVec3 closestPlayableEdge(
    const game::terrain::TerrainLogic& terrain,
    LogicFixedVec3 point) noexcept {
    if (!terrain.map().isLoaded()) return point;
    using Fixed = math::q32_32;
    const game::terrain::TerrainExtentRaw extent =
        terrain.map().playableExtentRaw();
    const Fixed minimumX = Fixed::from_raw(extent.minimumX);
    const Fixed minimumY = Fixed::from_raw(extent.minimumY);
    const Fixed maximumX = Fixed::from_raw(extent.maximumX);
    const Fixed maximumY = Fixed::from_raw(extent.maximumY);
    Fixed best = Fixed::abs(point.x - minimumX);
    LogicFixedVec3 result = point;
    result.x = minimumX;
    const Fixed right = Fixed::abs(point.x - maximumX);
    if (right < best) {
        best = right;
        result.x = maximumX;
    }
    const Fixed bottom = Fixed::abs(point.y - minimumY);
    if (bottom < best) {
        best = bottom;
        result.x = point.x;
        result.y = minimumY;
    }
    const Fixed top = Fixed::abs(point.y - maximumY);
    if (top < best) {
        result.x = point.x;
        result.y = maximumY;
    }
    result.z = Fixed::from_raw(
        terrain.groundHeightRaw(result.x.raw(), result.y.raw()));
    return result;
}

} // namespace

void ObjectOclUpdateSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!objectTemplate || !objectTemplate->archetype ||
        !objectTemplate->archetype->oclUpdatePlan) return;

    ObjectOclUpdateComponent component;
    component.plan = objectTemplate->archetype->oclUpdatePlan;
    component.instances.resize(component.plan->rules.size());
    for (size_t index = 0; index < component.plan->rules.size(); ++index) {
        const game::ObjectOclUpdateParameters& rule = component.plan->rules[index];
        ObjectOclUpdateRuntime& runtime = component.instances[index];
        runtime.defaultContent =
            content.findObjectCreationListId(rule.objectCreationList);
        runtime.factionContent.reserve(rule.factionObjectCreationLists.size());
        for (const game::ObjectFactionOclReference& reference :
             rule.factionObjectCreationLists) {
            runtime.factionContent.push_back(
                content.findObjectCreationListId(reference.objectCreationList));
        }
    }
    ecs::emplace<ObjectOclUpdateComponent>(registry, entity,
                                            std::move(component));
}

bool ObjectOclUpdateSystem::resetTimers(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick) const {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    ObjectOclUpdateComponent* component =
        ecs::try_get<ObjectOclUpdateComponent>(registry, *entity);
    if (!component || !component->plan) return false;

    if (component->plan->rules.empty() || component->instances.empty())
        return false;
    const game::ObjectOclUpdateParameters& rule = component->plan->rules.front();
    const int32_t minimum = static_cast<int32_t>(std::min<uint32_t>(
        rule.minimumDelayMilliseconds,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    const int32_t maximum = static_cast<int32_t>(std::min<uint32_t>(
        rule.maximumDelayMilliseconds,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    const uint32_t delayMilliseconds = static_cast<uint32_t>(
        random.integerInclusive(minimum, maximum));
    ObjectOclUpdateRuntime& runtime = component->instances.front();
    runtime.timerStartedTick = confirmedTick;
    runtime.nextCreationTick = saturatingAdd(
        confirmedTick, millisecondsToTicks(
            delayMilliseconds, logicFramesPerSecond));
    runtime.timerArmed = true;
    return true;
}

void ObjectOclUpdateSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectCreationListInvocation>& outInvocations) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectOclUpdateComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectOclUpdateComponent& component =
            ecs::get<ObjectOclUpdateComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, candidate.entity);
        if (!transform || !owner || !team) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const bool underConstruction = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
        // OCLUpdate opts into every Disabled reason so it can shift both
        // absolute timer endpoints one frame at a time. This preserves the
        // original countdown rather than firing a supply/OCL burst as soon
        // as an EMP or other disable expires.
        const bool disabled = isObjectDisabled(
            registry, candidate.entity, confirmedTick);
        const PlayerState* player = players.get(owner->player);

        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectOclUpdateParameters& rule =
                component.plan->rules[index];
            ObjectOclUpdateRuntime& runtime = component.instances[index];
            const auto schedule = [&]() {
                const int32_t minimum = static_cast<int32_t>(std::min<uint32_t>(
                    rule.minimumDelayMilliseconds,
                    static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
                const int32_t maximum = static_cast<int32_t>(std::min<uint32_t>(
                    rule.maximumDelayMilliseconds,
                    static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
                const uint32_t delayMilliseconds = static_cast<uint32_t>(
                    random.integerInclusive(minimum, maximum));
                runtime.timerStartedTick = confirmedTick;
                runtime.nextCreationTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        delayMilliseconds, logicFramesPerSecond));
                runtime.timerArmed = true;
            };

            if (runtime.timerArmed && disabled) {
                runtime.timerStartedTick = saturatingAdd(runtime.timerStartedTick, 1);
                runtime.nextCreationTick = saturatingAdd(runtime.nextCreationTick, 1);
                continue;
            }

            if (rule.factionTriggered) {
                const bool playable = player && player->isPlayableSide();
                if (runtime.factionNeutral) {
                    if (playable) {
                        runtime.factionNeutral = false;
                        runtime.controllingPlayer = owner->player;
                        schedule();
                    }
                } else if (!playable) {
                    runtime.factionNeutral = true;
                    runtime.controllingPlayer = INVALID_PLAYER_ID;
                    runtime.timerArmed = false;
                } else if (runtime.controllingPlayer != owner->player) {
                    runtime.controllingPlayer = owner->player;
                    schedule();
                }
                if (runtime.factionNeutral) continue;
            }

            if (!runtime.timerArmed) {
                if (underConstruction) continue;
                schedule();
                continue;
            }
            if (confirmedTick < runtime.nextCreationTick || underConstruction) {
                continue;
            }

            // RefCode schedules the next deadline before executing the OCL.
            schedule();
            game::ObjectCreationListContentId selected = runtime.defaultContent;
            if (rule.factionTriggered) {
                selected = {};
                for (size_t factionIndex = 0;
                     factionIndex < rule.factionObjectCreationLists.size() &&
                     factionIndex < runtime.factionContent.size();
                     ++factionIndex) {
                    if (player && equalInsensitive(
                            player->side,
                            rule.factionObjectCreationLists[factionIndex].faction)) {
                        selected = runtime.factionContent[factionIndex];
                        break;
                    }
                }
            }
            if (!selected || !content.findObjectCreationList(selected)) continue;

            LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
                registry, candidate.entity, *transform);
            LogicFixedVec3 primary = sourcePosition;
            if (rule.createAtEdge) {
                primary = closestPlayableEdge(terrain, sourcePosition);
            }
            LogicFixedVec3 velocity;
            if (const ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(registry,
                                                         candidate.entity)) {
                velocity = physics->velocityUnitsPerSecond;
            }
            game::ObjectVeterancyLevel veterancy =
                game::ObjectVeterancyLevel::Regular;
            if (const ObjectVeterancyComponent* experience =
                    ecs::try_get<ObjectVeterancyComponent>(registry,
                                                           candidate.entity)) {
                veterancy = experience->level;
            }
            const ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(registry, candidate.entity);
            const ObjectTerrainLayerComponent* terrainLayer =
                ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                           candidate.entity);
            outInvocations.push_back({
                .content = selected,
                .source = candidate.object,
                .owner = owner->player,
                .primaryTeam = team->team,
                .primaryPosition = primary,
                .secondaryPosition = sourcePosition,
                .sourceVelocity = velocity,
                .orientationRadians = readAuthoritativeObjectYaw(
                    registry, candidate.entity, *transform),
                .veterancy = veterancy,
                .authoredOrder = rule.authoredOrder,
                .emissionSequence = nextEmissionSequence,
                .confirmedTick = confirmedTick,
                .sourcePathfindLayer = terrainLayer
                    ? terrainLayer->pathfindLayer
                    : game::terrain::kGroundPathfindLayer,
                .hasSecondaryPosition = true,
                .sourceAirborne = airborne && airborne->isAirborne,
            });
            advanceSequence(nextEmissionSequence);
        }
    }
}

} // namespace engine
