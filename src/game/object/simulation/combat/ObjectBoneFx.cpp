#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectBoneFx.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"

#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kNeverDue = std::numeric_limits<uint64_t>::max();

[[nodiscard]] uint64_t mixSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t entryKey(
    uint64_t sessionSeed, ObjectId object, uint32_t authoredOrder,
    ObjectBodyDamageState state, const game::ObjectBoneFxEntry& entry,
    uint32_t activationEpoch, uint32_t sampleOrdinal) noexcept {
    uint64_t value = sessionSeed ^ 0x424f4e4546585550ull; // "BONEFXUP"
    value ^= static_cast<uint64_t>(object.value) * 0x9e3779b97f4a7c15ull;
    value ^= (static_cast<uint64_t>(authoredOrder) + 1u) << 19u;
    value ^= (static_cast<uint64_t>(state) + 1u) << 11u;
    value ^= (static_cast<uint64_t>(entry.slot) + 1u) << 5u;
    value ^= static_cast<uint64_t>(entry.kind) + 1u;
    value ^= static_cast<uint64_t>(activationEpoch) * 0x94d049bb133111ebull;
    value ^= static_cast<uint64_t>(sampleOrdinal) * 0xbf58476d1ce4e5b9ull;
    return mixSeed(value);
}

[[nodiscard]] uint64_t sampleDelayFrames(
    const ObjectSimulationRules& rules, uint64_t sessionSeed, ObjectId object,
    uint32_t authoredOrder, ObjectBodyDamageState state,
    const game::ObjectBoneFxEntry& entry, uint32_t activationEpoch,
    uint32_t sampleOrdinal) noexcept {
    const math::q32_32 framesPerSecond{
        static_cast<int32_t>(std::max<uint32_t>(1, rules.logicFramesPerSecond))};
    const math::q32_32 thousand{int32_t{1000}};
    const int64_t minimum = math::q32_32::max(
        {}, entry.minimumDelayMilliseconds * framesPerSecond / thousand).raw();
    const int64_t maximum = math::q32_32::max(
        {}, entry.maximumDelayMilliseconds * framesPerSecond / thousand).raw();
    if (maximum <= minimum) {
        return static_cast<uint64_t>(minimum >> 32u);
    }
    const uint64_t span = static_cast<uint64_t>(maximum - minimum);
    const uint64_t sampled = static_cast<uint64_t>(minimum) +
        entryKey(sessionSeed, object, authoredOrder, state, entry,
                 activationEpoch, sampleOrdinal) % (span + 1u);
    return sampled >> 32u;
}

[[nodiscard]] uint64_t saturatedAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] uint64_t particleGroup(uint32_t authoredOrder) noexcept {
    return ((static_cast<uint64_t>(authoredOrder) + 1u) << 8u) | 0x40u;
}

[[nodiscard]] uint64_t maskFor(
    const game::ObjectBoneFxRule& rule,
    game::ObjectBoneFxPayloadKind kind) noexcept {
    switch (kind) {
    case game::ObjectBoneFxPayloadKind::FxList:
        return rule.fxDamageTypes;
    case game::ObjectBoneFxPayloadKind::ObjectCreationList:
        return rule.oclDamageTypes;
    case game::ObjectBoneFxPayloadKind::ParticleSystem:
        return rule.particleDamageTypes;
    }
    return 0;
}

[[nodiscard]] ObjectTransitionDamageFxEventKind eventKindFor(
    game::ObjectBoneFxPayloadKind kind) noexcept {
    switch (kind) {
    case game::ObjectBoneFxPayloadKind::FxList:
        return ObjectTransitionDamageFxEventKind::FxList;
    case game::ObjectBoneFxPayloadKind::ObjectCreationList:
        return ObjectTransitionDamageFxEventKind::ObjectCreationList;
    case game::ObjectBoneFxPayloadKind::ParticleSystem:
        return ObjectTransitionDamageFxEventKind::ParticleSystem;
    }
    return ObjectTransitionDamageFxEventKind::FxList;
}

[[nodiscard]] ObjectTransitionDamageFxAnchor snapshotAnchor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    ObjectTransitionDamageFxAnchor result;
    if (const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity)) {
        result.position = readAuthoritativeObjectPosition(
            registry, entity, *transform);
        result.yawRadians = readAuthoritativeObjectYaw(
            registry, entity, *transform);
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->ownsAttitude) {
        result.rollRadians = physics->roll;
        result.pitchRadians = physics->pitch;
        result.yawRadians = physics->yaw;
    }
    return result;
}

void initializeRuleState(
    ObjectBoneFxRuntimeRule& runtime, const game::ObjectBoneFxRule& rule,
    ObjectId object, const ObjectSimulationRules& rules, uint64_t sessionSeed,
    uint64_t confirmedTick, ObjectBodyDamageState state) {
    runtime.state = state;
    runtime.active = true;
    ++runtime.activationEpoch;
    if (runtime.activationEpoch == 0) runtime.activationEpoch = 1;
    const size_t stateIndex = static_cast<size_t>(state);
    if (stateIndex >= rule.entries.size()) {
        runtime.entries.clear();
        return;
    }
    const container::Vector<game::ObjectBoneFxEntry>& entries =
        rule.entries[stateIndex];
    runtime.entries.assign(entries.size(), {});
    for (size_t index = 0; index < entries.size(); ++index) {
        ObjectBoneFxRuntimeEntry& timer = runtime.entries[index];
        timer.sampleOrdinal = 1;
        timer.dueTick = saturatedAdd(
            confirmedTick, sampleDelayFrames(
                rules, sessionSeed, object, rule.authoredOrder, state,
                entries[index], runtime.activationEpoch,
                timer.sampleOrdinal));
    }
}

[[nodiscard]] uint64_t nextComponentDueTick(
    const ObjectBoneFxComponent& component) noexcept {
    if (component.stopped) return kNeverDue;
    uint64_t next = kNeverDue;
    for (const ObjectBoneFxRuntimeRule& rule : component.rules) {
        if (!rule.active) return 0;
        for (const ObjectBoneFxRuntimeEntry& entry : rule.entries) {
            next = std::min(next, entry.dueTick);
        }
    }
    return next;
}

struct BoneFxWakeEntry final {
    uint64_t dueTick = kNeverDue;
    ObjectId object = INVALID_OBJECT_ID;
};

struct BoneFxWakeQueue final {
    container::Vector<BoneFxWakeEntry> entries;
    bool initialized = false;
};

[[nodiscard]] bool precedes(const BoneFxWakeEntry& left,
                            const BoneFxWakeEntry& right) noexcept {
    return left.dueTick != right.dueTick
        ? left.dueTick < right.dueTick
        : left.object < right.object;
}

void scheduleBoneFxWake(ecs::registry& registry, ObjectId object,
                        uint64_t dueTick) {
    BoneFxWakeQueue* queue = registry.ctx().find<BoneFxWakeQueue>();
    if (!queue || !queue->initialized || !object) return;
    const auto existing = std::find_if(
        queue->entries.begin(), queue->entries.end(),
        [object](const BoneFxWakeEntry& entry) {
            return entry.object == object;
        });
    if (existing != queue->entries.end()) queue->entries.erase(existing);
    if (dueTick == kNeverDue) return;
    const BoneFxWakeEntry entry{.dueTick = dueTick, .object = object};
    queue->entries.insert(
        std::lower_bound(queue->entries.begin(), queue->entries.end(),
                         entry, precedes),
        entry);
}

[[nodiscard]] LogicFixedVec3 pristineBoneLocalPosition(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content, container::StringView boneName) {
    if (!content || boneName.empty()) return {};
    const game::W3dPristineBoneCatalog* catalog =
        content->pristineBoneCatalog();
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!catalog || !source || !source->archetype || !visual) return {};
    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const size_t ruleIndex = game::selectModelConditionVisualRuleIndex(
        templateData, visual->modelConditionFlags);
    if (ruleIndex >= templateData.modelConditionVisuals.size()) return {};
    const std::optional<data::w3d::FixedRigidTransform> transform =
        catalog->find(source->archetype->name, ruleIndex, boneName);
    return transform ? LogicFixedVec3{
        .x = transform->translation.x,
        .y = transform->translation.y,
        .z = transform->translation.z,
    } : LogicFixedVec3{};
}

} // namespace

void ObjectBoneFxSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectBoneFxPlan> plan =
        objectTemplate && objectTemplate->archetype
            ? objectTemplate->archetype->boneFxPlan
            : container::SharedPtr<const game::ObjectBoneFxPlan>{};
    if (!plan || plan->rules.empty() || !plan->hasDamageModule) return;
    ObjectBoneFxComponent component;
    component.plan = plan;
    component.rules.resize(plan->rules.size());
    if (ObjectBoneFxComponent* existing =
            ecs::try_get<ObjectBoneFxComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectBoneFxComponent>(registry, entity,
                                            std::move(component));
    }
    if (const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity)) {
        scheduleBoneFxWake(registry, identity->id, 0);
    }
}

void ObjectBoneFxSystem::onHealthEvent(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event, const ObjectSimulationRules& rules,
    uint64_t sessionSeed, uint64_t& nextEmissionSequence,
    container::Vector<ObjectTransitionDamageFxEvent>& output) const {
    if (event.kind != ObjectHealthEventKind::DamageStateChanged ||
        !event.object) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    ObjectBoneFxComponent* component =
        ecs::try_get<ObjectBoneFxComponent>(registry, *entity);
    if (!component || !component->plan) return;
    // BoneFXDamage::onBodyDamageStateChange calls initTimes() even after a
    // StructureTopple/Collapse stopAllBoneFX(). The stop applies to the
    // current state only; a real later body transition reactivates BoneFX.
    component->stopped = false;
    const ObjectTransitionDamageFxAnchor primary =
        snapshotAnchor(registry, *entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    const size_t count = std::min(component->rules.size(),
                                  component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectBoneFxRule& rule = component->plan->rules[index];
        const bool hasParticles = std::any_of(
            rule.entries.begin(), rule.entries.end(), [](const auto& state) {
                return std::any_of(state.begin(), state.end(),
                    [](const game::ObjectBoneFxEntry& entry) {
                        return entry.kind ==
                            game::ObjectBoneFxPayloadKind::ParticleSystem;
                    });
            });
        if (hasParticles) {
            output.push_back({
                .kind = ObjectTransitionDamageFxEventKind::StopParticleGroup,
                .object = event.object,
                .primary = primary,
                .sourcePathfindLayer = sourcePathfindLayer,
                .attachmentGroup = particleGroup(rule.authoredOrder),
                .authoredOrder = component->plan->damageModuleAuthoredOrder,
                .emissionSequence = nextEmissionSequence++,
                .confirmedTick = event.confirmedTick,
                .oclRequiresDamageSource = false,
            });
        }
        initializeRuleState(
            component->rules[index], rule, event.object, rules, sessionSeed,
            event.confirmedTick, event.currentState);
    }
    component->nextDueTick = nextComponentDueTick(*component);
    scheduleBoneFxWake(registry, event.object, component->nextDueTick);
}

bool ObjectBoneFxSystem::stopAll(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBoneFxStopRequest& request,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectTransitionDamageFxEvent>& output) const {
    if (!request.object) return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(request.object);
    if (!entity) return false;
    ObjectBoneFxComponent* component =
        ecs::try_get<ObjectBoneFxComponent>(registry, *entity);
    if (!component || !component->plan || component->stopped) {
        return false;
    }

    component->stopped = true;
    component->nextDueTick = kNeverDue;
    scheduleBoneFxWake(registry, request.object, kNeverDue);
    const ObjectTransitionDamageFxAnchor primary =
        snapshotAnchor(registry, *entity);
    const size_t count = std::min(component->rules.size(),
                                  component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectBoneFxRuntimeRule& runtime = component->rules[index];
        runtime.active = false;
        runtime.entries.clear();
        const game::ObjectBoneFxRule& rule = component->plan->rules[index];
        const bool hasParticles = std::any_of(
            rule.entries.begin(), rule.entries.end(), [](const auto& state) {
                return std::any_of(state.begin(), state.end(),
                    [](const game::ObjectBoneFxEntry& entry) {
                        return entry.kind ==
                            game::ObjectBoneFxPayloadKind::ParticleSystem;
                    });
            });
        if (!hasParticles) continue;
        output.push_back({
            .kind = ObjectTransitionDamageFxEventKind::StopParticleGroup,
            .object = request.object,
            .primary = primary,
            .attachmentGroup = particleGroup(rule.authoredOrder),
            .authoredOrder = request.callerAuthoredOrder,
            .emissionSequence = nextEmissionSequence++,
            .confirmedTick = request.confirmedTick,
            .oclRequiresDamageSource = false,
        });
    }
    return true;
}

void ObjectBoneFxSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot* content,
    const ObjectSimulationRules& rules, uint64_t sessionSeed,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectTransitionDamageFxEvent>& output) const {
    BoneFxWakeQueue* wakeQueue = registry.ctx().find<BoneFxWakeQueue>();
    if (!wakeQueue) {
        wakeQueue = &registry.ctx().emplace<BoneFxWakeQueue>();
    }
    if (!wakeQueue->initialized) {
        wakeQueue->initialized = true;
        wakeQueue->entries.clear();
        const auto bootstrap = ecs::view<
            ObjectBoneFxComponent, const ObjectIdentityComponent>(registry);
        wakeQueue->entries.reserve(bootstrap.size_hint());
        for (const ecs::entity entity : bootstrap) {
            const ObjectIdentityComponent& identity = bootstrap
                .template get<const ObjectIdentityComponent>(entity);
            const ObjectBoneFxComponent& component = bootstrap
                .template get<ObjectBoneFxComponent>(entity);
            if (identity.id && !component.stopped &&
                component.nextDueTick != kNeverDue) {
                wakeQueue->entries.push_back({
                    .dueTick = component.nextDueTick,
                    .object = identity.id,
                });
            }
        }
        std::sort(wakeQueue->entries.begin(), wakeQueue->entries.end(),
                  precedes);
    }

    container::Vector<ObjectId> objects;
    const BoneFxWakeEntry upper{
        .dueTick = confirmedTick,
        .object = std::numeric_limits<ObjectId>::max(),
    };
    const auto dueEnd = std::upper_bound(
        wakeQueue->entries.begin(), wakeQueue->entries.end(), upper,
        [](const BoneFxWakeEntry& value, const BoneFxWakeEntry& candidate) {
            return precedes(value, candidate);
        });
    objects.reserve(static_cast<size_t>(
        std::distance(wakeQueue->entries.begin(), dueEnd)));
    for (auto cursor = wakeQueue->entries.begin(); cursor != dueEnd;
         ++cursor) {
        objects.push_back(cursor->object);
    }
    wakeQueue->entries.erase(wakeQueue->entries.begin(), dueEnd);
    std::sort(objects.begin(), objects.end());

    for (const ObjectId object : objects) {
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(object);
        if (!entity) continue;
        ObjectBoneFxComponent* component =
            ecs::try_get<ObjectBoneFxComponent>(registry, *entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        if (!component || !component->plan || component->stopped ||
            !health) continue;
        const RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, *entity);
        const bool effectivelyHidden = visual && visual->hidden;
        const ObjectTransitionDamageFxAnchor primary =
            snapshotAnchor(registry, *entity);
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
        const uint32_t sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer;
        const uint8_t damageIndex =
            static_cast<uint8_t>(health->lastDamageType);
        const uint64_t damageBit = damageIndex < 64
            ? uint64_t{1} << damageIndex : 0;
        const size_t ruleCount = std::min(
            component->rules.size(), component->plan->rules.size());
        for (size_t ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex) {
            ObjectBoneFxRuntimeRule& runtime = component->rules[ruleIndex];
            const game::ObjectBoneFxRule& rule =
                component->plan->rules[ruleIndex];
            if (!runtime.active) {
                initializeRuleState(runtime, rule, object, rules, sessionSeed,
                    confirmedTick,
                    objectBodyDamagePresentationState(
                        registry, *entity, health->damageState));
            }
            const size_t stateIndex = static_cast<size_t>(runtime.state);
            if (stateIndex >= rule.entries.size()) continue;
            const container::Vector<game::ObjectBoneFxEntry>& entries =
                rule.entries[stateIndex];
            const size_t entryCount = std::min(entries.size(),
                                               runtime.entries.size());
            for (size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
                const game::ObjectBoneFxEntry& entry = entries[entryIndex];
                ObjectBoneFxRuntimeEntry& timer = runtime.entries[entryIndex];
                if (timer.dueTick == kNeverDue ||
                    timer.dueTick > confirmedTick) continue;

                const bool damageTypeAccepted = !health->hasLastDamageInfo ||
                    (maskFor(rule, entry.kind) & damageBit) != 0;
                const bool hiddenParticle = effectivelyHidden &&
                    entry.kind ==
                        game::ObjectBoneFxPayloadKind::ParticleSystem;
                if (!entry.resource.empty() && damageTypeAccepted &&
                    !hiddenParticle) {
                    // BoneFXUpdate resolves and caches all three payload
                    // families from Drawable::getPristineBonePositions().
                    // Keeping FX/particles on the animated renderer pose
                    // makes emitters wander with turrets and can place the
                    // first event at the root before pose-cache warm-up.
                    game::ObjectTransitionDamageFxLocation location{
                        .kind = game::ObjectTransitionDamageFxLocationKind::
                            LocalCoordinate,
                        .localPosition = pristineBoneLocalPosition(
                            registry, *entity, content, entry.boneName),
                    };
                    output.push_back({
                        .kind = eventKindFor(entry.kind),
                        .object = object,
                        .primary = primary,
                        .sourcePathfindLayer = sourcePathfindLayer,
                        .location = std::move(location),
                        .resource = entry.resource,
                        .attachmentGroup = entry.kind ==
                                game::ObjectBoneFxPayloadKind::ParticleSystem
                            ? particleGroup(rule.authoredOrder) : 0,
                        .authoredOrder = rule.authoredOrder,
                        .slot = entry.slot,
                        .emissionSequence = nextEmissionSequence++,
                        .confirmedTick = confirmedTick,
                        .oclRequiresDamageSource = false,
                    });
                }

                if (entry.onlyOnce) {
                    timer.dueTick = kNeverDue;
                } else {
                    ++timer.sampleOrdinal;
                    timer.dueTick = saturatedAdd(
                        confirmedTick, sampleDelayFrames(
                            rules, sessionSeed, object, rule.authoredOrder,
                            runtime.state, entry, runtime.activationEpoch,
                            timer.sampleOrdinal));
                }
            }
        }
        component->nextDueTick = nextComponentDueTick(*component);
        scheduleBoneFxWake(registry, object, component->nextDueTick);
    }
}

} // namespace engine
