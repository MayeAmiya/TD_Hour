#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/economy/ObjectProductionDetail.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/command/CommandBarOverrides.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>


namespace engine {

using namespace production_detail;

void ObjectProductionSystem::initializeObject(ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype) return;

    if (const container::SharedPtr<const game::ObjectProductionExitPlan>& exitPlan =
            templateComponent->archetype->productionExitPlan) {
        ObjectProductionExitComponent exit{
            .plan = exitPlan,
            .initialBurstRemaining = exitPlan->initialBurst,
        };
        if (ObjectProductionExitComponent* existing =
                ecs::try_get<ObjectProductionExitComponent>(registry, entity)) {
            *existing = std::move(exit);
        } else {
            ecs::emplace<ObjectProductionExitComponent>(registry, entity,
                                                         std::move(exit));
        }
    }
    if (!templateComponent->archetype->productionPlan) return;

    ObjectProductionComponent component{
        .plan = templateComponent->archetype->productionPlan,
        .exitPlan = templateComponent->archetype->productionExitPlan,
    };
    if (ObjectProductionComponent* existing =
            ecs::try_get<ObjectProductionComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectProductionComponent>(registry, entity, std::move(component));
    }
}

std::optional<ObjectProductionExitReservation>
ObjectProductionSystem::reserveExternalExit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId host,
    uint64_t confirmedTick) const {
    if (!host || lifecycle.isPendingDestroy(host)) return std::nullopt;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(host);
    if (!entity) return std::nullopt;
    ObjectProductionExitComponent* runtime =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    if (!runtime) return std::nullopt;
    return reserveExitRuntime(registry, lifecycle, content, *entity,
                              confirmedTick, *runtime);
}

bool ObjectProductionSystem::commitExternalExit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId host, ObjectProductionExitReservation reservation,
    ObjectId spawnedObject, uint64_t confirmedTick,
    uint32_t framesPerSecond) const {
    if (!host || !reservation || lifecycle.isPendingDestroy(host)) {
        return false;
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(host);
    if (!entity) return false;
    ObjectProductionExitComponent* runtime =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    return runtime && commitExitReservation(
        *runtime, reservation, spawnedObject, confirmedTick, framesPerSecond);
}

void ObjectProductionSystem::releaseExternalExit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId host,
    ObjectProductionExitReservation reservation) const noexcept {
    if (!host || !reservation) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(host);
    if (!entity) return;
    ObjectProductionExitComponent* runtime =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    if (runtime) releaseExitReservation(*runtime, reservation);
}

bool ObjectProductionSystem::acknowledgeSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId producer,
    uint32_t productionId, uint32_t quantityIndex, ObjectId spawnedObject,
    ObjectProductionExitReservation reservation, uint64_t confirmedTick,
    uint32_t framesPerSecond) const {
    if (!producer || lifecycle.isPendingDestroy(producer)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return false;
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    ObjectProductionExitComponent* exitRuntime =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    if (!component || !exitRuntime || component->jobs.empty()) return false;
    ObjectProductionJob& job = component->jobs.front();
    if (job.kind != ObjectProductionJobKind::Unit || job.productionId != productionId ||
        !job.constructionComplete ||
        quantityIndex != job.quantityProduced || quantityIndex >= job.quantityTotal) {
        return false;
    }
    if (!commitExitReservation(*exitRuntime, reservation, spawnedObject,
                               confirmedTick, framesPerSecond)) {
        return false;
    }
    ++job.quantityProduced;
    if (job.quantityProduced >= job.quantityTotal) {
        component->jobs.erase(component->jobs.begin());
    } else if (component->exitPlan &&
               (component->exitPlan->kind ==
                    game::ObjectProductionExitKind::AirfieldParking ||
                component->exitPlan->kind ==
                    game::ObjectProductionExitKind::FlightDeck)) {
        // Every aircraft in a quantity batch owns a distinct parking space
        // and therefore a distinct door. Select the next free slot only
        // after the previous spawn has committed its concrete ObjectId.
        job.exitDoorAssigned = false;
    }
    ++component->revision;
    return true;
}

void ObjectProductionSystem::releaseSpawnReservation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId producer, ObjectProductionExitReservation reservation) const {
    if (!producer || !reservation) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(producer);
    if (!entity) return;
    ObjectProductionExitComponent* runtime =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    if (runtime) releaseExitReservation(*runtime, reservation);
}

bool ObjectProductionSystem::acknowledgePlayerUpgrade(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId producer,
    UpgradeContentId upgrade) const {
    if (!producer || !upgrade || lifecycle.isPendingDestroy(producer)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return false;
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || component->jobs.empty()) return false;
    const ObjectProductionJob& job = component->jobs.front();
    if ((job.kind != ObjectProductionJobKind::PlayerUpgrade &&
         job.kind != ObjectProductionJobKind::ObjectUpgrade) ||
        !job.constructionComplete ||
        job.upgrade != upgrade) {
        return false;
    }
    component->jobs.erase(component->jobs.begin());
    ++component->revision;
    return true;
}

} // namespace engine
