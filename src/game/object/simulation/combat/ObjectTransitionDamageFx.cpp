#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectTransitionDamageFx.h"
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
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

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

[[nodiscard]] uint64_t particleGroup(uint32_t authoredOrder,
                                     ObjectBodyDamageState state) noexcept {
    return ((static_cast<uint64_t>(authoredOrder) + 1u) << 8u) |
        (static_cast<uint64_t>(state) + 1u);
}

[[nodiscard]] uint64_t maskFor(
    const game::ObjectTransitionDamageFxRule& rule,
    game::ObjectTransitionDamageFxPayloadKind kind) noexcept {
    switch (kind) {
    case game::ObjectTransitionDamageFxPayloadKind::FxList:
        return rule.fxDamageTypes;
    case game::ObjectTransitionDamageFxPayloadKind::ObjectCreationList:
        return rule.oclDamageTypes;
    case game::ObjectTransitionDamageFxPayloadKind::ParticleSystem:
        return rule.particleDamageTypes;
    }
    return 0;
}

[[nodiscard]] ObjectTransitionDamageFxEventKind eventKindFor(
    game::ObjectTransitionDamageFxPayloadKind kind) noexcept {
    switch (kind) {
    case game::ObjectTransitionDamageFxPayloadKind::FxList:
        return ObjectTransitionDamageFxEventKind::FxList;
    case game::ObjectTransitionDamageFxPayloadKind::ObjectCreationList:
        return ObjectTransitionDamageFxEventKind::ObjectCreationList;
    case game::ObjectTransitionDamageFxPayloadKind::ParticleSystem:
        return ObjectTransitionDamageFxEventKind::ParticleSystem;
    }
    return ObjectTransitionDamageFxEventKind::FxList;
}

[[nodiscard]] uint64_t mixTransitionRandom(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t transitionBoneRandomKey(
    uint64_t sessionSeed, const ObjectHealthEvent& event,
    const game::ObjectTransitionDamageFxRule& rule,
    const game::ObjectTransitionDamageFxEntry& entry) noexcept {
    uint64_t value = sessionSeed ^ 0x5452414e53444d47ull; // "TRANSDMG"
    value ^= static_cast<uint64_t>(event.object.value) *
        0x9e3779b97f4a7c15ull;
    value ^= static_cast<uint64_t>(event.source.value) *
        0xbf58476d1ce4e5b9ull;
    value ^= event.confirmedTick * 0x94d049bb133111ebull;
    value ^= (static_cast<uint64_t>(rule.authoredOrder) + 1u) << 24u;
    value ^= (static_cast<uint64_t>(event.previousState) + 1u) << 16u;
    value ^= (static_cast<uint64_t>(event.currentState) + 1u) << 12u;
    value ^= (static_cast<uint64_t>(entry.slot) + 1u) << 4u;
    value ^= static_cast<uint64_t>(entry.kind) + 1u;
    for (const unsigned char character : entry.location.boneName) {
        const uint8_t folded = character >= 'A' && character <= 'Z'
            ? static_cast<uint8_t>(character + ('a' - 'A'))
            : character;
        value ^= folded;
        value *= 1099511628211ull;
    }
    return mixTransitionRandom(value);
}

[[nodiscard]] game::ObjectTransitionDamageFxLocation
resolvePristineLocation(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content, uint64_t sessionSeed,
    const ObjectHealthEvent& event,
    const game::ObjectTransitionDamageFxRule& rule,
    const game::ObjectTransitionDamageFxEntry& entry) {
    if (entry.location.kind !=
        game::ObjectTransitionDamageFxLocationKind::Bone) {
        return entry.location;
    }

    // TransitionDamageFX resolves every authored bone through Drawable's
    // pristine pose before dispatching FXList, OCL or ParticleSystem. It does
    // not sample the currently animated skeleton. Resolve the current visual
    // rule through the sealed fixed-point catalog; a missing model/bone keeps
    // the stock object-root fallback at local (0,0,0).
    game::ObjectTransitionDamageFxLocation result;
    result.kind = game::ObjectTransitionDamageFxLocationKind::LocalCoordinate;
    if (!content || entry.location.boneName.empty()) return result;
    const game::W3dPristineBoneCatalog* catalog =
        content->pristineBoneCatalog();
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!catalog || !source || !source->archetype || !visual) return result;
    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const size_t visualRuleIndex =
        game::selectModelConditionVisualRuleIndex(
            templateData, visual->modelConditionFlags);
    if (visualRuleIndex >= templateData.modelConditionVisuals.size()) {
        return result;
    }

    const auto findPosition = [&](container::StringView boneName)
        -> std::optional<LogicFixedVec3> {
        const std::optional<data::w3d::FixedRigidTransform> transform =
            catalog->find(source->archetype->name, visualRuleIndex, boneName);
        if (!transform) return std::nullopt;
        return LogicFixedVec3{
            .x = transform->translation.x,
            .y = transform->translation.y,
            .z = transform->translation.z,
        };
    };

    if (!entry.location.randomBone) {
        if (const std::optional<LogicFixedVec3> position =
                findPosition(entry.location.boneName)) {
            result.localPosition = *position;
        }
        return result;
    }

    constexpr size_t kMaximumRandomBones = 32;
    container::Array<LogicFixedVec3, kMaximumRandomBones> positions{};
    size_t boneCount = 0;
    for (size_t index = 1; index <= kMaximumRandomBones; ++index) {
        container::String indexedName = entry.location.boneName;
        if (index < 10) indexedName.push_back('0');
        indexedName += std::to_string(index);
        const std::optional<LogicFixedVec3> position =
            findPosition(indexedName);
        // RefCode stops the prefix walk at its first missing suffix; it never
        // skips a hole to discover a later bone.
        if (!position) break;
        positions[boneCount++] = *position;
    }
    if (boneCount != 0) {
        const uint64_t sample = transitionBoneRandomKey(
            sessionSeed, event, rule, entry);
        result.localPosition = positions[sample % boneCount];
    }
    return result;
}

} // namespace

void ObjectTransitionDamageFxSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectTransitionDamageFxPlan> plan =
        objectTemplate && objectTemplate->archetype
            ? objectTemplate->archetype->transitionDamageFxPlan
            : container::SharedPtr<const game::ObjectTransitionDamageFxPlan>{};
    if (!plan) return;
    ObjectTransitionDamageFxComponent component{.plan = plan};
    if (ObjectTransitionDamageFxComponent* existing =
            ecs::try_get<ObjectTransitionDamageFxComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectTransitionDamageFxComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectTransitionDamageFxSystem::onHealthEvent(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event, const GameContentSnapshot* content,
    uint64_t sessionSeed, uint64_t& nextEmissionSequence,
    container::Vector<ObjectTransitionDamageFxEvent>& output) const {
    if (event.kind != ObjectHealthEventKind::DamageStateChanged ||
        !event.object) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    const ObjectTransitionDamageFxComponent* component =
        ecs::try_get<ObjectTransitionDamageFxComponent>(registry, *entity);
    if (!component || !component->plan) return;

    const ObjectTransitionDamageFxAnchor primary =
        snapshotAnchor(registry, *entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    ObjectTransitionDamageFxAnchor secondary;
    bool hasSecondary = false;
    if (event.source) {
        if (const std::optional<ecs::entity> source =
                lifecycle.entityFromIdIncludingPending(event.source)) {
            secondary = snapshotAnchor(registry, *source);
            hasSecondary = true;
        }
    }
    const size_t oldState = static_cast<size_t>(event.previousState);
    const size_t newState = static_cast<size_t>(event.currentState);
    if (oldState >= game::kTransitionDamageStateCount ||
        newState >= game::kTransitionDamageStateCount) return;
    const uint8_t damageIndex =
        static_cast<uint8_t>(event.bodyLastDamageType);
    const uint64_t damageBit = damageIndex < 64
        ? uint64_t{1} << damageIndex : 0;
    const bool becameWorse = newState > oldState;

    for (const game::ObjectTransitionDamageFxRule& rule :
         component->plan->rules) {
        const bool oldStateHasParticles = std::any_of(
            rule.entries[oldState].begin(), rule.entries[oldState].end(),
            [](const game::ObjectTransitionDamageFxEntry& entry) {
                return entry.kind ==
                    game::ObjectTransitionDamageFxPayloadKind::ParticleSystem;
            });
        if (oldStateHasParticles) {
            output.push_back({
                .kind = ObjectTransitionDamageFxEventKind::StopParticleGroup,
                .object = event.object,
                .damageSource = event.source,
                .primary = primary,
                .secondary = secondary,
                .sourcePathfindLayer = sourcePathfindLayer,
                .attachmentGroup = particleGroup(
                    rule.authoredOrder, event.previousState),
                .authoredOrder = rule.authoredOrder,
                .emissionSequence = nextEmissionSequence++,
                .confirmedTick = event.confirmedTick,
                .hasSecondary = hasSecondary,
            });
        }
        if (!becameWorse) continue;

        for (const game::ObjectTransitionDamageFxEntry& entry :
             rule.entries[newState]) {
            if ((maskFor(rule, entry.kind) & damageBit) == 0) continue;
            if (entry.kind ==
                    game::ObjectTransitionDamageFxPayloadKind::ObjectCreationList &&
                !hasSecondary) {
                // RefCode only executes TransitionDamageFX OCLs when the
                // DamageInfo source resolves to a live Object.
                continue;
            }
            game::ObjectTransitionDamageFxLocation location =
                resolvePristineLocation(
                    registry, *entity, content, sessionSeed, event, rule,
                    entry);
            output.push_back({
                .kind = eventKindFor(entry.kind),
                .object = event.object,
                .damageSource = event.source,
                .primary = primary,
                .secondary = secondary,
                .sourcePathfindLayer = sourcePathfindLayer,
                .location = std::move(location),
                .resource = entry.resource,
                .attachmentGroup = entry.kind ==
                        game::ObjectTransitionDamageFxPayloadKind::ParticleSystem
                    ? particleGroup(rule.authoredOrder, event.currentState) : 0,
                .authoredOrder = rule.authoredOrder,
                .slot = entry.slot,
                .emissionSequence = nextEmissionSequence++,
                .confirmedTick = event.confirmedTick,
                .hasSecondary = hasSecondary,
            });
        }
    }
}

} // namespace engine
