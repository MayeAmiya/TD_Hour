#include "core/container/container_types.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/contracts/ObjectDeathReaction.h"

#include "game/object/runtime/ObjectStatus.h"

namespace engine {
namespace {

using HealthScalar = ObjectHealthComponent::Scalar;

struct InitialBodyHealth final {
    HealthScalar current{};
    HealthScalar initial{};
    HealthScalar maximum{};
};

[[nodiscard]] InitialBodyHealth resolveInitialBodyHealth(
    const ObjectSpawnRequest& request, const game::ThingTemplate& thingTemplate) noexcept {
    const game::ObjectBodyTemplate& body = thingTemplate.body;
    // InactiveBody is logically dead/indestructible yet remains a live ECS
    // entity for scenery, helpers and map infrastructure. Its Body interface
    // reports zero health; it must not silently become a synthetic 1/100 HP
    // ActiveBody just because the modern entity exists.
    if (!body.acceptsDamage()) return {};

    InitialBodyHealth result{
        .current = body.initialHealthFixed,
        .initial = body.initialHealthFixed,
        .maximum = body.maximumHealthFixed,
    };
    if (request.maximumHealthOverride &&
        *request.maximumHealthOverride >= HealthScalar{}) {
        // Object::updateObjValuesFromMapProperties calls
        // Body::setMaxHealth(..., SAME_CURRENTHEALTH) before InitialHealth.
        // That changes the initial baseline to the override and clips only a
        // current value above the new maximum.
        result.maximum = *request.maximumHealthOverride;
        result.initial = result.maximum;
        if (result.current > result.maximum) result.current = result.maximum;
    }
    if (request.initialHealthFraction) {
        // Map objectInitialHealth is a percentage, not raw HP. ActiveBody's
        // internalChangeHealth performs the [0,max] clamp for this explicit
        // mutation; do not apply that clamp to untouched template data.
        result.current = std::clamp(
            result.initial * *request.initialHealthFraction,
            HealthScalar{}, result.maximum);
    }
    return result;
}

[[nodiscard]] ObjectBodyDamageState initialDamageState(HealthScalar current, HealthScalar maximum,
                                                        const ObjectSimulationRules& rules) noexcept {
    const HealthScalar zero{};
    if (current <= zero || maximum <= zero) return ObjectBodyDamageState::Rubble;
    const HealthScalar fraction = current / maximum;
    // ActiveBody::calcDamageState uses strict `>` comparisons. Exact 50% and
    // 10% therefore enter the worse state, matching the legacy thresholds.
    if (fraction > rules.unitDamagedThresholdFixed) {
        return ObjectBodyDamageState::Pristine;
    }
    if (fraction > rules.unitReallyDamagedThresholdFixed) {
        return ObjectBodyDamageState::Damaged;
    }
    return ObjectBodyDamageState::ReallyDamaged;
}

[[nodiscard]] ObjectGeometryShape toGeometryShape(game::ObjectGeometryType type) noexcept {
    switch (type) {
    case game::ObjectGeometryType::Sphere: return ObjectGeometryShape::Sphere;
    case game::ObjectGeometryType::Cylinder: return ObjectGeometryShape::Cylinder;
    case game::ObjectGeometryType::Box: return ObjectGeometryShape::Box;
    }
    return ObjectGeometryShape::Sphere;
}

[[nodiscard]] bool hasTreeSwayClientUpdate(const game::ThingTemplate& thingTemplate) noexcept {
    return std::any_of(thingTemplate.modules.begin(), thingTemplate.modules.end(),
                       [](const game::ModuleData& module) {
        return module.category == game::ModuleRecipeCategory::ClientUpdate &&
               module.moduleClass == "SwayClientUpdate";
    });
}

} // namespace

ObjectLifecycle::ObjectLifecycle(ecs::registry& registry) noexcept
    : m_registry(registry) {}

void ObjectLifecycle::reset(bool clearRegistry) noexcept {
    if (clearRegistry) m_registry.clear();
    m_ids.reset();
    m_entitiesById.clear();
    m_pendingDestroys.clear();
    m_events.clear();
}

ObjectSpawnResult ObjectLifecycle::create(const ObjectSpawnRequest& request,
                                          const game::ThingTemplate& thingTemplate,
                                          const ObjectSimulationRules& rules) {
    if (request.templateName.empty()) return {};
    const std::optional<ObjectId> allocated = m_ids.tryAllocate();
    if (!allocated) return {};

    const ObjectId object = *allocated;
    const ecs::entity entity = ecs::create(m_registry);
    const bool indexed = m_entitiesById.emplace(object, entity).second;
    if (!indexed) {
        // ObjectIdAllocator is the sole issuer, so this is a hard invariant
        // failure rather than a recoverable duplicate that could be silently
        // redirected to another entity.
        ecs::destroy(m_registry, entity);
        return {};
    }

    ecs::emplace<ObjectIdentityComponent>(m_registry, entity, ObjectIdentityComponent{.id = object});
    ecs::emplace<ObjectLifecycleComponent>(m_registry, entity, ObjectLifecycleComponent{
        .origin = request.origin,
        .phase = ObjectLifecyclePhase::Alive,
        .createdAtTick = request.confirmedTick,
    });
    ecs::emplace<ThingTemplateComponent>(m_registry, entity, ThingTemplateComponent{
        .name = request.templateName,
    });
    ecs::emplace<OwnerComponent>(m_registry, entity, OwnerComponent{.player = request.owner});
    if (request.primaryTeam) {
        ecs::emplace<PrimaryTeamComponent>(m_registry, entity,
                                           PrimaryTeamComponent{.team = request.primaryTeam});
        ecs::emplace<OriginalOwnershipComponent>(m_registry, entity,
                                                  OriginalOwnershipComponent{
                                                      .owner = request.owner,
                                                      .team = request.primaryTeam,
                                                  });
    }
    ObjectFixedTransformComponent fixedTransform = request.transform;
    fixedTransform.authoritative = true;
    ecs::emplace<ObjectFixedTransformComponent>(
        m_registry, entity,
        fixedTransform);
    // Float Transform is a presentation/legacy compatibility projection of
    // the already-quantized authoritative value. No later simulation owner
    // may use it to reconstruct the fixed transform.
    ecs::emplace<TransformComponent>(m_registry, entity, TransformComponent{
        .x = fixedTransform.position.x.to_float(),
        .y = fixedTransform.position.y.to_float(),
        .z = fixedTransform.position.z.to_float(),
        .rotation = fixedTransform.yawRadians.to_float(),
    });
    if (request.initialPathfindLayer) {
        ecs::emplace<ObjectTerrainLayerComponent>(
            m_registry, entity,
            ObjectTerrainLayerComponent{
                .pathfindLayer = *request.initialPathfindLayer,
                .lastChangedTick = request.confirmedTick,
            });
    }
    if (request.producedBy)
    {
        ecs::emplace<ObjectProducedByComponent>(m_registry, entity, *request.producedBy);
    }
    if (request.producer)
    {
        ecs::emplace<ObjectProducerComponent>(
            m_registry, entity,
            ObjectProducerComponent{.producer = request.producer});
    }
    if (request.constructedBy &&
        (thingTemplate.body.kind == game::ObjectBodyKind::Structure ||
         thingTemplate.body.kind == game::ObjectBodyKind::HiveStructure)) {
        ecs::emplace<ObjectConstructedByComponent>(
            m_registry, entity,
            ObjectConstructedByComponent{
                .constructorObject = request.constructedBy,
            });
    }
    InitialBodyHealth initialHealth = resolveInitialBodyHealth(request, thingTemplate);
    if (request.startsUnderConstruction && thingTemplate.body.acceptsDamage()) {
        // RefCode publishes a newly placed construction site at exactly one
        // hit point. Construction progress is then the only writer that
        // raises it toward the template maximum.
        initialHealth.current = std::min(
            HealthScalar{int32_t{1}}, initialHealth.maximum);
        // ActiveBody::internalChangeHealth lowers only current/previous HP;
        // InitialHealth remains the authored Body value for later instance,
        // capture and max-health operations.
    }
    ObjectHealthComponent health{
        .currentFixed = initialHealth.current,
        .previousFixed = initialHealth.current,
        .maximumFixed = initialHealth.maximum,
        .initialFixed = initialHealth.initial,
        .subdualDamageCapFixed = thingTemplate.body.subdualDamageCapFixed,
        .subdualDamageHealAmountFixed =
            thingTemplate.body.subdualDamageHealAmountFixed,
        .secondLifeMaximumHealthFixed =
            thingTemplate.body.undeadSecondLifeMaximumHealthFixed,
        .subdualDamageHealIntervalMilliseconds = thingTemplate.body.subdualDamageHealIntervalMilliseconds,
        .damageState = thingTemplate.body.acceptsDamage()
            ? initialDamageState(initialHealth.current,
                                 initialHealth.maximum, rules)
            : ObjectBodyDamageState::Pristine,
        .acceptsDamage = thingTemplate.body.acceptsDamage(),
        .clampsToOneHealth = thingTemplate.body.clampsToOneHealth(),
        .onlyUnresistableCanKill = thingTemplate.body.onlyUnresistableCanKill(),
        .hasSecondLife = thingTemplate.body.kind == game::ObjectBodyKind::Undead,
        .effectivelyDead = !thingTemplate.body.acceptsDamage() ||
            initialHealth.current <= HealthScalar{},
    };
    ecs::emplace<ObjectHealthComponent>(m_registry, entity, std::move(health));
    const ObjectSpawnGeometryOverride geometryValues =
        request.geometryOverride.value_or(ObjectSpawnGeometryOverride{
            .shape = toGeometryShape(thingTemplate.geometry.type),
            .isSmall = thingTemplate.geometry.isSmall,
            .majorRadius = thingTemplate.geometry.majorRadiusFixed,
            .minorRadius = thingTemplate.geometry.minorRadiusFixed,
            .height = thingTemplate.geometry.heightFixed,
            .boundingCircleRadius =
                thingTemplate.geometry.boundingCircleRadiusFixed,
            .boundingSphereRadius =
                thingTemplate.geometry.boundingSphereRadiusFixed,
        });
    ObjectGeometryComponent geometry{
        .shape = geometryValues.shape,
        .isSmall = geometryValues.isSmall,
        .majorRadiusFixed = geometryValues.majorRadius,
        .minorRadiusFixed = geometryValues.minorRadius,
        .heightFixed = geometryValues.height,
        .boundingCircleRadiusFixed = geometryValues.boundingCircleRadius,
        .boundingSphereRadiusFixed = geometryValues.boundingSphereRadius,
    };
    ecs::emplace<ObjectGeometryComponent>(
        m_registry, entity, std::move(geometry));
    if (!thingTemplate.defaultW3dModel.empty() ||
        !thingTemplate.drawVisualChannels.empty()) {
        RenderModelComponent renderModel{
            .modelAsset = thingTemplate.defaultW3dModel,
            // This is authored module capability only. Session-frozen Feature
            // quality gates the shared TreeSway presentation state, so object
            // construction must not bake a process-global preference into an
            // otherwise immutable component.
            .treeSwayEnabled = hasTreeSwayClientUpdate(thingTemplate),
            .boundingRadius =
                thingTemplate.geometry.boundingSphereRadiusFixed.to_float(),
        };
        renderModel.channels.reserve(thingTemplate.drawVisualChannels.size());
        for (size_t channelIndex = 0;
             channelIndex < thingTemplate.drawVisualChannels.size();
             ++channelIndex) {
            renderModel.channels.push_back(RenderModelChannelState{
                .channelIndex = static_cast<uint32_t>(channelIndex),
            });
        }
        ecs::emplace<RenderModelComponent>(
            m_registry, entity, std::move(renderModel));
        VehicleDrawPresentationComponent vehicleDraw;
        for (size_t channelIndex = 0;
             channelIndex < thingTemplate.drawVisualChannels.size();
             ++channelIndex) {
            if (!thingTemplate.drawVisualChannels[channelIndex]
                     .vehicleDraw.enabled()) {
                continue;
            }
            vehicleDraw.channels.push_back({
                .channelIndex = static_cast<uint32_t>(channelIndex),
                .previousYaw = request.transform.yawRadians.to_float(),
            });
        }
        if (!vehicleDraw.channels.empty()) {
            ecs::emplace<VehicleDrawPresentationComponent>(
                m_registry, entity, std::move(vehicleDraw));
        }
    }
    if (request.mapProvenance) {
        ecs::emplace<MapObjectProvenanceComponent>(m_registry, entity, *request.mapProvenance);
    }
    const game::ObjectStatusMask initialStatusMask =
        static_cast<game::ObjectStatusMask>(request.initialStatusMask) |
        (!thingTemplate.isSelectable
             ? game::objectStatusBit(game::ObjectStatusFlag::Unselectable)
             : game::ObjectStatusMask{}) |
        (request.startsUnderConstruction
             ? game::objectStatusBit(
                   game::ObjectStatusFlag::UnderConstruction)
             : game::ObjectStatusMask{});
    if (initialStatusMask != 0) {
        static_cast<void>(ObjectStatusSystem::apply(
            m_registry, entity,
            {.setMask = initialStatusMask,
             .confirmedTick = request.confirmedTick}));
    }

    // The entity becomes visible to incremental world consumers only after
    // every required creation component has been installed.
    markObjectDirty(m_registry, entity, kObjectDirtyAll);

    m_events.push_back({
        .kind = ObjectLifecycleEventKind::Created,
        .object = object,
        .owner = request.owner,
        .origin = request.origin,
        .confirmedTick = request.confirmedTick,
        .templateName = request.templateName,
    });
    return {.object = object, .entity = entity};
}

bool ObjectLifecycle::abortUnpublishedCreate(ObjectId object, size_t eventCheckpoint) {
    // GameSession uses this only to roll back an entity which has not crossed
    // its lifecycle publication boundary yet.  Do not implement that path by
    // requesting a normal destroy and flushing: a global flush would commit
    // unrelated pending destroys, and takeEvents() would erase unrelated
    // observers' queued events.
    if (!object || eventCheckpoint > m_events.size()) return false;

    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end() || !m_registry.valid(found->second)) return false;
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive) return false;

    const auto eventSuffix = m_events.begin() +
        static_cast<container::Vector<ObjectLifecycleEvent>::difference_type>(eventCheckpoint);
    const bool createdInSuffix = std::any_of(eventSuffix, m_events.end(), [object](
        const ObjectLifecycleEvent& event) {
        return event.kind == ObjectLifecycleEventKind::Created && event.object == object;
    });
    if (!createdInSuffix) return false;

    // Retain every event that preceded the creation checkpoint, and retain
    // any unrelated event appended while this transaction was initializing
    // components.  An ObjectId has one entity for the session, so all suffix
    // events carrying this ObjectId belong exclusively to this aborted create.
    m_events.erase(std::remove_if(eventSuffix, m_events.end(), [object](
        const ObjectLifecycleEvent& event) { return event.object == object; }),
        m_events.end());
    // A newly-created entity must still be Alive, so this list normally has
    // no matching entry.  Remove only a defensive matching entry instead of
    // calling the global flushRequestedDestroys() path.
    m_pendingDestroys.erase(std::remove_if(m_pendingDestroys.begin(), m_pendingDestroys.end(),
        [object](const PendingDestroy& pending) { return pending.object == object; }),
        m_pendingDestroys.end());
    ecs::destroy(m_registry, found->second);
    m_entitiesById.erase(found);
    // ObjectIdAllocator is intentionally monotonic for a session.  Retaining
    // this consumed ID prevents an aborted create from reusing an identity a
    // transient internal system may already have observed.
    return true;
}

bool ObjectLifecycle::requestDestroy(ObjectId object, ObjectDestroyReason reason,
                                     uint64_t confirmedTick) {
    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end()) return false;
    ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    OwnerComponent* owner = ecs::try_get<OwnerComponent>(m_registry, found->second);
    ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(m_registry, found->second);
    if (!lifecycle || lifecycle->phase == ObjectLifecyclePhase::PendingDestroy) return false;
    markObjectDirty(m_registry, found->second, kObjectDirtyAll);

    lifecycle->phase = ObjectLifecyclePhase::PendingDestroy;
    // RefCode's GameLogic::destroyObject marks OBJECT_STATUS_DESTROYED as
    // soon as the deferred destroy is requested. KeepObjectDie intentionally
    // never reaches this path, so dead rubble remains effectively dead while
    // retaining its live ObjectId and unmodified status mask.
    static_cast<void>(ObjectStatusSystem::apply(m_registry, found->second,
        {.setMask= game::objectStatusBit(game::ObjectStatusFlag::Destroyed), .confirmedTick = confirmedTick}));
    m_pendingDestroys.push_back({.object = object, .reason = reason, .confirmedTick = confirmedTick});
    m_events.push_back({
        .kind = ObjectLifecycleEventKind::DestroyRequested,
        .object = object,
        .owner = owner ? owner->player : INVALID_PLAYER_ID,
        .origin = lifecycle->origin,
        .destroyReason = reason,
        .confirmedTick = confirmedTick,
        .templateName = templateComponent ? templateComponent->name : container::String{},
    });
    return true;
}

size_t ObjectLifecycle::flushRequestedDestroys() {
    size_t destroyed = 0;
    for (const PendingDestroy& pending : m_pendingDestroys) {
        const auto found = m_entitiesById.find(pending.object);
        if (found == m_entitiesById.end()) continue;
        const ecs::entity entity = found->second;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(m_registry, entity);
        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(m_registry, entity);
        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_registry, entity);
        if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::PendingDestroy) continue;

        // Collect every value needed by downstream consumers before EnTT
        // destroys component storage. No event carries the entity itself.
        ObjectLifecycleEvent event{
            .kind = ObjectLifecycleEventKind::Destroyed,
            .object = pending.object,
            .owner = owner ? owner->player : INVALID_PLAYER_ID,
            .origin = lifecycle->origin,
            .destroyReason = pending.reason,
            .confirmedTick = pending.confirmedTick,
            .templateName = templateComponent ? templateComponent->name : container::String{},
        };
        ecs::destroy(m_registry, entity);
        m_entitiesById.erase(found);
        m_events.push_back(std::move(event));
        ++destroyed;
    }
    m_pendingDestroys.clear();
    return destroyed;
}

bool ObjectLifecycle::changeOwner(ObjectId object, PlayerId newOwner,
                                  uint64_t confirmedTick,
                                  bool allowPendingDestroy) {
    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end()) return false;
    ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    OwnerComponent* owner = ecs::try_get<OwnerComponent>(m_registry, found->second);
    ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(m_registry, found->second);
    const bool mutablePhase = lifecycle &&
        (lifecycle->phase == ObjectLifecyclePhase::Alive ||
         (allowPendingDestroy &&
          lifecycle->phase == ObjectLifecyclePhase::PendingDestroy));
    if (!mutablePhase || !owner || owner->player == newOwner) {
        return false;
    }
    const PlayerId previousOwner = owner->player;
    owner->player = newOwner;
    markObjectDirty(
        m_registry, found->second,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    m_events.push_back({
        .kind = ObjectLifecycleEventKind::OwnershipChanged,
        .object = object,
        .previousOwner = previousOwner,
        .owner = newOwner,
        .origin = lifecycle->origin,
        .confirmedTick = confirmedTick,
        .templateName = templateComponent ? templateComponent->name : container::String{},
    });
    return true;
}

std::optional<ecs::entity> ObjectLifecycle::entityFromId(ObjectId object) const noexcept {
    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end()) return std::nullopt;
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive) return std::nullopt;
    return found->second;
}

std::optional<ecs::entity>
ObjectLifecycle::entityFromIdIncludingPending(ObjectId object) const noexcept {
    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end() || !m_registry.valid(found->second))
        return std::nullopt;
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    if (!lifecycle ||
        (lifecycle->phase != ObjectLifecyclePhase::Alive &&
         lifecycle->phase != ObjectLifecyclePhase::PendingDestroy))
    {
        return std::nullopt;
    }
    return found->second;
}

ObjectId ObjectLifecycle::objectIdFromEntity(ecs::entity entity) const noexcept {
    if (entity == ecs::null || !m_registry.valid(entity)) return INVALID_OBJECT_ID;
    const ObjectIdentityComponent* identity = ecs::try_get<ObjectIdentityComponent>(m_registry, entity);
    const ObjectLifecycleComponent* lifecycle = ecs::try_get<ObjectLifecycleComponent>(m_registry, entity);
    return identity && lifecycle && lifecycle->phase == ObjectLifecyclePhase::Alive
        ? identity->id
        : INVALID_OBJECT_ID;
}

bool ObjectLifecycle::isPendingDestroy(ObjectId object) const noexcept {
    const auto found = m_entitiesById.find(object);
    if (found == m_entitiesById.end()) return false;
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(m_registry, found->second);
    return lifecycle && lifecycle->phase == ObjectLifecyclePhase::PendingDestroy;
}

container::Vector<ObjectLifecycleEvent> ObjectLifecycle::takeEvents() {
    container::Vector<ObjectLifecycleEvent> result = std::move(m_events);
    m_events.clear();
    return result;
}

} // namespace engine
