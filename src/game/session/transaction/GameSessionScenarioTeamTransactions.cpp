#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(const ObjectKindOfComponent* kinds,
                                 game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool hasProductionCreateActions(
    const script::ScriptRuntime& runtime,
    const scenario::ScriptTeamDefinition& definition) noexcept {
    if (!definition.plan.executesActionsOnCreate) return false;
    const auto& program = runtime.program();
    if (!program) return false;
    const auto hooks = program->teamHooks();
    const auto found = std::find_if(
        hooks.begin(), hooks.end(),
        [&definition](const script::ScriptTeamHookDefinition& candidate) {
            return container::asciiEqualIgnoreCase(
                candidate.teamName, definition.name);
        });
    return found != hooks.end() &&
        static_cast<bool>(found->productionCreateActions);
}

void clearLegacySkirmishTeamBuildingFlags(
    const StrategicAIRuntime& strategicAI,
    script::ScriptRuntime& scriptRuntime,
    PlayerId owner) noexcept {
    const StrategicAIPlayerBrain* brain = strategicAI.findBrain(owner);
    if (brain && brain->autonomousSkirmish)
        scriptRuntime.clearLegacySkirmishTeamBuildingFlags();
}

} // namespace

void GameSessionScriptScenarioPlanTransactions::updateScenarioTeamProductions()
{
    container::Vector<ObjectTeamId> productions;
    for (const ObjectTeamRecord& record : m_world.m_objectTeams.teams()) {
        if (record.id && !record.active &&
            record.assemblyKind == ObjectTeamAssemblyKind::Production) {
            productions.push_back(record.id);
        }
    }
    std::sort(
        productions.begin(), productions.end(),
        [this](ObjectTeamId left, ObjectTeamId right) noexcept {
            const ObjectTeamRecord* leftRecord =
                m_world.m_objectTeams.find(left);
            const ObjectTeamRecord* rightRecord =
                m_world.m_objectTeams.find(right);
            const bool leftPriorityBuild = leftRecord &&
                leftRecord->productionMayUseBusyFactory;
            const bool rightPriorityBuild = rightRecord &&
                rightRecord->productionMayUseBusyFactory;
            if (leftPriorityBuild != rightPriorityBuild)
                return leftPriorityBuild;
            if (leftPriorityBuild && rightPriorityBuild) {
                const uint64_t leftStarted = leftRecord &&
                        leftRecord->assemblyStartTickKnown
                    ? leftRecord->assemblyStartedTick : 0;
                const uint64_t rightStarted = rightRecord &&
                        rightRecord->assemblyStartTickKnown
                    ? rightRecord->assemblyStartedTick : 0;
                // buildSpecificAITeam(priorityBuild=true) prepends every new
                // request, so the latest direct script request owns the head.
                if (leftStarted != rightStarted)
                    return leftStarted > rightStarted;
                return left > right;
            }
            const int32_t leftPriority =
                m_world.m_objectTeams
                    .productionPriority(left).value_or(1);
            const int32_t rightPriority =
                m_world.m_objectTeams
                    .productionPriority(right).value_or(1);
            return leftPriority != rightPriority
                ? leftPriority > rightPriority
                : left < right;
        });

    struct UnitTarget final {
        container::SharedPtr<const game::ObjectArchetype> product;
        uint32_t rosterIndex = UINT32_MAX;
        uint32_t minimum = 0;
        uint32_t maximum = 0;
    };
    const auto saturatingAdd = [](uint32_t left, uint32_t right) noexcept {
        return right > std::numeric_limits<uint32_t>::max() - left
            ? std::numeric_limits<uint32_t>::max() : left + right;
    };

    for (const ObjectTeamId team : productions) {
        const ObjectTeamRecord* record = m_world.m_objectTeams.find(team);
        if (!record || record->active ||
            record->assemblyKind != ObjectTeamAssemblyKind::Production)
            continue;
        const scenario::ScriptTeamDefinition* definition =
            m_presentation.m_scenarioDefinition && record->scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      record->scenarioDefinition)
                : nullptr;
        if (!definition) {
            static_cast<void>(m_world.m_objectProduction.detachTeamProduction(
                m_world.m_registry, team));
            static_cast<void>(m_world.m_objectTeams.clearAssembly(team));
            continue;
        }

        container::Vector<UnitTarget> targets;
        targets.reserve(definition->plan.units.size());
        for (size_t rosterIndex = 0;
             rosterIndex < definition->plan.units.size(); ++rosterIndex) {
            const scenario::ScenarioTeamUnitPlan& source =
                definition->plan.units[rosterIndex];
            const container::SharedPtr<const game::ObjectArchetype> product =
                m_content.m_contentSnapshot.findObjectArchetype(source.templateName);
            if (!product || source.maximumUnits == 0) continue;
            targets.push_back({
                .product = product,
                .rosterIndex = static_cast<uint32_t>(rosterIndex),
                .minimum = source.minimumUnits,
                .maximum = source.maximumUnits,
            });
        }
        std::stable_sort(
            targets.begin(), targets.end(),
            [](const UnitTarget& left, const UnitTarget& right) {
                // Required WorkOrders precede optional-only ones so a cash-
                // constrained build cannot spend its first admission on a
                // min=0 roster slot while a minimum slot remains unqueued.
                return (left.minimum != 0) > (right.minimum != 0);
            });

        LogicFixedVec3 home{};
        std::optional<ObjectProductionRoutePoint> rally;
        if (const game::terrain::WaypointRecord* waypoint =
                m_content.m_terrain.waypointByName(
                    definition->plan.homeWaypoint)) {
            home = {
                math::q32_32::from_raw(waypoint->positionRaw[0]),
                math::q32_32::from_raw(waypoint->positionRaw[1]),
                math::q32_32::from_raw(waypoint->positionRaw[2]),
            };
            rally = ObjectProductionRoutePoint{
                .x = home.x,
                .y = home.y,
                .z = home.z};
        } else {
            const PlayerState* owner =
                m_content.m_players.get(definition->resolvedOwner);
            bool foundStart = false;
            if (owner && owner->startPosition >= 0) {
                for (const game::terrain::MultiplayerStartPosition& start :
                     m_content.m_terrain.multiplayerStartPositions()) {
                    if (start.index != owner->startPosition) continue;
                    home = {
                        math::q32_32::from_raw(start.positionRaw[0]),
                        math::q32_32::from_raw(start.positionRaw[1]),
                        math::q32_32::from_raw(start.positionRaw[2]),
                    };
                    foundStart = true;
                    break;
                }
            }
            if (!foundStart)
                home.z = math::q32_32::from_raw(
                    m_content.m_terrain.groundHeightRaw(0, 0));
        }
        const math::q32_32 recruitRadius = math::q32_32::max(
            math::q32_32{},
            m_content.m_objectSimulationRules.ai.maximumRecruitDistance);
        const math::q32_32 recruitRadiusSquared =
            recruitRadius * recruitRadius;
        const bool expired = record->assemblyDeadlineTick !=
                std::numeric_limits<uint64_t>::max() &&
            m_presentation.m_confirmedTick > record->assemblyDeadlineTick;
        uint32_t sequence = record->assemblySourceSequence == 0
            ? 1u : record->assemblySourceSequence;
        const uint64_t confirmedTick =
            m_presentation.m_confirmedTick;
        const uint64_t baseRetryTicks = static_cast<uint64_t>(std::max(
            1, m_content.m_startInfo.gameSpeedFPS));
        const auto nextRetryTick =
            [confirmedTick, baseRetryTicks](uint32_t failures) noexcept {
                const uint64_t multiplier = std::min<uint64_t>(
                    8, static_cast<uint64_t>(failures) + 1u);
                const uint64_t delay = baseRetryTicks >
                        std::numeric_limits<uint64_t>::max() / multiplier
                    ? std::numeric_limits<uint64_t>::max()
                    : baseRetryTicks * multiplier;
                return confirmedTick >
                        std::numeric_limits<uint64_t>::max() - delay
                    ? std::numeric_limits<uint64_t>::max()
                    : confirmedTick + delay;
            };

        if (!expired) {
            for (uint32_t phase = 0; phase < 2; ++phase) {
                if (phase == 1) {
                    bool allMinimumCommitted = true;
                    for (const UnitTarget& target : targets) {
                        const uint32_t completed =
                            m_world.m_objectTeams.productionUnitCompleted(
                                team, target.rosterIndex);
                        const uint32_t pending =
                            m_world.m_objectProduction.pendingUnitCountForTeam(
                                m_world.m_registry, team, target.product->name,
                                target.rosterIndex);
                        allMinimumCommitted = allMinimumCommitted &&
                            saturatingAdd(completed, pending) >=
                                target.minimum;
                    }
                    if (!allMinimumCommitted) break;
                }
                for (size_t targetIndex = 0;
                     targetIndex < targets.size(); ++targetIndex) {
                const UnitTarget& target = targets[targetIndex];
                const uint32_t targetCount = phase == 0
                    ? target.minimum : target.maximum;
                if (targetCount == 0 ||
                    (phase == 1 && target.maximum <= target.minimum)) {
                    continue;
                }
                uint32_t completed =
                    m_world.m_objectTeams.productionUnitCompleted(
                        team, target.rosterIndex);
                uint32_t pending =
                    m_world.m_objectProduction.pendingUnitCountForTeam(
                        m_world.m_registry, team, target.product->name,
                        target.rosterIndex);
                while (saturatingAdd(completed, pending) < targetCount) {
                    // RefCode keeps at most one factory attached to a
                    // WorkOrder. Retry/recruit only after that job completes
                    // or its producer disappears.
                    if (pending != 0) break;
                    const ObjectTeamProductionWorkOrder persisted =
                        m_world.m_objectTeams
                            .productionWorkOrder(team, target.rosterIndex);
                    if (confirmedTick < persisted.nextAttemptTick) break;
                    if (recruitScenarioTeamUnit(
                            team, *definition, target.product, home,
                            recruitRadiusSquared, sequence,
                            m_presentation.m_confirmedTick, target.rosterIndex)) {
                        if (completed !=
                            std::numeric_limits<uint32_t>::max())
                            ++completed;
                        continue;
                    }

                    bool ignorePrerequisites = false;
                    if (!m_port.productionPolicy.admitsObjectBuildability(
                            definition->resolvedOwner, *target.product,
                            ignorePrerequisites)) {
                        static_cast<void>(
                            m_world.m_objectTeams
                                .updateProductionWorkOrder(
                                    team, target.rosterIndex,
                                    INVALID_OBJECT_ID,
                                    nextRetryTick(persisted.failureCount),
                                    true));
                        break;
                    }
                    container::Vector<ObjectId> idleProducers;
                    container::Vector<ObjectId> busyProducers;
                    const auto view = ecs::view<
                        const ObjectIdentityComponent,
                        const OwnerComponent,
                        const ObjectProductionComponent>(m_world.m_registry);
                    idleProducers.reserve(view.size_hint());
                    busyProducers.reserve(view.size_hint());
                    for (const ecs::entity entity : view) {
                        const ObjectIdentityComponent& identity =
                            view.template get<const ObjectIdentityComponent>(
                                entity);
                        const OwnerComponent& owner =
                            view.template get<const OwnerComponent>(entity);
                        const ObjectProductionComponent& production =
                            view.template get<const ObjectProductionComponent>(
                                entity);
                        if (!identity.id ||
                            owner.player != definition->resolvedOwner ||
                            m_world.m_objects.isPendingDestroy(identity.id) ||
                            !production.plan || !production.exitPlan ||
                            !canObjectBuildTemplate(
                                m_world.m_registry, entity, m_content.m_contentSnapshot,
                                m_presentation.m_scriptCommandBarOverrides, m_content.m_players,
                                definition->resolvedOwner, *target.product,
                                ignorePrerequisites)) {
                            continue;
                        }
                        const ObjectStatusComponent* status =
                            ecs::try_get<ObjectStatusComponent>(
                                m_world.m_registry, entity);
                        if ((status && status->hasAny(
                                game::objectStatusBit(
                                    game::ObjectStatusFlag::UnderConstruction) |
                                game::objectStatusBit(
                                    game::ObjectStatusFlag::Sold)))) {
                            continue;
                        }
                        (production.jobs.empty()
                            ? idleProducers : busyProducers)
                            .push_back(identity.id);
                    }
                    std::sort(idleProducers.begin(), idleProducers.end());
                    std::sort(busyProducers.begin(), busyProducers.end());
                    // RefCode remembers the final compatible busy factory
                    // when no idle one exists. ObjectId is the modern stable
                    // traversal key in place of mutable BuildList pointers.
                    if (record->productionMayUseBusyFactory) {
                        std::reverse(busyProducers.begin(),
                                     busyProducers.end());
                        idleProducers.insert(
                            idleProducers.end(), busyProducers.begin(),
                            busyProducers.end());
                    }
                    if (persisted.producer) {
                        const auto cached = std::find(
                            idleProducers.begin(), idleProducers.end(),
                            persisted.producer);
                        if (cached != idleProducers.end()) {
                            std::rotate(idleProducers.begin(), cached,
                                        std::next(cached));
                        }
                    }

                    bool queued = false;
                    bool moneyBlocked = false;
                    ObjectId selectedProducer = INVALID_OBJECT_ID;
                    for (const ObjectId producer : idleProducers) {
                        const ObjectProductionRequestResult result =
                            m_world.m_objectProduction.queueUnit(
                                m_world.m_registry, m_world.m_objects, m_content.m_players,
                                m_content.m_contentSnapshot,
                                m_presentation.m_scriptCommandBarOverrides, producer,
                                definition->resolvedOwner, target.product,
                                m_presentation.m_confirmedTick, sequence,
                                static_cast<uint32_t>(std::max(
                                    1, m_content.m_startInfo.gameSpeedFPS)),
                                m_content.m_objectSimulationRules.energy,
                                ignorePrerequisites, team, rally,
                                target.rosterIndex);
                        if (result.accepted) {
                            queued = true;
                            selectedProducer = producer;
                            if (sequence !=
                                std::numeric_limits<uint32_t>::max()) {
                                ++sequence;
                            }
                            pending = saturatingAdd(pending, 1u);
                            break;
                        }
                        if (result.rejection ==
                            ObjectProductionRejectionReason::InsufficientFunds) {
                            moneyBlocked = true;
                            break;
                        }
                    }
                    if (queued) {
                        const uint64_t nextAttempt = confirmedTick >
                                std::numeric_limits<uint64_t>::max() -
                                    baseRetryTicks
                            ? std::numeric_limits<uint64_t>::max()
                            : confirmedTick + baseRetryTicks;
                        static_cast<void>(
                            m_world.m_objectTeams
                                .updateProductionWorkOrder(
                                    team, target.rosterIndex,
                                    selectedProducer,
                                    nextAttempt, false));
                    } else {
                        static_cast<void>(
                            m_world.m_objectTeams
                                .updateProductionWorkOrder(
                                    team, target.rosterIndex,
                                    moneyBlocked
                                        ? persisted.producer
                                        : INVALID_OBJECT_ID,
                                    nextRetryTick(persisted.failureCount),
                                    true));
                    }
                    if (!queued || moneyBlocked) break;
                }
            }
            }
        }
        static_cast<void>(m_world.m_objectTeams.updateAssemblySourceSequence(
            team, sequence));

        // An authored empty/zero roster is immediately all-built in RefCode;
        // non-singleton empty activation will then retire naturally.
        bool allMaximumBuilt = true;
        bool minimumCommitted = true;
        uint32_t pendingTotal = 0;
        for (size_t targetIndex = 0;
             targetIndex < targets.size(); ++targetIndex) {
            const UnitTarget& target = targets[targetIndex];
            const uint32_t completed =
                m_world.m_objectTeams.productionUnitCompleted(
                    team, target.rosterIndex);
            const uint32_t pending =
                m_world.m_objectProduction.pendingUnitCountForTeam(
                    m_world.m_registry, team, target.product->name,
                    target.rosterIndex);
            pendingTotal = saturatingAdd(pendingTotal, pending);
            allMaximumBuilt = allMaximumBuilt &&
                completed >= target.maximum;
            // TeamInQueue::isMinimumBuilt counts the one factory currently
            // attached to a WorkOrder as one committed unit. It does not
            // disband an expired Team merely because that required product
            // has not exited the factory yet.
            minimumCommitted = minimumCommitted &&
                saturatingAdd(completed, pending != 0 ? 1u : 0u) >=
                    target.minimum;
        }

        if (expired && !minimumCommitted) {
            // No completed recruit and no attached factory job covers at
            // least one required WorkOrder slot: this is the original
            // minimum-build failure path.
            static_cast<void>(m_world.m_objectProduction.detachTeamProduction(
                m_world.m_registry, team));
            const container::Vector<ObjectId> members{
                m_world.m_objectTeams.members(team).begin(),
                m_world.m_objectTeams.members(team).end()};
            const std::optional<ObjectTeamId> fallback =
                m_world.m_objectTeams.defaultTeam(definition->resolvedOwner);
            if (fallback) {
                for (const ObjectId member : members) {
                    static_cast<void>(m_port.transferObjectToTeam(
                        member, *fallback, m_presentation.m_confirmedTick));
                }
            }
            static_cast<void>(m_world.m_objectTeams.clearAssembly(team));
            if (!definition->isSingleton)
                static_cast<void>(
                    m_world.m_objectTeams.retireEmptyScenarioTeam(team));
            clearLegacySkirmishTeamBuildingFlags(
                m_ai.m_strategicAI, m_presentation.m_scriptRuntime,
                definition->resolvedOwner);
            continue;
        }

        if (!(allMaximumBuilt && pendingTotal == 0) &&
            !(expired && minimumCommitted && pendingTotal == 0)) {
            bool anyIdle = false;
            if (definition->plan.executesActionsOnCreate) {
                for (const ObjectId member : m_world.m_objectTeams.members(team)) {
                    const std::optional<ai::ObjectAIActorStateView> state =
                        m_ai.m_objectAI.actorState(member);
                    if (state && state->idle) {
                        anyIdle = true;
                        break;
                    }
                }
            }
            if (anyIdle) {
                static_cast<void>(m_world.m_objectTeams.markProductionStarted(
                    team, m_presentation.m_confirmedTick));
            }
            continue;
        }
        const uint64_t readyFrames =
            static_cast<uint64_t>(std::max(
                1, m_content.m_startInfo.gameSpeedFPS)) * 60u;
        const uint64_t readyOrigin = record->assemblyStartTickKnown
            ? record->assemblyStartedTick : m_presentation.m_confirmedTick;
        const uint64_t readyDeadline = readyOrigin >
                std::numeric_limits<uint64_t>::max() - readyFrames
            ? std::numeric_limits<uint64_t>::max()
            : readyOrigin + readyFrames;
        static_cast<void>(m_world.m_objectTeams.beginAssembly(
            team, ObjectTeamAssemblyKind::ProductionReady,
            readyDeadline, sequence, readyOrigin));
    }
}

void GameSessionScriptScenarioPlanTransactions::updateScenarioTeamAssemblies()
{
    container::Vector<ObjectTeamId> assemblies;
    for (const ObjectTeamRecord& record : m_world.m_objectTeams.teams()) {
        if (record.id && !record.active &&
            record.assemblyKind != ObjectTeamAssemblyKind::None) {
            assemblies.push_back(record.id);
        }
    }
    std::sort(assemblies.begin(), assemblies.end());

    for (const ObjectTeamId team : assemblies) {
        const ObjectTeamRecord* record = m_world.m_objectTeams.find(team);
        if (!record || record->active ||
            record->assemblyKind == ObjectTeamAssemblyKind::None) continue;
        // The production planner owns empty/in-flight Teams and moves them to
        // ProductionReady only after max completion or timed minimum success.
        if (record->assemblyKind == ObjectTeamAssemblyKind::Production)
            continue;
        const scenario::ScriptTeamDefinition* definition =
            m_presentation.m_scenarioDefinition && record->scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      record->scenarioDefinition)
                : nullptr;
        const container::Span<const ObjectId> members =
            m_world.m_objectTeams.members(team);
        if (members.empty()) {
            if (record->assemblyKind ==
                    ObjectTeamAssemblyKind::ProductionReady && definition) {
                // Empty authored rosters are still all-built Teams. RefCode
                // activates them so OnCreate may populate the Team; retiring
                // the inactive record here silently skipped that hook.
                static_cast<void>(m_world.m_objectTeams.activate(
                    team, m_presentation.m_confirmedTick));
                clearLegacySkirmishTeamBuildingFlags(
                    m_ai.m_strategicAI, m_presentation.m_scriptRuntime,
                    record->owner);
                continue;
            }
            static_cast<void>(m_world.m_objectTeams.clearAssembly(team));
            if (definition && !definition->isSingleton)
                static_cast<void>(
                    m_world.m_objectTeams.retireEmptyScenarioTeam(team));
            continue;
        }

        bool ready = true;
        bool anyIdle = false;
        for (const ObjectId member : members) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(member);
            if (!entity || m_world.m_objects.isPendingDestroy(member)) {
                ready = false;
                continue;
            }
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
            if ((health && health->effectivelyDead) ||
                ecs::try_get<ObjectContainedByComponent>(
                    m_world.m_registry, *entity)) {
                ready = false;
                continue;
            }
            const ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
            if (queue && !queue->orders.empty()) {
                ready = false;
            }
            const std::optional<ai::ObjectAIActorStateView> state =
                m_ai.m_objectAI.actorState(member);
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *entity);
            if (type && type->archetype && type->archetype->hasAiUpdate &&
                !state) {
                // RefCode constructs AIUpdate synchronously with the Object.
                // Modern lifecycle publication may expose Team membership one
                // phase earlier; do not activate or run OnCreate through that
                // transient gap.
                ready = false;
                continue;
            }
            anyIdle = anyIdle || (state && state->idle);
            if (state && state->state != ai::AIStateId::Idle) {
                ready = false;
            }
        }
        // checkReadyTeams deliberately stops waiting for every member when
        // an idle member can execute the authored ProductionCondition THEN
        // actions (the other members may have been put into Guard by those
        // actions). The action target must have resolved in the frozen Script
        // program; the raw Team flag/name alone is insufficient.
        if (!ready && anyIdle && definition &&
            hasProductionCreateActions(
                m_presentation.m_scriptRuntime, *definition)) {
            ready = true;
        }
        if (!ready && m_presentation.m_confirmedTick <= record->assemblyDeadlineTick)
            continue;
        static_cast<void>(m_world.m_objectTeams.activate(
            team, m_presentation.m_confirmedTick));
        clearLegacySkirmishTeamBuildingFlags(
            m_ai.m_strategicAI, m_presentation.m_scriptRuntime,
            record->owner);
    }

    // TeamInQueue destruction activates an authored zero-roster Team so its
    // OnCreate action can run; Team::update then removes a still-empty,
    // non-singleton instance. Activation after this tick's hook scan keeps a
    // pending creation pulse, while activation observed by the current hook
    // scan has had that pulse consumed. Retire only the latter so OnCreate
    // gets one complete confirmed transaction to populate the Team.
    container::Vector<ObjectTeamId> emptyCreatedTeams;
    for (const ObjectTeamRecord& record : m_world.m_objectTeams.teams()) {
        if (!record.id || !record.active || !record.scenarioDefinition ||
            !record.members.empty() || record.pendingInitialCreationPulse ||
            !record.hasCreationPulse ||
            record.createdAtConfirmedTick !=
                m_presentation.m_confirmedTick) {
            continue;
        }
        const scenario::ScriptTeamDefinition* definition =
            m_presentation.m_scenarioDefinition
            ? m_presentation.m_scenarioDefinition->findScriptTeam(
                  record.scenarioDefinition)
            : nullptr;
        if (definition && !definition->isSingleton)
            emptyCreatedTeams.push_back(record.id);
    }
    for (const ObjectTeamId team : emptyCreatedTeams) {
        static_cast<void>(m_world.m_objectTeams.deactivate(team));
        static_cast<void>(
            m_world.m_objectTeams.retireEmptyScenarioTeam(team));
    }
}

void GameSessionScriptScenarioPlanTransactions::resolveScenarioReinforcementTransportOrders()
{
    container::Vector<ObjectId> transports;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectOrderQueueComponent,
                                const ObjectLocomotionComponent,
                                const TransformComponent>(m_world.m_registry);
    transports.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectOrderQueueComponent& queue =
            view.template get<const ObjectOrderQueueComponent>(entity);
        if (queue.orders.empty()) continue;
        const ObjectOrderIntent& order = queue.orders.front();
        if (order.kind != ObjectOrderKind::Move ||
            order.source != ObjectOrderSource::System ||
            (order.systemPurpose !=
                 ObjectOrderSystemPurpose::ScenarioReinforcementDeliver &&
             order.systemPurpose !=
                 ObjectOrderSystemPurpose::ScenarioReinforcementExit)) {
            continue;
        }
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (object) transports.push_back(object);
    }
    std::sort(transports.begin(), transports.end());

    for (const ObjectId transport : transports) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(transport);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        ObjectLocomotionComponent* locomotion = entity
            ? ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity)
            : nullptr;
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !queue || queue->orders.empty() || !locomotion ||
            !transform || locomotion->hasActiveMove ||
            locomotion->state != ObjectLocomotionState::Idle) {
            continue;
        }
        const ObjectOrderIntent order = queue->orders.front();
        if (order.kind != ObjectOrderKind::Move ||
            order.source != ObjectOrderSource::System ||
            (order.systemPurpose !=
                 ObjectOrderSystemPurpose::ScenarioReinforcementDeliver &&
             order.systemPurpose !=
                 ObjectOrderSystemPurpose::ScenarioReinforcementExit)) {
            continue;
        }
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        const math::q32_32 targetX = order.targetX;
        const math::q32_32 targetY = order.targetY;
        const math::q32_32 dx = position.x - targetX;
        const math::q32_32 dy = position.y - targetY;
        const math::q32_32 arrival = math::q32_32::max(
            math::q32_32::from_fraction(1, 1000),
            locomotion->closeEnough + math::q32_32::from_fraction(1, 1000));
        if (dx * dx + dy * dy > arrival * arrival) continue;

        queue->orders.erase(queue->orders.begin());
        ++queue->revision;
        if (order.systemPurpose ==
            ObjectOrderSystemPurpose::ScenarioReinforcementDeliver) {
            static_cast<void>(m_world.m_objectSimulation.requestContainment(
                m_world.m_registry, m_world.m_objects,
                {.kind = ObjectContainmentRequestKind::EjectAll,
                 .container = transport,
                 .confirmedTick = m_presentation.m_confirmedTick,
                 .force = true},
                &m_content.m_players, &m_content.m_contentSnapshot));
            const ObjectTeamId team{order.systemPurposeInstance};
            if (team && m_world.m_objectTeams.find(team)) {
                static_cast<void>(m_world.m_objectTeams.activate(
                    team, m_presentation.m_confirmedTick));
            }
        } else {
            static_cast<void>(m_port.lifecycle.requestDestroyObject(
                transport, ObjectDestroyReason::System,
                m_presentation.m_confirmedTick));
        }
    }
}

void GameSessionScriptScenarioPlanTransactions::resolveScriptContainmentEnterIntents()
{
    container::Vector<ObjectId> objects;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectScriptContainmentEnterComponent>(
        m_world.m_registry);
    objects.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (object) objects.push_back(object);
    }
    std::sort(objects.begin(), objects.end());

    for (const ObjectId object : objects) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        ObjectScriptContainmentEnterComponent* intent = entity
            ? ecs::try_get<ObjectScriptContainmentEnterComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !intent) continue;
        const ObjectId targetId = intent->target;
        const std::optional<ecs::entity> target =
            m_world.m_objects.entityFromId(targetId);
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
        const ObjectOrderIntent* order = queue && !queue->orders.empty()
            ? &queue->orders.front() : nullptr;
        const bool ownsOrder = order &&
            order->kind == ObjectOrderKind::Move &&
            order->source == ObjectOrderSource::System &&
            order->systemPurpose ==
                ObjectOrderSystemPurpose::ContainmentEnter &&
            order->systemPurposeInstance == targetId.value &&
            order->sourceSequence == intent->sourceSequence;
        const auto clearIntent = [&]() {
            if (queue && ownsOrder && !queue->orders.empty()) {
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                if (ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            m_world.m_registry, *entity)) {
                    locomotion->forwardSpeed = {};
                    locomotion->movingBackward = false;
                    locomotion->hasActiveMove = false;
                    locomotion->state = ObjectLocomotionState::Idle;
                }
            }
            ecs::remove<ObjectScriptContainmentEnterComponent>(
                m_world.m_registry, *entity);
        };
        if (!target || !ownsOrder) {
            clearIntent();
            continue;
        }
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        const TransformComponent* targetTransform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *target);
        if (!transform || !targetTransform) {
            clearIntent();
            continue;
        }
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity);
        const ObjectGeometryComponent* targetGeometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *target);
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        const math::q32_32 one{int32_t{1}};
        const math::q32_32 arrival = math::q32_32::max(
            one, locomotion ? locomotion->closeEnough : one) +
            (geometry ? math::q32_32::max(
                math::q32_32{}, geometry->boundingCircleRadiusFixed)
                      : math::q32_32{}) +
            (targetGeometry ? math::q32_32::max(
                math::q32_32{},
                targetGeometry->boundingCircleRadiusFixed)
                            : math::q32_32{});
        const LogicFixedVec3 objectPosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *entity,
                *transform);
        const LogicFixedVec3 targetPosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *target,
                *targetTransform);
        const math::q32_32 dx = objectPosition.x - targetPosition.x;
        const math::q32_32 dy = objectPosition.y - targetPosition.y;
        const math::q32_32 verticalAllowance = math::q32_32::max(
            one,
            (geometry ? math::q32_32::max(
                math::q32_32{}, geometry->heightFixed)
                      : math::q32_32{}) +
            (targetGeometry ? math::q32_32::max(
                math::q32_32{}, targetGeometry->heightFixed)
                            : math::q32_32{}));
        const bool closeEnough = dx * dx + dy * dy <= arrival * arrival;
        const bool verticalOverlap = math::q32_32::abs(
            objectPosition.z - targetPosition.z) <= verticalAllowance;
        if (!closeEnough || !verticalOverlap) {
            // An inactive locomotor does not mean the move failed: the
            // deterministic navigation request may still be queued or its
            // result may be waiting at the next confirmed feedback boundary.
            // Rewriting the queue revision here cancelled and resubmitted the
            // same path every tick, then discarded the entry after three
            // frames.  ObjectAI owns terminal failure; while this correlated
            // order remains at the head, leave it stable and wait.
            continue;
        }

        const bool attached = m_world.m_objectSimulation.requestContainment(
            m_world.m_registry, m_world.m_objects,
            {.kind = ObjectContainmentRequestKind::Attach,
             .container = targetId,
             .object = object,
             .confirmedTick = m_presentation.m_confirmedTick},
            &m_content.m_players, &m_content.m_contentSnapshot);
        // A full/invalid target is terminal for this authored Enter command;
        // a later script may issue a new one after capacity changes.
        clearIntent();
        static_cast<void>(attached);
    }
}

} // namespace engine
