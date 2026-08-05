#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionContainmentTransactions.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/runtime/ObjectStatus.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine {
using namespace object_lifecycle_detail;

GameSessionObjectSaleTransactions::GameSessionObjectSaleTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort barrier) noexcept
    : m_state{content, world, ai, presentation}, m_barrier(barrier) {}

bool GameSessionObjectSaleTransactions::requestDestroyObject(
    ObjectId object, ObjectDestroyReason reason, uint64_t confirmedTick) {
    return GameSessionObjectLifecycleTransactions{
        m_state.content, m_state.world, m_state.presentation,
        m_barrier}
        .requestDestroyObject(object, reason, confirmedTick);
}

bool GameSessionObjectSaleTransactions::beginObjectSale(
    ObjectId object, PlayerId player, uint64_t confirmedTick,
    bool respectScriptUnsellable)
{
    if (!domainState().contentState().m_active || !domainState().presentationState().m_hasConfirmedFrame || confirmedTick != domainState().presentationState().m_confirmedTick ||
        !object || !domainState().contentState().m_players.get(player) || domainState().worldState().m_objects.isPendingDestroy(object) ||
        domainState().worldState().m_ownership.ownerOf(object) != std::optional<PlayerId>{player}) {
        return false;
    }

    const std::optional<ecs::entity> entity = domainState().worldState().m_objects.entityFromId(object);
    if (!entity || ecs::try_get<ObjectSaleComponent>(domainState().worldState().m_registry, *entity))
        return false;
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(domainState().worldState().m_registry, *entity);
    if (!hasObjectKind(kinds, game::ObjectKindOf::Structure)) return false;
    if (respectScriptUnsellable) {
        const ObjectScriptPanelPolicyComponent* policy =
            ecs::try_get<ObjectScriptPanelPolicyComponent>(
                domainState().worldState().m_registry, *entity);
        if (policy && policy->unsellable) return false;
    }
    // SCRIPT_UNSELLABLE gates the ordinary ControlBar Sell command only.
    // Player::sellEverythingUnderTheSun bypasses that UI policy, so a map
    // script can still dispose of every eligible faction structure.
    const ObjectStatusComponent* existingStatus =
        ecs::try_get<ObjectStatusComponent>(domainState().worldState().m_registry, *entity);
    if (existingStatus && existingStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold) |
            game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) {
        // A foundation is cancelled through CancelConstruction so its builder
        // task, paid cost, terrain/navigation occupancy and lifecycle close as
        // one transaction. Treating it as a finished sellable structure left
        // the construction transaction alive behind a sale animation.
        return false;
    }
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(domainState().worldState().m_registry, *entity);
    if (!type || !type->archetype) return false;

    const uint64_t durationFrames = static_cast<uint64_t>(std::max<uint32_t>(
        1, domainState().contentState().m_objectSimulationRules.logicFramesPerSecond)) * 6u;
    // RefCode completes on start + (1.5s scaffold + 4.5s descent - 1 frame).
    const uint64_t duration = durationFrames == 0 ? 0 : durationFrames - 1u;
    const uint64_t completionTick = confirmedTick >
            std::numeric_limits<uint64_t>::max() - duration
        ? std::numeric_limits<uint64_t>::max()
        : confirmedTick + duration;

    static_cast<void>(ObjectStatusSystem::apply(
        domainState().worldState().m_registry, *entity,
        {.setMask =
             game::objectStatusBit(game::ObjectStatusFlag::Sold) |
             game::objectStatusBit(game::ObjectStatusFlag::Unselectable),
         .confirmedTick = confirmedTick}));

    // ProductionUpdate owns paid jobs, so sale must cancel through the same
    // refund path before the building becomes a delayed lifecycle tombstone.
    static_cast<void>(domainState().worldState().m_objectProduction.cancelAndRefundAll(
        domainState().worldState().m_registry, domainState().worldState().m_objects, domainState().contentState().m_players, object));
    ObjectOrderQueueComponent* orders =
        ecs::try_get<ObjectOrderQueueComponent>(domainState().worldState().m_registry, *entity);
    if (!orders) orders = &ecs::emplace<ObjectOrderQueueComponent>(
        domainState().worldState().m_registry, *entity);
    orders->orders.clear();
    ++orders->revision;
    ++orders->externalRevision;
    if (orders->externalRevision == 0) ++orders->externalRevision;
    static_cast<void>(domainState().aiState().m_objectAI.synchronizeOrderExternalRevision(
        object, orders->externalRevision));
    // TunnelTracker owns one player-wide roster. Selling an entrance only
    // releases that roster when this is the last completed live entrance;
    // ordinary containers still eject immediately at sale admission.
    bool ejectContainmentOnSale = true;
    const ObjectContainmentRuntimeComponent* containmentRuntime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(domainState().worldState().m_registry, *entity);
    const bool sellingTunnel = containmentRuntime &&
        containmentRuntime->plan && std::any_of(
            containmentRuntime->plan->rules.begin(),
            containmentRuntime->plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Tunnel;
            });
    const bool sellingCave = containmentRuntime && containmentRuntime->hasCave &&
        containmentRuntime->plan && std::any_of(
            containmentRuntime->plan->rules.begin(),
            containmentRuntime->plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Cave;
            });
    if (sellingTunnel) {
        const auto tunnels = ecs::view<
            const ObjectIdentityComponent,
            const OwnerComponent,
            const ObjectContainmentRuntimeComponent>(domainState().worldState().m_registry);
        for (const ecs::entity candidate : tunnels) {
            const ObjectIdentityComponent& identity =
                tunnels.template get<const ObjectIdentityComponent>(candidate);
            const OwnerComponent& tunnelOwner =
                tunnels.template get<const OwnerComponent>(candidate);
            const ObjectContainmentRuntimeComponent& tunnelRuntime =
                tunnels.template get<
                    const ObjectContainmentRuntimeComponent>(candidate);
            if (!identity.id || identity.id == object ||
                tunnelOwner.player != player || !tunnelRuntime.plan ||
                !domainState().worldState().m_objects.entityFromId(identity.id)) {
                continue;
            }
            const ObjectStatusComponent* tunnelStatus =
                ecs::try_get<ObjectStatusComponent>(domainState().worldState().m_registry, candidate);
            if (tunnelStatus && tunnelStatus->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::UnderConstruction) |
                    game::objectStatusBit(game::ObjectStatusFlag::Sold))) {
                continue;
            }
            if (std::any_of(
                    tunnelRuntime.plan->rules.begin(),
                    tunnelRuntime.plan->rules.end(),
                    [](const ObjectContainmentRule& rule) {
                        return rule.kind == ObjectContainmentKind::Tunnel;
                    })) {
                ejectContainmentOnSale = false;
                break;
            }
        }
    }
    if (sellingCave) {
        struct CavePassenger final {
            ObjectId object = INVALID_OBJECT_ID;
            uint64_t enteredTick = 0;
            uint64_t entryOrdinal = 0;
        };
        container::Vector<CavePassenger> passengers;
        const auto passengerView = ecs::view<
            const ObjectIdentityComponent,
            const ObjectContainedByComponent>(
                domainState().worldState().m_registry);
        passengers.reserve(passengerView.size_hint());
        for (const ecs::entity passengerEntity : passengerView) {
            const ObjectIdentityComponent& identity =
                passengerView.template get<
                    const ObjectIdentityComponent>(passengerEntity);
            const ObjectContainedByComponent& edge =
                passengerView.template get<
                    const ObjectContainedByComponent>(passengerEntity);
            const std::optional<ecs::entity> actualContainer =
                domainState().worldState().m_objects.entityFromId(
                    edge.container);
            const ObjectContainmentRuntimeComponent* actualRuntime =
                actualContainer
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                      domainState().worldState().m_registry,
                      *actualContainer)
                : nullptr;
            if (!identity.id || !actualRuntime || !actualRuntime->plan ||
                !actualRuntime->hasCave ||
                actualRuntime->caveIndex != containmentRuntime->caveIndex ||
                edge.containmentRuleIndex >=
                    actualRuntime->plan->rules.size() ||
                actualRuntime->plan->rules[edge.containmentRuleIndex].kind !=
                    ObjectContainmentKind::Cave) {
                continue;
            }
            uint64_t entryOrdinal = 0;
            if (const ObjectContainmentComponent* actualContents =
                    ecs::try_get<ObjectContainmentComponent>(
                        domainState().worldState().m_registry,
                        *actualContainer)) {
                const auto record = std::lower_bound(
                    actualContents->objects.begin(),
                    actualContents->objects.end(), identity.id,
                    [](const ObjectContainedObjectRecord& value,
                       ObjectId sought) {
                        return value.object < sought;
                    });
                if (record != actualContents->objects.end() &&
                    record->object == identity.id) {
                    entryOrdinal = record->entryOrdinal;
                }
            }
            passengers.push_back({
                .object = identity.id,
                .enteredTick = edge.confirmedEnteredTick,
                .entryOrdinal = entryOrdinal,
            });
        }
        std::sort(
            passengers.begin(), passengers.end(),
            [](const CavePassenger& left,
               const CavePassenger& right) noexcept {
                if (left.enteredTick != right.enteredTick)
                    return left.enteredTick < right.enteredTick;
                if (left.entryOrdinal != right.entryOrdinal)
                    return left.entryOrdinal < right.entryOrdinal;
                return left.object < right.object;
            });
        GameSessionContainmentTransactions containmentTransactions{
            domainState().worldState().m_registry,
            domainState().worldState().m_objects,
            domainState().worldState().m_objectSimulation,
            domainState().worldState().m_spatialIndex,
            domainState().worldState().m_objectTeams,
            domainState().contentState().m_players,
            domainState().contentState().m_contentSnapshot};
        for (const CavePassenger& passenger : passengers) {
            static_cast<void>(containmentTransactions.requestPlayerExit(
                object, passenger.object, confirmedTick,
                domainState().worldState().m_ownership,
                domainState().aiState().m_objectAI));
        }
        // CaveContain inherits OpenContain::onSelling: every network rider is
        // put into AI_EXIT and the ordinary door/ExitDelay serializes egress.
        // Never replace that with the force EjectAll primitive.
        ejectContainmentOnSale = false;
    }
    if (ejectContainmentOnSale) {
        static_cast<void>(domainState().worldState().m_objectSimulation.requestContainment(
            domainState().worldState().m_registry, domainState().worldState().m_objects,
            {.kind = ObjectContainmentRequestKind::EjectAll,
             .container = object,
             .confirmedTick = confirmedTick,
             .force = true}, &domainState().contentState().m_players, &domainState().contentState().m_contentSnapshot));
    }

    // MinefieldBehavior destroys mines produced by the structure as soon as
    // selling begins.  Resolve this through the stable producer relation,
    // never by an ECS entity pointer retained by the mine.
    container::Vector<ObjectId> producedMines;
    const auto mineView = ecs::view<
        const ObjectIdentityComponent, const ObjectProducerComponent,
        const ObjectKindOfComponent>(domainState().worldState().m_registry);
    for (const ecs::entity mineEntity : mineView) {
        const ObjectIdentityComponent& identity =
            mineView.template get<const ObjectIdentityComponent>(mineEntity);
        const ObjectProducerComponent& produced =
            mineView.template get<const ObjectProducerComponent>(mineEntity);
        const ObjectKindOfComponent& mineKinds =
            mineView.template get<const ObjectKindOfComponent>(mineEntity);
        if (identity.id && produced.producer == object &&
            hasObjectKind(&mineKinds, game::ObjectKindOf::Mine) &&
            domainState().worldState().m_objects.entityFromId(identity.id)) {
            producedMines.push_back(identity.id);
        }
    }
    std::sort(producedMines.begin(), producedMines.end());
    for (const ObjectId mine : producedMines) {
        static_cast<void>(requestDestroyObject(
            mine, ObjectDestroyReason::PlayerSell, confirmedTick));
    }

    // ParkingPlaceBehavior::killAllParkedUnits runs at sale admission. Both
    // land airfields and flight decks expose stable ObjectId space arrays.
    container::Vector<ObjectId> parkedAircraft;
    if (const ObjectAirfieldComponent* airfield =
            ecs::try_get<ObjectAirfieldComponent>(domainState().worldState().m_registry, *entity)) {
        for (const ObjectAirfieldParkingRuntime& parking :
             airfield->parkingPlaces) {
            for (const ObjectId aircraft : parking.spaces)
                if (aircraft) parkedAircraft.push_back(aircraft);
        }
        for (const ObjectAirfieldFlightDeckRuntime& deck : airfield->flightDecks) {
            for (const ObjectId aircraft : deck.spaces)
                if (aircraft) parkedAircraft.push_back(aircraft);
        }
    }
    std::sort(parkedAircraft.begin(), parkedAircraft.end());
    parkedAircraft.erase(
        std::unique(parkedAircraft.begin(), parkedAircraft.end()),
        parkedAircraft.end());
    for (const ObjectId aircraft : parkedAircraft) {
        if (domainState().worldState().m_objects.entityFromId(aircraft)) {
            static_cast<void>(requestDestroyObject(
                aircraft, ObjectDestroyReason::PlayerSell, confirmedTick));
        }
    }

    ecs::emplace<ObjectSaleComponent>(
        domainState().worldState().m_registry, *entity,
        ObjectSaleComponent{
            .startedTick = confirmedTick,
            .completionTick = completionTick,
            .revision = 1,
        });
    markObjectDirty(
        domainState().worldState().m_registry, *entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    return true;
}

bool GameSessionConfirmedCommandPort::sellObject(
    ObjectId object, PlayerId player, uint64_t confirmedTick) {
    if (objectForbidsPlayerCommands(object)) return true;
    return m_sale.beginObjectSale(
        object, player, confirmedTick, true);
}

bool GameSessionConfirmedCommandPort::cancelConstruction(
    ObjectId object, PlayerId player, uint64_t confirmedTick) {
    if (objectForbidsPlayerCommands(object)) return true;
    return productionTransactions().cancelConstruction(
        object, player, confirmedTick);
}

void GameSessionObjectSaleTransactions::settleDueSales(
    uint64_t confirmedTick)
{
    if (!domainState().contentState().m_active ||
        !domainState().presentationState().m_hasConfirmedFrame ||
        confirmedTick != domainState().presentationState().m_confirmedTick) {
        return;
    }
    struct DueSale final {
        ObjectId object = INVALID_OBJECT_ID;
    };
    container::Vector<DueSale> due;
    const auto view = ecs::view<
        const ObjectIdentityComponent, const ObjectSaleComponent>(domainState().worldState().m_registry);
    due.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectSaleComponent& sale =
            view.template get<const ObjectSaleComponent>(entity);
        const bool live = identity.id &&
            domainState().worldState().m_objects.entityFromId(identity.id);
        if (!live) continue;

        // Construction percent is a confirmed clock projection rather than a
        // mutating component field.  Publish its changing body sink every
        // tick, and rebuild Draw conditions exactly when the descent crosses
        // zero so the main body can select Model=None while fence/scaffold
        // channels retain their authored teardown transitions.
        uint8_t dirty = objectDirtyBit(
            ObjectDirtyDomain::RenderExtraction);
        if (confirmedTick > sale.startedTick &&
            !sale.soldVisualActive(confirmedTick - 1u) &&
            sale.soldVisualActive(confirmedTick)) {
            dirty |= objectDirtyBit(ObjectDirtyDomain::ModelCondition);
        }
        markObjectDirty(
            domainState().worldState().m_registry, entity, dirty);

        if (sale.completionTick <= confirmedTick) {
            due.push_back({identity.id});
        }
    }
    std::sort(due.begin(), due.end(), [](const DueSale& left,
                                         const DueSale& right) {
        return left.object < right.object;
    });
    for (const DueSale& sale : due) {
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(sale.object);
        if (!entity) continue;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(domainState().worldState().m_registry, *entity);
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(domainState().worldState().m_registry, *entity);
        const PlayerState* payer = owner ? domainState().contentState().m_players.get(owner->player) : nullptr;
        int64_t refund = 0;
        if (payer && type && type->archetype) {
            refund = static_cast<int64_t>(
                type->archetype->templateData.refundValue);
            if (refund == 0) {
                const int64_t buildCost = std::max<int64_t>(
                    0, calculateObjectBuildCost(
                        *type->archetype, *payer, domainState().worldState().m_registry, domainState().worldState().m_objects));
                refund = domainState().contentState().m_objectSimulationRules.economy.applySellPercentage(
                    buildCost);
            }
        }
        if (owner && refund > 0)
            static_cast<void>(domainState().contentState().m_players.adjustCash(owner->player, refund));
        static_cast<void>(requestDestroyObject(
            sale.object, ObjectDestroyReason::PlayerSell, confirmedTick));
    }
}

} // namespace engine
