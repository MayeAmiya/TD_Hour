#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"

#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/script/GameSessionScriptTeamsDetail.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/command/OrderExecutor.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>
#include <iterator>
#include <variant>

namespace engine {
using namespace script_team_detail;
using namespace object_lifecycle_detail;

namespace {

[[nodiscard]] int64_t saturatingAddInt64(
    int64_t left, int64_t right) noexcept {
    if (right > 0 &&
        left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    if (right < 0 &&
        left < std::numeric_limits<int64_t>::min() - right) {
        return std::numeric_limits<int64_t>::min();
    }
    return left + right;
}

[[nodiscard]] int64_t saturatingMultiplyByPositive(
    int64_t value, int64_t multiplier) noexcept {
    if (multiplier <= 0) return 0;
    if (value > 0 &&
        value > std::numeric_limits<int64_t>::max() / multiplier) {
        return std::numeric_limits<int64_t>::max();
    }
    if (value < 0 &&
        value < std::numeric_limits<int64_t>::min() / multiplier) {
        return std::numeric_limits<int64_t>::min();
    }
    return value * multiplier;
}

[[nodiscard]] int64_t scaleInt64ByUnitFixed(
    int64_t value, math::q32_32 factor) noexcept {
    const int64_t oneRaw = math::q32_32{int32_t{1}}.raw();
    const int64_t factorRaw = std::clamp<int64_t>(
        factor.raw(), 0, oneRaw);
    const int64_t whole = value / oneRaw;
    const int64_t remainder = value % oneRaw;
    // |remainder| < 2^32 and factorRaw <= 2^32, so the unsigned product is
    // strictly below 2^64. Splitting around the Q32.32 denominator avoids a
    // signed value*raw overflow while preserving truncation toward zero.
    const uint64_t remainderMagnitude = remainder < 0
        ? static_cast<uint64_t>(-(remainder + 1)) + 1u
        : static_cast<uint64_t>(remainder);
    const uint64_t fractionalMagnitude =
        remainderMagnitude * static_cast<uint64_t>(factorRaw) /
        static_cast<uint64_t>(oneRaw);
    const int64_t fractional = remainder < 0
        ? -static_cast<int64_t>(fractionalMagnitude)
        : static_cast<int64_t>(fractionalMagnitude);
    return saturatingAddInt64(whole * factorRaw, fractional);
}

} // namespace

bool GameSessionScriptScenarioPlanTransactions::guardScriptTeamSupplyCenter(
    ObjectTeamId team, int32_t minimumSupplies,
    uint32_t sourceScriptId, uint32_t sourceSequence,
    uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) return false;
    const ObjectTeamRecord* teamRecord = m_world.m_objectTeams.find(team);
    const PlayerState* player = teamRecord
        ? m_content.m_players.get(teamRecord->owner) : nullptr;
    if (!teamRecord || !player) return false;
    // Player::guardSupplyCenter is an AIPlayer forwarding method. Human and
    // neutral controllers silently do nothing in RefCode.
    if (player->controller != PlayerControllerKind::Ai) return true;

    const auto objects = ecs::view<const ObjectIdentityComponent,
                                   const OwnerComponent,
                                   const ObjectLifecycleComponent,
                                   const ThingTemplateComponent,
                                   const TransformComponent>(m_world.m_registry);
    struct SupplyTarget final {
        ObjectId object = INVALID_OBJECT_ID;
        LogicFixedVec3 position{};
        math::q32_32 radius{};
    };

    math::q32_32 baseX{};
    math::q32_32 baseY{};
    uint64_t baseCount = 0;
    math::q32_32 enemyX{};
    math::q32_32 enemyY{};
    uint64_t enemyCount = 0;
    for (const ecs::entity entity : objects) {
        const ObjectLifecycleComponent& lifecycle =
            objects.template get<const ObjectLifecycleComponent>(entity);
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !type.archetype || !kindOfListContains(
                type.archetype, game::ObjectKindOf::Structure)) {
            continue;
        }
        const OwnerComponent& owner =
            objects.template get<const OwnerComponent>(entity);
        const TransformComponent& transform =
            objects.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        if (owner.player == teamRecord->owner) {
            baseX += position.x;
            baseY += position.y;
            ++baseCount;
        } else if (m_content.m_players.relationships().get(
                       teamRecord->owner, owner.player) ==
                   PlayerRelationship::Enemies) {
            enemyX += position.x;
            enemyY += position.y;
            ++enemyCount;
        }
    }
    if (baseCount != 0) {
        const math::q32_32 count{
            static_cast<int32_t>(baseCount)};
        baseX /= count;
        baseY /= count;
    } else if (player->startPosition >= 0) {
        for (const game::terrain::MultiplayerStartPosition& start :
             m_content.m_terrain.multiplayerStartPositions()) {
            if (start.index != player->startPosition) continue;
            baseX = math::q32_32::from_raw(start.positionRaw[0]);
            baseY = math::q32_32::from_raw(start.positionRaw[1]);
            break;
        }
    }
    if (enemyCount != 0) {
        const math::q32_32 count{
            static_cast<int32_t>(enemyCount)};
        enemyX /= count;
        enemyY /= count;
    }

    std::optional<SupplyTarget> target;
    // isSupplySourceAttacked scans CASH_GENERATOR/DOZER/HARVESTER objects
    // damaged within the retail ten-frame window. Stable ObjectId replaces
    // PlayerTeamList pointer traversal when several qualify.
    if (confirmedTick != 0) {
        constexpr uint64_t kLegacyAttackWindow = 10;
        for (const ecs::entity entity : objects) {
            const ObjectIdentityComponent& identity =
                objects.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                objects.template get<const OwnerComponent>(entity);
            const ObjectLifecycleComponent& lifecycle =
                objects.template get<const ObjectLifecycleComponent>(entity);
            const ThingTemplateComponent& type =
                objects.template get<const ThingTemplateComponent>(entity);
            if (!identity.id || owner.player != teamRecord->owner ||
                lifecycle.phase != ObjectLifecyclePhase::Alive ||
                !type.archetype ||
                (!kindOfListContains(type.archetype,
                                     game::ObjectKindOf::CashGenerator) &&
                 !kindOfListContains(type.archetype,
                                     game::ObjectKindOf::Dozer) &&
                 !kindOfListContains(type.archetype,
                                     game::ObjectKindOf::Harvester))) {
                continue;
            }
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
            if (!health || !health->hasLastDamageInfo ||
                health->lastDamageType == game::DamageType::HEALING ||
                health->lastDamageTick > confirmedTick ||
                confirmedTick - health->lastDamageTick >=
                    kLegacyAttackWindow) {
                continue;
            }
            const TransformComponent& transform =
                objects.template get<const TransformComponent>(entity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
            if (target && identity.id > target->object) continue;
            target = SupplyTarget{
                .object = identity.id,
                .position = readAuthoritativeObjectPosition(
                    m_world.m_registry, entity,
                    transform),
                .radius = geometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          geometry->boundingCircleRadiusFixed)
                    : math::q32_32{},
            };
        }
    }

    uint64_t requiredValue = minimumSupplies <= 0
        ? 0u : static_cast<uint64_t>(minimumSupplies);
    while (!target) {
        math::q32_32 bestDistanceSquared{};
        bool foundTarget = false;
        for (const ecs::entity entity : objects) {
            const ObjectIdentityComponent& identity =
                objects.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                objects.template get<const OwnerComponent>(entity);
            const ObjectLifecycleComponent& lifecycle =
                objects.template get<const ObjectLifecycleComponent>(entity);
            const ThingTemplateComponent& type =
                objects.template get<const ThingTemplateComponent>(entity);
            PlayerRelationship relationship = m_content.m_players.relationships().get(
                teamRecord->owner, owner.player);
            if (const PrimaryTeamComponent* primaryTeam =
                    ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, entity)) {
                if (const std::optional<PlayerRelationship> overrideRelationship =
                        m_content.m_players.teamRelationshipOverride(
                            teamRecord->owner, primaryTeam->team)) {
                    relationship = *overrideRelationship;
                }
            }
            if (!identity.id ||
                lifecycle.phase != ObjectLifecyclePhase::Alive ||
                !type.archetype ||
                !kindOfListContains(type.archetype,
                                    game::ObjectKindOf::Structure) ||
                !kindOfListContains(type.archetype,
                                    game::ObjectKindOf::SupplySource) ||
                relationship == PlayerRelationship::Enemies) {
                continue;
            }
            const ObjectEconomyComponent* economy =
                ecs::try_get<ObjectEconomyComponent>(m_world.m_registry, entity);
            if (!economy || economy->supplyWarehouseDocks.empty()) continue;
            uint64_t boxes = 0;
            for (const ObjectSupplyWarehouseDockRuntime& warehouse :
                 economy->supplyWarehouseDocks) {
                boxes = warehouse.boxesStored >
                        std::numeric_limits<uint64_t>::max() - boxes
                    ? std::numeric_limits<uint64_t>::max()
                    : boxes + warehouse.boxesStored;
            }
            const uint64_t value = boxes >
                    std::numeric_limits<uint64_t>::max() / 100u
                ? std::numeric_limits<uint64_t>::max()
                : boxes * 100u;
            if (value < requiredValue) continue;

            const TransformComponent& transform =
                objects.template get<const TransformComponent>(entity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
            const math::q32_32 radius = geometry
                ? math::q32_32::max(
                      math::q32_32{},
                      geometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, entity, transform);
            bool alreadyServed = false;
            for (const ecs::entity candidate : objects) {
                const OwnerComponent& candidateOwner =
                    objects.template get<const OwnerComponent>(candidate);
                const ObjectLifecycleComponent& candidateLifecycle =
                    objects.template get<const ObjectLifecycleComponent>(
                        candidate);
                const ThingTemplateComponent& candidateType =
                    objects.template get<const ThingTemplateComponent>(
                        candidate);
                if (candidateOwner.player != teamRecord->owner ||
                    candidateLifecycle.phase != ObjectLifecyclePhase::Alive ||
                    !candidateType.archetype ||
                    !kindOfListContains(
                        candidateType.archetype,
                        game::ObjectKindOf::CashGenerator)) {
                    continue;
                }
                const ObjectMapStatusComponent* candidateMap =
                    ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry,
                                                           candidate);
                if (candidateMap && candidateMap->offMap) continue;
                const TransformComponent& candidateTransform =
                    objects.template get<const TransformComponent>(candidate);
                const ObjectGeometryComponent* candidateGeometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, candidate);
                const math::q32_32 candidateRadius = candidateGeometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          candidateGeometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                const LogicFixedVec3 candidatePosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, candidate,
                        candidateTransform);
                const math::q32_32 dx = candidatePosition.x - position.x;
                const math::q32_32 dy = candidatePosition.y - position.y;
                const math::q32_32 close =
                    math::q32_32{int32_t{200}} + radius + candidateRadius;
                if (dx * dx + dy * dy <= close * close) {
                    alreadyServed = true;
                    break;
                }
            }
            if (alreadyServed) continue;

            const math::q32_32 dx = position.x - baseX;
            const math::q32_32 dy = position.y - baseY;
            const math::q32_32 distanceSquared = dx * dx + dy * dy;
            if (enemyCount != 0) {
                const math::q32_32 ex = position.x - enemyX;
                const math::q32_32 ey = position.y - enemyY;
                if (distanceSquared * math::q32_32{int32_t{2}} >
                    (ex * ex + ey * ey) *
                        math::q32_32{int32_t{3}}) {
                    continue;
                }
            }
            if (foundTarget && target &&
                (distanceSquared > bestDistanceSquared ||
                 (distanceSquared == bestDistanceSquared &&
                  identity.id > target->object))) {
                continue;
            }
            target = SupplyTarget{
                .object = identity.id,
                .position = position,
                .radius = radius,
            };
            bestDistanceSquared = distanceSquared;
            foundTarget = true;
        }
        if (target) break;
        requiredValue /= 2u;
        if (requiredValue <= 100u) break;
    }
    if (!target) return true;

    // groupGuardPosition offsets the warehouse anchor toward the current
    // enemy structure center by 80% of the warehouse bounding radius.
    if (enemyCount != 0) {
        const math::q32_32 offsetX = target->position.x - enemyX;
        const math::q32_32 offsetY = target->position.y - enemyY;
        const math::q32_32 length = math::q32_32::sqrt(
            offsetX * offsetX + offsetY * offsetY);
        if (length > math::q32_32{}) {
            const math::q32_32 offsetDistance =
                target->radius * math::q32_32::from_fraction(4, 5);
            target->position.x = target->position.x -
                offsetX / length * offsetDistance;
            target->position.y = target->position.y -
                offsetY / length * offsetDistance;
        }
    }

    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.legacyMembers(team);
    if (members.empty()) return true;
    container::Vector<ObjectId> actors;
    actors.reserve(members.size());
    for (const ObjectId member : members) {
        // AIGroup::groupGuardPosition forwards only to members with an
        // AIUpdateInterface. Avoid materializing an order queue on static or
        // presentation-only Team members.
        if (m_ai.m_objectAI.actorState(member)) actors.push_back(member);
    }
    if (actors.empty()) return true;
    return m_port.executeScriptOrder({
        .contextPlayer = teamRecord->owner,
        .authority = ScriptOrderAuthority::ScenarioTeam,
        .scenarioTeam = team,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .sourceEffectOrdinal = sourceSequence,
        .kind = ObjectOrderKind::TacticalAttack,
        .tacticalAttackSubtype = ObjectTacticalAttackSubtype::Guard,
        .actors = std::move(actors),
        .targetPosition = {
            .x = target->position.x,
            .y = target->position.y,
            .z = target->position.z,
            .valid = true,
        },
    }).accepted;
}

bool GameSessionScriptScenarioPlanTransactions::executeScriptSkirmishApproach(
    ObjectTeamId team, container::StringView pathPrefix,
    bool followPath, bool asTeam, uint32_t sourceScriptId,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || pathPrefix.empty()) {
        return false;
    }
    const ObjectTeamRecord* teamRecord = m_world.m_objectTeams.find(team);
    const PlayerState* owner = teamRecord
        ? m_content.m_players.get(teamRecord->owner) : nullptr;
    if (!teamRecord || !owner) return false;

    const std::optional<PlayerId> enemyPlayer =
        m_port.currentEnemyPlayer(teamRecord->owner);
    const PlayerState* enemy = enemyPlayer
        ? m_content.m_players.get(*enemyPlayer) : nullptr;
    if (!enemy || enemy->startPosition < 0) return true;

    math::q32_32 centerX{};
    math::q32_32 centerY{};
    size_t centerCount = 0;
    container::Vector<ObjectId> actors;
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity) : nullptr;
        if (!transform) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        centerX += position.x;
        centerY += position.y;
        ++centerCount;
        if (m_ai.m_objectAI.actorState(member)) actors.push_back(member);
    }
    if (centerCount == 0 || actors.empty()) return true;
    const math::q32_32 fixedCenterCount{
        static_cast<int32_t>(centerCount)};
    centerX /= fixedCenterCount;
    centerY /= fixedCenterCount;

    container::String pathLabel{pathPrefix};
    pathLabel += std::to_string(enemy->startPosition + 1);
    const game::terrain::WaypointRecord* start =
        m_content.m_terrain.closestWaypointOnPathRaw(
            centerX.raw(), centerY.raw(),
            pathLabel);
    if (!start) return true;

    ScriptOrderIntent order{
        .contextPlayer = teamRecord->owner,
        .authority = ScriptOrderAuthority::ScenarioTeam,
        .scenarioTeam = team,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .sourceEffectOrdinal = sourceSequence,
        .kind = ObjectOrderKind::Move,
        .moveRouteSubtype = followPath
            ? (asTeam
                ? ObjectMoveRouteSubtype::WaypointPathTeam
                : ObjectMoveRouteSubtype::WaypointPathIndividuals)
            : ObjectMoveRouteSubtype::Direct,
        .actors = std::move(actors),
    };
    if (followPath) {
        order.waypointStartId = start->id;
        order.waypointGraphRevision = m_content.m_terrain.waypointGraphRevision();
    } else {
        order.targetPosition = {
            .x = math::q32_32::from_raw(start->positionRaw[0]),
            .y = math::q32_32::from_raw(start->positionRaw[1]),
            .z = math::q32_32::from_raw(start->positionRaw[2]),
            .valid = true,
        };
    }
    return m_port.executeScriptOrder(order).accepted;
}

bool GameSessionScriptScenarioPlanTransactions::fireScriptSpecialPowerAtMostCost(
    PlayerId player, container::StringView specialPower,
    uint32_t sourceScriptId, uint32_t sourceSequence,
    uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || specialPower.empty()) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    const SpecialPowerDefinition* definition =
        m_content.m_contentSnapshot.findSpecialPower(specialPower);
    if (!playerState || !definition) return true;
    // doSkirmishFireSpecialPowerAtMostCost calls the module directly with
    // COMMAND_FIRED_BY_SCRIPT. Like the other named/script SpecialPower
    // entry points, it deliberately bypasses the ordinary required-science
    // gate; ObjectSpecialPower still owns readiness/disabled admission.

    ObjectId source = INVALID_OBJECT_ID;
    const game::ObjectSpecialPowerRule* sourceRule = nullptr;
    for (const ObjectId candidate : m_world.m_ownership.objects(player)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(candidate);
        const ObjectLifecycleComponent* lifecycle = entity
            ? ecs::try_get<ObjectLifecycleComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectMapStatusComponent* mapStatus = entity
            ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectHealthComponent* health = entity
            ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectStatusComponent* status = entity
            ? ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectSpecialPowerComponent* powers = entity
            ? ecs::try_get<ObjectSpecialPowerComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !lifecycle ||
            lifecycle->phase != ObjectLifecyclePhase::Alive ||
            m_world.m_objects.isPendingDestroy(candidate) ||
            (health && health->effectivelyDead) ||
            (mapStatus && mapStatus->offMap) ||
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) ||
            isObjectDisabled(m_world.m_registry, *entity, confirmedTick) ||
            !powers) {
            continue;
        }
        for (size_t index = 0; index < powers->instances.size(); ++index) {
            const ObjectSpecialPowerRuntime& runtime =
                powers->instances[index];
            if (runtime.content != definition->id ||
                runtime.readyTick > confirmedTick ||
                runtime.pausedCount != 0) {
                continue;
            }
            source = candidate;
            if (powers->plan && index < powers->plan->rules.size()) {
                sourceRule = &powers->plan->rules[index];
            }
            break;
        }
        if (source) break;
    }
    if (!source) return true;

    // Share the dynamic `<This Player's Enemy>` policy so Skirmish planners
    // and authored SIDE selectors never drift onto different opponents.
    const std::optional<PlayerId> enemy = m_port.currentEnemyPlayer(player);
    if (!enemy) return true;

    struct CostTarget final {
        ObjectId object = INVALID_OBJECT_ID;
        math::q32_32 x{};
        math::q32_32 y{};
        int64_t value = 0;
        game::ObjectKindOfMask kinds{};
        bool airborne = false;
    };
    container::Vector<CostTarget> targets;
    for (const ObjectId candidate : m_world.m_ownership.objects(*enemy)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(candidate);
        const ObjectLifecycleComponent* lifecycle = entity
            ? ecs::try_get<ObjectLifecycleComponent>(m_world.m_registry, *entity)
            : nullptr;
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectHealthComponent* health = entity
            ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectMapStatusComponent* mapStatus = entity
            ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *entity)
            : nullptr;
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
            : nullptr;
        const PlayerState* targetPlayer = owner
            ? m_content.m_players.get(owner->player) : nullptr;
        const ObjectKindOfComponent* kinds = entity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity)
            : nullptr;
        const ObjectAirborneComponent* airborne = entity
            ? ecs::try_get<ObjectAirborneComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !lifecycle || !transform || !type ||
            !type->archetype || !targetPlayer ||
            lifecycle->phase != ObjectLifecyclePhase::Alive ||
            m_world.m_objects.isPendingDestroy(candidate) ||
            (health && health->effectivelyDead) ||
            (mapStatus && mapStatus->offMap)) {
            continue;
        }
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        targets.push_back({
            .object = candidate,
            .x = position.x,
            .y = position.y,
            .value = std::max<int64_t>(
                0, calculateObjectBuildCost(
                       *type->archetype, *targetPlayer,
                       m_world.m_registry, m_world.m_objects)),
            .kinds = kinds ? kinds->mask : game::ObjectKindOfMask{},
            .airborne = airborne && airborne->isAirborne,
        });
    }

    const math::q32_32 radius = math::q32_32::max(
        math::q32_32{int32_t{50}}, definition->radiusCursorRadius);
    const math::q32_32 radiusSquared = radius * radius;
    int64_t bestValue = 0;
    ObjectId bestObject = INVALID_OBJECT_ID;
    math::q32_32 targetX{};
    math::q32_32 targetY{};
    bool specializedTarget = false;

    if (definition->specialPowerType ==
            game::SpecialPowerType::ClusterMines ||
        definition->specialPowerType ==
            game::SpecialPowerType::NukeClusterMines) {
        // AISkirmishPlayer::computeSuperweaponTarget deliberately mines one
        // approach edge of its own base instead of maximizing enemy value.
        math::q32_32 baseX{};
        math::q32_32 baseY{};
        uint64_t baseCount = 0;
        for (const ObjectId candidate : m_world.m_ownership.objects(player)) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(candidate);
            const ObjectKindOfComponent* kinds = entity
                ? ecs::try_get<ObjectKindOfComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            const TransformComponent* transform = entity
                ? ecs::try_get<TransformComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            if (!entity || !kinds || !transform ||
                !game::objectHasKind(
                    kinds->mask, game::ObjectKindOf::Structure)) {
                continue;
            }
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, *entity, *transform);
            baseX += position.x;
            baseY += position.y;
            ++baseCount;
        }
        if (baseCount != 0) {
            const math::q32_32 count{static_cast<int32_t>(baseCount)};
            baseX /= count;
            baseY /= count;
            math::q32_32 baseRadius{};
            for (const ObjectId candidate :
                 m_world.m_ownership.objects(player)) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(candidate);
                const ObjectKindOfComponent* kinds = entity
                    ? ecs::try_get<ObjectKindOfComponent>(
                          m_world.m_registry, *entity)
                    : nullptr;
                const TransformComponent* transform = entity
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry, *entity)
                    : nullptr;
                if (!entity || !kinds || !transform ||
                    !game::objectHasKind(
                        kinds->mask, game::ObjectKindOf::Structure)) {
                    continue;
                }
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *entity, *transform);
                const math::q32_32 dx = position.x - baseX;
                const math::q32_32 dy = position.y - baseY;
                const ObjectGeometryComponent* geometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, *entity);
                baseRadius = math::q32_32::max(
                    baseRadius,
                    math::q32_32::sqrt(dx * dx + dy * dy) +
                        (geometry
                             ? math::q32_32::max(
                                   math::q32_32{},
                                   geometry->boundingCircleRadiusFixed)
                             : math::q32_32{}));
            }
            if (baseRadius <= math::q32_32{})
                baseRadius = math::q32_32{int32_t{200}};
            const int32_t mode =
                m_content.m_simulationRandom.integerInclusive(0, 2);
            container::String pathLabel = mode == 1
                ? "Flank" : mode == 2 ? "Backdoor" : "Center";
            if (playerState->startPosition >= 0) {
                pathLabel += std::to_string(
                    playerState->startPosition + 1);
            }
            const game::terrain::WaypointRecord* approach =
                m_content.m_terrain.closestWaypointOnPathRaw(
                    baseX.raw(), baseY.raw(), pathLabel);
            math::q32_32 goalX{};
            math::q32_32 goalY{};
            if (approach) {
                goalX = math::q32_32::from_raw(
                    approach->positionRaw[0]);
                goalY = math::q32_32::from_raw(
                    approach->positionRaw[1]);
            } else if (!targets.empty()) {
                math::q32_32 minimumX = targets.front().x;
                math::q32_32 maximumX = targets.front().x;
                math::q32_32 minimumY = targets.front().y;
                math::q32_32 maximumY = targets.front().y;
                for (const CostTarget& target : targets) {
                    minimumX = math::q32_32::min(minimumX, target.x);
                    maximumX = math::q32_32::max(maximumX, target.x);
                    minimumY = math::q32_32::min(minimumY, target.y);
                    maximumY = math::q32_32::max(maximumY, target.y);
                }
                goalX = (minimumX + maximumX) /
                    math::q32_32{int32_t{2}};
                goalY = (minimumY + maximumY) /
                    math::q32_32{int32_t{2}};
            }
            const math::q32_32 directionX = goalX - baseX;
            const math::q32_32 directionY = goalY - baseY;
            const math::q32_32 length = math::q32_32::sqrt(
                directionX * directionX + directionY * directionY);
            if (length > math::q32_32{}) {
                targetX = baseX + directionX / length * baseRadius;
                targetY = baseY + directionY / length * baseRadius;
                specializedTarget = true;
            }
        }
    }

    if (!specializedTarget) {
        if (targets.empty()) return true;
        const bool sneakAttack = definition->specialPowerType ==
            game::SpecialPowerType::SneakAttack;
        for (const CostTarget& center : targets) {
            int64_t aggregate = 0;
            for (const CostTarget& candidate : targets) {
                const math::q32_32 dx = candidate.x - center.x;
                const math::q32_32 dy = candidate.y - center.y;
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                if (distanceSquared > radiusSquared) continue;
                if (!sneakAttack && candidate.airborne &&
                    game::objectHasKind(
                        candidate.kinds, game::ObjectKindOf::Aircraft)) {
                    continue;
                }
                int64_t value = candidate.value;
                const bool commandCenter = game::objectHasKind(
                    candidate.kinds, game::ObjectKindOf::CommandCenter);
                const bool superweapon = game::objectHasKind(
                    candidate.kinds, game::ObjectKindOf::FsSuperweapon);
                if (sneakAttack) {
                    const bool defended = game::objectHasKind(
                            candidate.kinds,
                            game::ObjectKindOf::FsBaseDefense) ||
                        game::objectHasKind(
                            candidate.kinds,
                            game::ObjectKindOf::TechBaseDefense) ||
                        ((game::objectHasKind(
                              candidate.kinds,
                              game::ObjectKindOf::Vehicle) ||
                          game::objectHasKind(
                              candidate.kinds,
                              game::ObjectKindOf::Infantry)) &&
                         !game::objectHasKind(
                             candidate.kinds,
                             game::ObjectKindOf::Dozer) &&
                         !game::objectHasKind(
                             candidate.kinds,
                             game::ObjectKindOf::Harvester));
                    if (commandCenter || superweapon)
                        value = saturatingMultiplyByPositive(value, 5);
                    if (defended)
                        value = -saturatingMultiplyByPositive(value, 5);
                } else if (commandCenter || superweapon) {
                    value /= 10;
                }
                const math::q32_32 distance =
                    math::q32_32::sqrt(distanceSquared);
                const math::q32_32 factor = math::q32_32::max(
                    math::q32_32{},
                    math::q32_32{int32_t{1}} -
                        distance /
                            (math::q32_32{int32_t{2}} * radius));
                const int64_t weighted =
                    scaleInt64ByUnitFixed(value, factor);
                aggregate = saturatingAddInt64(aggregate, weighted);
            }
            if (aggregate > bestValue ||
                (aggregate == bestValue && aggregate > 0 &&
                 (!bestObject || center.object < bestObject))) {
                bestValue = aggregate;
                bestObject = center.object;
                targetX = center.x;
                targetY = center.y;
            }
        }
    }
    if ((!specializedTarget && (!bestObject || bestValue <= 0)) ||
        (targetX == math::q32_32{} && targetY == math::q32_32{})) {
        return true;
    }

    math::q32_32 targetZ = math::q32_32::from_raw(
        m_content.m_terrain.groundHeightRaw(
            targetX.raw(), targetY.raw()));

    // SneakAttack is also a construction placement. Keep the cost optimum,
    // then move only as far as necessary to a legal reference-object site.
    if (definition->specialPowerType ==
            game::SpecialPowerType::SneakAttack &&
        sourceRule && !sourceRule->referenceObject.empty()) {
        const container::SharedPtr<const game::ObjectArchetype> reference =
            m_content.m_contentSnapshot.findObjectArchetype(
                sourceRule->referenceObject);
        if (reference) {
            const math::q32_32 yaw =
                reference->templateData.placementViewAngleRadiansFixed;
            const auto legalAt = [&](math::q32_32 x,
                                     math::q32_32 y) {
                const math::q32_32 z = math::q32_32::from_raw(
                    m_content.m_terrain.groundHeightRaw(
                        x.raw(), y.raw()));
                const GameSessionBuildPlacementLegalityEvaluation evaluation =
                    m_port.evaluateBuildPlacement(
                        source, {x, y, z}, yaw, player, *reference,
                        true);
                if (!evaluation.evaluated || evaluation.legality !=
                        selection::LocalPlacementLegality::Legal) {
                    return false;
                }
                targetX = x;
                targetY = y;
                targetZ = z;
                return true;
            };
            bool legal = legalAt(targetX, targetY);
            const int64_t cellSizeRaw =
                m_content.m_navigation.grid().transform().cellSizeRaw;
            const math::q32_32 step = cellSizeRaw > 0
                ? math::q32_32::max(
                      math::q32_32{int32_t{1}},
                      math::q32_32::from_raw(cellSizeRaw))
                : math::q32_32{int32_t{10}};
            for (math::q32_32 diameter = step;
                 !legal && diameter <= math::q32_32{int32_t{400}};
                 diameter += step) {
                const math::q32_32 offset = diameter /
                    math::q32_32{int32_t{2}};
                for (math::q32_32 x = targetX - offset;
                     !legal && x <= targetX + offset; x += step) {
                    legal = legalAt(x, targetY - offset) ||
                        legalAt(x, targetY + offset);
                }
                for (math::q32_32 y = targetY - offset;
                     !legal && y <= targetY + offset; y += step) {
                    legal = legalAt(targetX - offset, y) ||
                        legalAt(targetX + offset, y);
                }
            }
            if (!legal) return true;
        }
    }

    return m_port.executeScriptOrder({
        .contextPlayer = player,
        .authority = ScriptOrderAuthority::NamedObjects,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .sourceEffectOrdinal = sourceSequence,
        .kind = ObjectOrderKind::SpecialPower,
        .actors = {source},
        .targetPosition = {
            .x = targetX,
            .y = targetY,
            .z = targetZ,
            .valid = true,
        },
        .contentName = definition->name,
    }).accepted;
}

bool GameSessionScriptScenarioPlanTransactions::attackScriptNearestValueGroup(
    ObjectTeamId team, script::ScriptComparison comparison,
    int32_t minimumValue, uint32_t sourceScriptId,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) {
        return false;
    }
    const ObjectTeamRecord* teamRecord = m_world.m_objectTeams.find(team);
    if (!teamRecord || !m_content.m_players.get(teamRecord->owner)) return false;
    // RefCode initializes no target for the other comparison spellings and
    // therefore only meaningfully supports these two forms.
    if (comparison != script::ScriptComparison::GreaterEqual &&
        comparison != script::ScriptComparison::Greater) {
        return true;
    }

    math::q32_32 centerX{};
    math::q32_32 centerY{};
    size_t centerCount = 0;
    container::Vector<ObjectId> actors;
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!transform) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        centerX += position.x;
        centerY += position.y;
        ++centerCount;
        actors.push_back(member);
    }
    if (centerCount == 0 || actors.empty()) return true;
    const math::q32_32 fixedCenterCount{
        static_cast<int32_t>(centerCount)};
    centerX /= fixedCenterCount;
    centerY /= fixedCenterCount;

    struct ValueCell final {
        int32_t x = 0;
        int32_t y = 0;
        int64_t value = 0;
    };
    const int64_t valueCellWorldSizeRaw =
        math::q32_32{int32_t{100}}.raw();
    const auto valueCellCoordinate = [valueCellWorldSizeRaw](
            int64_t raw) noexcept {
        int64_t quotient = raw / valueCellWorldSizeRaw;
        if (raw < 0 && raw % valueCellWorldSizeRaw != 0) --quotient;
        return quotient;
    };
    container::Vector<ValueCell> cells;
    const auto candidates = ecs::view<
        const ObjectIdentityComponent, const ObjectLifecycleComponent,
        const OwnerComponent, const TransformComponent,
        const ThingTemplateComponent>(m_world.m_registry);
    for (const ecs::entity entity : candidates) {
        const ObjectIdentityComponent& identity =
            candidates.template get<const ObjectIdentityComponent>(entity);
        const ObjectLifecycleComponent& lifecycle =
            candidates.template get<const ObjectLifecycleComponent>(entity);
        const OwnerComponent& owner =
            candidates.template get<const OwnerComponent>(entity);
        const TransformComponent& transform =
            candidates.template get<const TransformComponent>(entity);
        const ThingTemplateComponent& type =
            candidates.template get<const ThingTemplateComponent>(entity);
        const PlayerState* targetPlayer = m_content.m_players.get(owner.player);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
        if (!identity.id || !type.archetype || !targetPlayer ||
            lifecycle.phase != ObjectLifecyclePhase::Alive ||
            m_world.m_objects.isPendingDestroy(identity.id) ||
            (health && health->effectivelyDead) ||
            (mapStatus && mapStatus->offMap) ||
            relationshipBetweenPlayerAndObject(
                m_world.m_registry, m_content.m_players, teamRecord->owner, entity) !=
                PlayerRelationship::Enemies) {
            continue;
        }
        const LogicFixedVec3 fixedPosition = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const int64_t rawCellX = valueCellCoordinate(
            fixedPosition.x.raw());
        const int64_t rawCellY = valueCellCoordinate(
            fixedPosition.y.raw());
        if (rawCellX < std::numeric_limits<int32_t>::min() ||
            rawCellX > std::numeric_limits<int32_t>::max() ||
            rawCellY < std::numeric_limits<int32_t>::min() ||
            rawCellY > std::numeric_limits<int32_t>::max()) {
            continue;
        }
        const int32_t cellX = static_cast<int32_t>(rawCellX);
        const int32_t cellY = static_cast<int32_t>(rawCellY);
        const auto insertPosition = std::lower_bound(
            cells.begin(), cells.end(), std::pair{cellX, cellY},
            [](const ValueCell& cell,
               const std::pair<int32_t, int32_t>& key) {
                return std::pair{cell.x, cell.y} < key;
            });
        const int64_t value = std::max<int64_t>(
            0, calculateObjectBuildCost(
                   *type.archetype, *targetPlayer,
                   m_world.m_registry, m_world.m_objects));
        if (insertPosition == cells.end() ||
            insertPosition->x != cellX ||
            insertPosition->y != cellY) {
            cells.insert(insertPosition, {
                .x = cellX,
                .y = cellY,
                .value = value,
            });
        } else {
            insertPosition->value = value >
                    std::numeric_limits<int64_t>::max() -
                        insertPosition->value
                ? std::numeric_limits<int64_t>::max()
                : insertPosition->value + value;
        }
    }

    const ValueCell* best = nullptr;
    math::q32_32 bestDistance = math::q32_32::from_raw(
        std::numeric_limits<int64_t>::max());
    const math::q32_32 halfCell{int32_t{50}};
    for (const ValueCell& cell : cells) {
        // PartitionManager's greaterThan flag is TRUE for both supported
        // comparison spellings in RefCode, so equality never qualifies.
        if (cell.value <= static_cast<int64_t>(minimumValue)) continue;
        const math::q32_32 x = math::q32_32{cell.x} *
            math::q32_32{int32_t{100}} + halfCell;
        const math::q32_32 y = math::q32_32{cell.y} *
            math::q32_32{int32_t{100}} + halfCell;
        const math::q32_32 dx = x - centerX;
        const math::q32_32 dy = y - centerY;
        const math::q32_32 distance = dx * dx + dy * dy;
        if (distance < bestDistance ||
            (distance == bestDistance &&
             (!best || std::pair{cell.x, cell.y} <
                           std::pair{best->x, best->y}))) {
            best = &cell;
            bestDistance = distance;
        }
    }
    if (!best) return true;
    const LogicFixedVec3 targetPositionFixed{
        math::q32_32{best->x} * math::q32_32{int32_t{100}} + halfCell,
        math::q32_32{best->y} * math::q32_32{int32_t{100}} + halfCell,
        {},
    };
    LogicFixedVec3 admittedTargetPosition = targetPositionFixed;
    admittedTargetPosition.z = math::q32_32::from_raw(
        m_content.m_terrain.groundHeightRaw(
            admittedTargetPosition.x.raw(), admittedTargetPosition.y.raw()));

    return m_port.executeScriptOrder({
        .contextPlayer = teamRecord->owner,
        .authority = ScriptOrderAuthority::ScenarioTeam,
        .scenarioTeam = team,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .sourceEffectOrdinal = sourceSequence,
        .kind = ObjectOrderKind::Move,
        .actors = std::move(actors),
        .targetPosition = {
            .x = admittedTargetPosition.x,
            .y = admittedTargetPosition.y,
            .z = admittedTargetPosition.z,
            .valid = true,
        },
        .attackMove = true,
    }).accepted;
}

bool GameSessionScriptScenarioPlanTransactions::executeScriptMostValuableCommandButton(
    ObjectTeamId team, container::StringView buttonName,
    math::q32_32 range, bool /*allTeamMembers*/,
    uint32_t sourceScriptId, uint32_t sourceSequence,
    uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || buttonName.empty() ||
        range < math::q32_32{}) {
        return false;
    }
    const ObjectTeamRecord* teamRecord = m_world.m_objectTeams.find(team);
    if (!teamRecord || !m_content.m_players.get(teamRecord->owner)) return false;
    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.legacyMembers(team);
    if (members.empty()) return true;

    const auto selection = m_port.selectCommandButton(
        members, buttonName,
        script::ScriptCommandButtonActorPolicy::All,
        math::q32_32{int32_t{100}},
        script::ScriptCommandButtonTargetKind::MostValuableEnemy,
        INVALID_OBJECT_ID, {}, {}, range);
    if (!selection || !selection->targetObject ||
        !selection->sourceActor) {
        return true;
    }
    // RefCode accepts the authored Boolean but never reads it; the selected
    // AIGroup always executes the CommandButton. Preserve that observable
    // compatibility instead of inventing a single-source interpretation.
    container::Vector<ObjectId> actors = selection->actors;
    if (actors.empty()) return true;

    return m_port.executeCommandButton({
        .contextPlayer = teamRecord->owner,
        .authority = ScriptOrderAuthority::ScenarioTeam,
        .scenarioTeam = team,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .sourceEffectOrdinal = sourceSequence,
        .kind = ObjectOrderKind::CommandButton,
        .actors = std::move(actors),
        .targetObject = selection->targetObject,
        .contentName = container::String{buttonName},
    }, false).accepted;
}

bool GameSessionScriptScenarioPlanTransactions::setScriptGlobalCombatPolicy(
    script::ScriptGlobalCombatPolicy policy, bool enabled,
    uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) return false;
    if (policy == script::ScriptGlobalCombatPolicy::ChooseVictimAlwaysNormal) {
        if (m_presentation.m_chooseVictimAlwaysNormal == enabled) return false;
        m_presentation.m_chooseVictimAlwaysNormal = enabled;
        return true;
    }
    if (m_presentation.m_objectsReceiveDifficultyBonuses == enabled) return false;
    m_presentation.m_objectsReceiveDifficultyBonuses = enabled;
    container::Vector<ObjectId> objects;
    const auto view = ecs::view<const ObjectIdentityComponent>(m_world.m_registry);
    objects.reserve(view.size());
    for (const ecs::entity entity : view) {
        const ObjectId object = ecs::get<const ObjectIdentityComponent>(
            m_world.m_registry, entity).id;
        if (object) objects.push_back(object);
    }
    std::sort(objects.begin(), objects.end());
    for (const ObjectId object : objects) {
        static_cast<void>(m_port.lifecycle.applyObjectDifficultyBonusPolicy(
            object, enabled, confirmedTick));
    }
    return true;
}

size_t GameSessionScriptScenarioPlanTransactions::sellEverythingForPlayer(
    PlayerId player, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick ||
        !m_content.m_players.get(player)) return 0;
    const container::Span<const ObjectId> owned = m_world.m_ownership.objects(player);
    const container::Vector<ObjectId> snapshot{owned.begin(), owned.end()};
    size_t started = 0;
    GameSessionObjectSaleTransactions sales{
        m_content, m_world, m_ai, m_presentation,
        m_port.lifecycle};
    for (const ObjectId object : snapshot) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        const ObjectKindOfComponent* kinds = entity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity) : nullptr;
        // Player::sellEverythingUnderTheSun intentionally does not sell every
        // owned Structure.  It selects faction structures plus the explicit
        // command-center/power categories used by campaign factions.
        if (!entity || (!hasFactionStructureKind(kinds) &&
                        !hasObjectKind(
                            kinds, game::ObjectKindOf::CommandCenter) &&
                        !hasObjectKind(kinds, game::ObjectKindOf::FsPower))) {
            continue;
        }
        started += sales.beginObjectSale(object, player, confirmedTick) ? 1u : 0u;
    }
    return started;
}

bool GameSessionScriptScenarioPlanTransactions::requestPlayerRepairStructure(
    PlayerId player, ObjectId structure, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick ||
        !m_content.m_players.get(player) || !structure)
        return false;

    const std::optional<ecs::entity> targetEntity =
        m_world.m_objects.entityFromId(structure);
    const OwnerComponent* targetOwner = targetEntity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *targetEntity)
        : nullptr;
    const TransformComponent* targetTransform = targetEntity
        ? ecs::try_get<TransformComponent>(m_world.m_registry, *targetEntity)
        : nullptr;
    if (!targetEntity || !targetOwner || !targetTransform)
        return false;

    struct Candidate final
    {
        ObjectId builder = INVALID_OBJECT_ID;
        math::q32_32 distanceSquared{};
    };
    const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
        m_world.m_registry, *targetEntity,
        *targetTransform);

    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const TransformComponent,
                                const ObjectBuilderComponent>(m_world.m_registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view)
    {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        const TransformComponent& transform =
            view.template get<const TransformComponent>(entity);
        const ObjectBuilderComponent& builder =
            view.template get<const ObjectBuilderComponent>(entity);
        if (!identity.id || owner.player != player ||
            builder.runtimes.empty() ||
            m_world.m_objects.isPendingDestroy(identity.id))
            continue;
        const LogicFixedVec3 builderPosition = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const math::q32_32 dx = builderPosition.x - targetPosition.x;
        const math::q32_32 dy = builderPosition.y - targetPosition.y;
        candidates.push_back({
            .builder = identity.id,
            .distanceSquared = dx * dx + dy * dy,
        });
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) noexcept {
                  if (left.distanceSquared != right.distanceSquared)
                      return left.distanceSquared < right.distanceSquared;
                  return left.builder < right.builder;
              });
    for (const Candidate& candidate : candidates)
    {
        if (m_world.m_objectSimulation.requestObjectRepair(
                m_world.m_registry, m_world.m_objects, m_content.m_players, candidate.builder,
                structure, confirmedTick, 0, true, false))
            return true;
    }
    return false;
}

} // namespace engine
