#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/debug/td_assert.h"
#include "GameRenderExtraction.h"

#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/HeatVisionVisualSettings.h"
#include "game/render/ClientTerrainObjectStore.h"
#include "game/render/LocalPlacementPreviewPresentation.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/player/FactionTemplate.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/script/runtime/ScriptProgram.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "game/terrain/TerrainLogic.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include "GameRenderExtractionDetail.h"

namespace engine::render_extraction_detail {

void appendVisualAssetDependency(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    container::StringView modelAsset,
    container::StringView animationState = {}) {
    if (modelAsset.empty()) return;
    container::String key;
    key.reserve(modelAsset.size() + animationState.size() + 1u);
    key.append(modelAsset);
    key.push_back('\x1f');
    key.append(animationState);
    if (!dependencyKeys.insert(key).second) return;
    snapshot.visualAssetDependencies.push_back({
        .modelAsset = container::String{modelAsset},
        .animationState = container::String{animationState},
    });
}

template<typename Rule>
void appendVisualRuleDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const Rule& rule) {
    appendVisualAssetDependency(
        snapshot, dependencyKeys, rule.model, rule.animation);
    for (const game::ModelAnimationCandidate& candidate :
         rule.animationCandidates) {
        appendVisualAssetDependency(
            snapshot, dependencyKeys, rule.model, candidate.resource);
    }
}

[[nodiscard]] const game::ModelConditionMask& stableVisualVariantMask() {
    // These conditions select durable art variants.  The remaining flags are
    // short-lived animation phases (doors, firing, packing, and so on) which
    // must all be ready for the currently active durable variant.
    static const game::ModelConditionMask result =
        game::modelConditionMaskOf(
            game::ModelConditionFlag::Damaged,
            game::ModelConditionFlag::ReallyDamaged,
            game::ModelConditionFlag::Rubble,
            game::ModelConditionFlag::SpecialDamaged,
            game::ModelConditionFlag::Night,
            game::ModelConditionFlag::Snow,
            game::ModelConditionFlag::WeaponsetVeteran,
            game::ModelConditionFlag::WeaponsetElite,
            game::ModelConditionFlag::WeaponsetHero,
            game::ModelConditionFlag::WeaponsetCrateUpgradeOne,
            game::ModelConditionFlag::WeaponsetCrateUpgradeTwo,
            game::ModelConditionFlag::WeaponsetPlayerUpgrade,
            game::ModelConditionFlag::AwaitingConstruction,
            game::ModelConditionFlag::PartiallyConstructed,
            game::ModelConditionFlag::ActivelyBeingConstructed,
            game::ModelConditionFlag::ConstructionComplete,
            game::ModelConditionFlag::PowerPlantUpgraded,
            game::ModelConditionFlag::Sold,
            game::ModelConditionFlag::Captured,
            game::ModelConditionFlag::ArmorsetCrateUpgradeOne,
            game::ModelConditionFlag::ArmorsetCrateUpgradeTwo,
            game::ModelConditionFlag::SecondLife,
            game::ModelConditionFlag::User1,
            game::ModelConditionFlag::User2,
            game::ModelConditionFlag::Disguised);
    return result;
}

[[nodiscard]] const game::ModelConditionMask&
constructionLifecycleVisualMask() {
    static const game::ModelConditionMask result =
        game::modelConditionMaskOf(
            game::ModelConditionFlag::AwaitingConstruction,
            game::ModelConditionFlag::PartiallyConstructed,
            game::ModelConditionFlag::ActivelyBeingConstructed,
            game::ModelConditionFlag::ConstructionComplete,
            game::ModelConditionFlag::Sold);
    return result;
}

[[nodiscard]] game::ModelConditionMask stableVisualVariant(
    game::ModelConditionMask conditions) noexcept {
    const game::ModelConditionMask& stable = stableVisualVariantMask();
    conditions.words[0] &= stable.words[0];
    conditions.words[1] &= stable.words[1];
    return conditions;
}

[[nodiscard]] bool visualRuleCanRunInStableVariant(
    const game::ModelConditionVisualRule& rule,
    const game::ModelConditionMask& activeStable) noexcept {
    if (rule.acceptedConditions.empty()) return true;
    const game::ModelConditionMask& construction =
        constructionLifecycleVisualMask();
    const bool constructionLifecycleActive =
        activeStable.intersectionCount(construction) != 0u;
    game::ModelConditionMask activeDurable = activeStable;
    activeDurable.clear(construction);
    return std::any_of(
        rule.acceptedConditions.begin(), rule.acceptedConditions.end(),
        [&activeDurable, &construction, constructionLifecycleActive](
                game::ModelConditionMask required) {
            required = stableVisualVariant(required);
            const bool constructionRule =
                required.intersectionCount(construction) != 0u;
            if (!constructionLifecycleActive && constructionRule) {
                return false;
            }
            // SparseMatchFinder deliberately lets AWAITING_CONSTRUCTION
            // select a rule authored with the complete construction trio.
            // During construction/sale, preload every phase in that family
            // (including the normal DOWN_DEFAULT endpoint), while retaining
            // the active day/snow/damage variant filter. This mirrors the
            // original preloaded W3DModelDraw and prevents runtime resource
            // latency from postponing scaffolds until completion.
            required.clear(construction);
            return (required.words[0] & ~activeDurable.words[0]) == 0u &&
                   (required.words[1] & ~activeDurable.words[1]) == 0u;
        });
}

void appendVisualChannelDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const game::ModelDrawVisualChannel& channel,
    const game::ModelConditionMask& activeStable) {
    appendVisualAssetDependency(
        snapshot, dependencyKeys, channel.defaultModel);

    container::HashSet<container::String> reachableTransitionKeys;
    reachableTransitionKeys.reserve(channel.conditionVisuals.size());
    for (const game::ModelConditionVisualRule& rule :
         channel.conditionVisuals) {
        if (!visualRuleCanRunInStableVariant(rule, activeStable)) continue;
        appendVisualRuleDependencies(snapshot, dependencyKeys, rule);
        if (!rule.transitionKey.empty()) {
            reachableTransitionKeys.insert(rule.transitionKey);
        }
    }
    for (const game::ModelConditionTransitionRule& rule :
         channel.transitions) {
        if ((!rule.sourceKey.empty() &&
             !reachableTransitionKeys.contains(rule.sourceKey)) ||
            (!rule.destinationKey.empty() &&
             !reachableTransitionKeys.contains(rule.destinationKey))) {
            continue;
        }
        appendVisualRuleDependencies(snapshot, dependencyKeys, rule);
    }
}

void appendThingVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const game::ThingTemplate& templateData,
    game::ModelConditionMask currentConditions) {
    const game::ModelConditionMask activeStable =
        stableVisualVariant(currentConditions);
    appendVisualAssetDependency(
        snapshot, dependencyKeys, templateData.defaultW3dModel);
    if (!templateData.drawVisualChannels.empty()) {
        for (const game::ModelDrawVisualChannel& channel :
             templateData.drawVisualChannels) {
            appendVisualChannelDependencies(
                snapshot, dependencyKeys, channel, activeStable);
        }
        return;
    }

    container::HashSet<container::String> reachableTransitionKeys;
    reachableTransitionKeys.reserve(
        templateData.modelConditionVisuals.size());
    for (const game::ModelConditionVisualRule& rule :
         templateData.modelConditionVisuals) {
        if (!visualRuleCanRunInStableVariant(rule, activeStable)) continue;
        appendVisualRuleDependencies(snapshot, dependencyKeys, rule);
        if (!rule.transitionKey.empty()) {
            reachableTransitionKeys.insert(rule.transitionKey);
        }
    }
    for (const game::ModelConditionTransitionRule& rule :
         templateData.modelConditionTransitions) {
        if ((!rule.sourceKey.empty() &&
             !reachableTransitionKeys.contains(rule.sourceKey)) ||
            (!rule.destinationKey.empty() &&
             !reachableTransitionKeys.contains(rule.destinationKey))) {
            continue;
        }
        appendVisualRuleDependencies(snapshot, dependencyKeys, rule);
    }
}

void appendWeaponProjectileVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    const ObjectWeaponComponent* weapons,
    game::ModelConditionMask sourceConditions) {
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return;
    }
    const ObjectWeaponSetRuntime& activeSet =
        weapons->sets[*weapons->activeWeaponSetIndex];
    for (const ObjectWeaponSlotRuntime& slot : activeSet.slots) {
        const game::WeaponTemplate* weapon =
            content.findWeapon(slot.content);
        if (!weapon || weapon->projectileObject.empty()) continue;
        const container::SharedPtr<const game::ObjectArchetype> projectile =
            content.findObjectArchetype(weapon->projectileObject);
        if (!projectile) continue;
        appendThingVisualAssetDependencies(
            snapshot, dependencyKeys, projectile->templateData,
            sourceConditions);
    }
}

void appendNamedArchetypeVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    container::StringView archetypeName,
    game::ModelConditionMask sourceConditions) {
    if (archetypeName.empty()) return;
    const container::SharedPtr<const game::ObjectArchetype> archetype =
        content.findObjectArchetype(archetypeName);
    if (!archetype) return;
    appendThingVisualAssetDependencies(
        snapshot, dependencyKeys, archetype->templateData,
        sourceConditions);
}

void appendWeaponTemplateProjectileVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    container::StringView weaponName,
    game::ModelConditionMask sourceConditions) {
    const game::WeaponTemplate* weapon =
        content.findWeapon(weaponName);
    if (!weapon) return;
    appendNamedArchetypeVisualAssetDependencies(
        snapshot, dependencyKeys, content, weapon->projectileObject,
        sourceConditions);
}

void appendObjectCreationListVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    container::StringView creationListName,
    game::ModelConditionMask sourceConditions) {
    const game::ObjectCreationListContentId id =
        content.findObjectCreationListId(creationListName);
    const game::ObjectCreationListDefinition* definition =
        content.findObjectCreationList(id);
    if (!definition) return;

    const auto appendGeneric = [&](
            const game::ObjectCreationGenericFields& fields) {
        for (const container::String& name : fields.names) {
            appendNamedArchetypeVisualAssetDependencies(
                snapshot, dependencyKeys, content, name,
                sourceConditions);
        }
    };
    for (const game::ObjectCreationNugget& nugget : definition->nuggets) {
        if (const auto* create =
                std::get_if<game::ObjectCreationCreateObjectNugget>(&nugget)) {
            appendGeneric(create->common);
        } else if (const auto* debris =
                       std::get_if<game::ObjectCreationCreateDebrisNugget>(
                           &nugget)) {
            appendGeneric(debris->common);
        } else if (const auto* delivery =
                       std::get_if<game::ObjectCreationDeliverPayloadNugget>(
                           &nugget)) {
            appendNamedArchetypeVisualAssetDependencies(
                snapshot, dependencyKeys, content, delivery->transport,
                sourceConditions);
            for (const game::ObjectCreationPayloadEntry& payload :
                 delivery->payload) {
                appendNamedArchetypeVisualAssetDependencies(
                    snapshot, dependencyKeys, content, payload.object,
                    sourceConditions);
            }
            appendNamedArchetypeVisualAssetDependencies(
                snapshot, dependencyKeys, content,
                delivery->visiblePayloadTemplateName, sourceConditions);
            appendWeaponTemplateProjectileVisualAssetDependencies(
                snapshot, dependencyKeys, content,
                delivery->visiblePayloadWeaponTemplate, sourceConditions);
        } else if (const auto* fire =
                       std::get_if<game::ObjectCreationFireWeaponNugget>(
                           &nugget)) {
            appendWeaponTemplateProjectileVisualAssetDependencies(
                snapshot, dependencyKeys, content, fire->weapon,
                sourceConditions);
        }
    }
}

void appendSpecialPowerVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    const game::ObjectArchetype& archetype,
    game::ModelConditionMask sourceConditions) {
    if (!archetype.specialPowerPlan) return;
    for (const game::ObjectSpecialPowerRule& rule :
         archetype.specialPowerPlan->rules) {
        if (rule.kind == game::ObjectSpecialPowerKind::ObjectCreationList) {
            appendObjectCreationListVisualAssetDependencies(
                snapshot, dependencyKeys, content,
                rule.objectCreationList, sourceConditions);
            for (const game::ObjectSpecialPowerUpgradeOcl& upgrade :
                 rule.upgradeObjectCreationLists) {
                appendObjectCreationListVisualAssetDependencies(
                    snapshot, dependencyKeys, content,
                    upgrade.objectCreationList, sourceConditions);
            }
        }
        appendNamedArchetypeVisualAssetDependencies(
            snapshot, dependencyKeys, content, rule.referenceObject,
            sourceConditions);
        appendNamedArchetypeVisualAssetDependencies(
            snapshot, dependencyKeys, content, rule.detonationObject,
            sourceConditions);
    }
}


[[nodiscard]] render::ProjectileTrailBlendMode projectileTrailBlend(
    game::ProjectileStreamBlendMode source) noexcept {
    switch (source) {
    case game::ProjectileStreamBlendMode::Alpha:
        return render::ProjectileTrailBlendMode::Alpha;
    case game::ProjectileStreamBlendMode::Multiply:
        return render::ProjectileTrailBlendMode::Multiply;
    case game::ProjectileStreamBlendMode::Opaque:
        return render::ProjectileTrailBlendMode::Opaque;
    case game::ProjectileStreamBlendMode::Additive:
    default:
        return render::ProjectileTrailBlendMode::Additive;
    }
}

[[nodiscard]] render::ProjectileTrailDepthMode projectileTrailDepth(
    game::ProjectileStreamDepthMode source) noexcept {
    switch (source) {
    case game::ProjectileStreamDepthMode::TestWrite:
        return render::ProjectileTrailDepthMode::TestWrite;
    case game::ProjectileStreamDepthMode::Disabled:
        return render::ProjectileTrailDepthMode::Disabled;
    case game::ProjectileStreamDepthMode::TestNoWrite:
    default:
        return render::ProjectileTrailDepthMode::TestNoWrite;
    }
}

// DumbProjectileBehavior treats local +X as the model's forward axis (the
// existing yaw projection is atan2(Y, X)). Build a stable Z-up basis from the
// authoritative fixed-point tangent so climbing and descending shells no
// longer remain visually flat while gameplay continues to use the exact same
// deterministic path values.
[[nodiscard]] std::optional<math::quat> projectileFlightOrientation(
    const ObjectProjectileComponent& projectile,
    uint64_t simulationFrame,
    bool preserveLaunchOrientation) noexcept {
    if (projectile.hasLaunchOrientation &&
        (preserveLaunchOrientation ||
         simulationFrame <= projectile.spawnedTick)) {
        const LogicFixedQuaternion& launch = projectile.launchOrientation;
        return math::quat{
            launch.x.to_float(), launch.y.to_float(),
            launch.z.to_float(), launch.w.to_float()}.normalized();
    }
    if (!projectile.orientToFlightPath || projectile.tumbleRandomly ||
        !projectile.hasFlightPathForward) {
        return std::nullopt;
    }
    math::vec3 localX{
        projectile.flightPathForward.x.to_float(),
        projectile.flightPathForward.y.to_float(),
        projectile.flightPathForward.z.to_float(),
    };
    const float length = localX.length();
    if (!std::isfinite(length) || length <= math::EPSILON) return std::nullopt;
    localX = localX / length;

    math::vec3 localY = math::vec3{0.0f, 0.0f, 1.0f}.cross(localX);
    if (localY.length_sq() <= math::EPSILON * math::EPSILON) {
        localY = math::vec3{0.0f, 1.0f, 0.0f}.cross(localX);
    }
    const float sideLength = localY.length();
    if (!std::isfinite(sideLength) || sideLength <= math::EPSILON) {
        return std::nullopt;
    }
    localY = localY / sideLength;
    const math::vec3 localZ = localX.cross(localY).normalized();
    return math::quat::from_matrix(
        math::transform::from_axes(localX, localY, localZ, {})).normalized();
}


} // namespace engine::render_extraction_detail
