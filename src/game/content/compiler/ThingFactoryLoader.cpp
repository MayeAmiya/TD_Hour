#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentDiagnostics.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectModuleCatalog.h"
#include "game/object/plan/economy/ObjectAutoDepositPlanTypes.h"
#include "game/object/plan/status/ObjectAutoHealPlanTypes.h"
#include "game/object/plan/structure/ObjectAirfieldPlanTypes.h"
#include "game/object/plan/status/ObjectBaseRegeneratePlanTypes.h"
#include "game/object/plan/lifecycle/ObjectCreatePlanTypes.h"
#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"
#include "game/object/plan/status/ObjectCrateCollidePlanTypes.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectOnDeletePlan.h"
#include "game/object/plan/movement/ObjectDynamicGeometryPlanTypes.h"
#include "game/object/plan/world/ObjectDynamicShroudPlanTypes.h"
#include "game/object/plan/status/ObjectEnemyNearPlanTypes.h"
#include "game/object/plan/movement/ObjectAnimationSteeringPlanTypes.h"
#include "game/object/plan/combat/ObjectTacticalPlanTypes.h"
#include "game/object/plan/economy/ObjectEconomyPlanTypes.h"
#include "game/object/plan/economy/ObjectBuilderPlanTypes.h"
#include "game/object/plan/lifecycle/ObjectRebuildHolePlanTypes.h"
#include "game/object/plan/status/ObjectCheckpointPlanTypes.h"
#include "game/object/plan/lifecycle/ObjectCleanupHazardPlanTypes.h"
#include "game/object/plan/combat/ObjectCombatInitializationPlanTypes.h"
#include "game/object/plan/structure/ObjectMinefieldPlanTypes.h"
#include "game/object/plan/combat/ObjectNeutronMissileSlowDeathPlanTypes.h"
#include "game/object/plan/combat/ObjectCountermeasuresPlanTypes.h"
#include "game/object/plan/combat/ObjectSmartBombPlanTypes.h"
#include "game/object/plan/combat/ObjectStickyBombPlanTypes.h"
#include "game/object/plan/movement/ObjectWaveGuidePlanTypes.h"
#include "game/object/plan/world/ObjectSpyVisionPlanTypes.h"
#include "game/object/plan/special/ObjectSpecialPowerPlanTypes.h"
#include "game/object/plan/structure/ObjectMissileLauncherBuildingPlanTypes.h"
#include "game/object/plan/structure/ObjectParticleUplinkCannonPlanTypes.h"
#include "game/object/plan/combat/ObjectTransitionDamageFxPlanTypes.h"
#include "game/object/plan/combat/ObjectBoneFxPlanTypes.h"
#include "game/object/plan/structure/ObjectBridgePlanTypes.h"
#include "game/object/plan/containment/ObjectSpawnSlavePlanTypes.h"
#include "game/object/plan/status/ObjectEmpUpdatePlanTypes.h"
#include "game/object/plan/combat/ObjectLeafletDropPlanTypes.h"
#include "game/object/plan/movement/ObjectFloatPlanTypes.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"
#include "game/object/plan/combat/ObjectFireWeaponCollidePlanTypes.h"
#include "game/object/plan/combat/ObjectFireWeaponUpdatePlanTypes.h"
#include "game/object/plan/combat/ObjectFireUpdatesPlanTypes.h"
#include "game/object/plan/lifecycle/ObjectHeightDiePlanTypes.h"
#include "game/object/plan/lifecycle/ObjectLifetimePlanTypes.h"
#include "game/object/plan/special/ObjectOclUpdatePlanTypes.h"
#include "game/object/plan/structure/ObjectOverchargePlanTypes.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "game/object/ai/runtime/AIRecipeOwnerRoute.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/plan/combat/ObjectProjectilePlanTypes.h"
#include "game/object/plan/status/ObjectPoisonedPlanTypes.h"
#include "game/object/plan/status/ObjectStealthPlanTypes.h"
#include "game/object/plan/structure/ObjectSupplyWarehouseCripplingPlanTypes.h"
#include "game/object/plan/structure/ObjectTechBuildingPlanTypes.h"
#include "game/object/plan/economy/ObjectProductionPlanTypes.h"
#include "game/object/plan/world/ObjectRadiusDecalPlanTypes.h"
#include "game/object/plan/movement/ObjectSquishCollidePlanTypes.h"
#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"
#include "game/object/plan/combat/ObjectWeaponBonusUpdatePlanTypes.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "core/platform/runtime_threads.h"
#include "core/io/VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <taskflow/algorithm/for_each.hpp>
#include "game/object/definition/ThingRecipeDetail.h"

namespace game {
using namespace detail;

bool ThingFactory::loadFromIniSources(
    container::Span<const ThingIniSource> sources,
    ini::LegacyIniLoadType loadType) {
    if (sources.empty()) return true;

    struct ParseJob final {
        const container::String* content = nullptr;
        size_t sourceIndex = 0;
        size_t layerIndex = 0;
    };
    struct ParsedLayer final {
        container::Vector<IniBlock> blocks;
        bool valid = false;
    };

    container::Vector<ParseJob> parseJobs;
    size_t layerCount = 0;
    for (const ThingIniSource& source : sources) layerCount += source.layers.size();
    parseJobs.reserve(layerCount);
    for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const ThingIniSource& source = sources[sourceIndex];
        if (source.layers.empty()) return false;
        for (size_t layerIndex = 0; layerIndex < source.layers.size(); ++layerIndex) {
            parseJobs.push_back({
                .content = &source.layers[layerIndex],
                .sourceIndex = sourceIndex,
                .layerIndex = layerIndex,
            });
        }
    }

    const auto loadBegin = std::chrono::steady_clock::now();
    container::Vector<ParsedLayer> parsedLayers(parseJobs.size());
    const auto parseOne = [&](size_t index) {
        GeneralsIniParser parser;
        ParsedLayer& output = parsedLayers[index];
        const ParseJob& job = parseJobs[index];
        output.valid = parser.parse(
            *job.content, sources[job.sourceIndex].path);
        if (output.valid) output.blocks = parser.takeBlocks();
    };
    if (parseJobs.size() >= 4u &&
        platform::runtime::sceneResourceWorkerCount() > 1u) {
        tf::Taskflow parseFlow;
        parseFlow.for_each_index(
            size_t{0}, parseJobs.size(), size_t{1}, parseOne);
        try {
            platform::runtime::sceneResourceExecutor().run(parseFlow).get();
        } catch (...) {
            return false;
        }
    } else {
        for (size_t index = 0; index < parseJobs.size(); ++index) parseOne(index);
    }
    for (const ParsedLayer& parsed : parsedLayers) {
        if (!parsed.valid) return false;
    }
    const auto parseEnd = std::chrono::steady_clock::now();

    // Any new layer (including a future Map.ini Object override) invalidates
    // the reverse prerequisite classification.  The content-freeze boundary
    // finalizes it again after the complete effective universe is known.
    m_derivedMetadataFinalized = false;
    bool valid = true;
    struct CompiledRecipeResult final {
        container::String name;
        container::SharedPtr<const ObjectArchetype> archetype;
        bool valid = true;
    };
    container::Vector<CompiledRecipeResult> compiledRecipes;
    container::HashMap<container::String,
                       container::SharedPtr<const CombatProfile>>
        stagedCombatProfiles;
    container::Vector<std::function<void()>> recipeCompileJobs;
    size_t projectedRecipeCount = 0;
    for (const ParsedLayer& parsed : parsedLayers) {
        projectedRecipeCount += static_cast<size_t>(std::count_if(
            parsed.blocks.begin(), parsed.blocks.end(), [](const IniBlock& block) {
                return block.type == "Object" || block.type == "ObjectReskin";
            }));
    }
    compiledRecipes.reserve(projectedRecipeCount);
    recipeCompileJobs.reserve(projectedRecipeCount);
    stagedCombatProfiles.reserve(projectedRecipeCount);
    m_things.reserve(m_things.size() + projectedRecipeCount);
    m_archetypes.reserve(m_archetypes.size() + projectedRecipeCount);
    int64_t inheritanceMicros = 0;
    int64_t entryApplyMicros = 0;
    int64_t combatMicros = 0;
    int64_t projectionFinalizeMicros = 0;
    int64_t recipeStageMicros = 0;

    const auto findCombatProfile = [&](const container::String& name) {
        if (const auto staged = stagedCombatProfiles.find(name);
            staged != stagedCombatProfiles.end()) {
            return staged->second;
        }
        if (const auto compiled = m_archetypes.find(name);
            compiled != m_archetypes.end() && compiled->second) {
            return compiled->second->combatProfile;
        }
        return container::SharedPtr<const CombatProfile>{};
    };
    const auto compileBlocks = [&](const container::Vector<IniBlock>& blocks,
                                   RecipeLoadMode loadMode,
                                   container::StringView filePath) {
        for (const IniBlock& block : blocks) {
            const auto inheritanceBegin = std::chrono::steady_clock::now();
            ThingAuthoringTemplate templateData;
            container::String targetName;
            container::SharedPtr<const CombatProfile> parentCombatProfile;
            bool copiedParentRecipe = false;
            bool reskin = false;
            if (block.type == "Object") {
                targetName = block.name;
                if (targetName.empty()) {
                    TD_LOG_WARN("[ThingFactory] Ignored unnamed Object in {}", filePath);
                    valid = false;
                    continue;
                }
                if (loadMode != RecipeLoadMode::Base) {
                    // A higher-priority VFS layer is a modern immutable
                    // equivalent of RefCode's final-override copy. It does
                    // not need an old linked override chain to preserve the
                    // effective parent recipe.
                    if (const auto parent = m_things.find(targetName); parent != m_things.end()) {
                        templateData = parent->second;
                        copiedParentRecipe = true;
                        parentCombatProfile = findCombatProfile(targetName);
                    }
                }
                if (!copiedParentRecipe && targetName != "DefaultThingTemplate") {
                    if (const auto base = m_things.find("DefaultThingTemplate"); base != m_things.end()) {
                        templateData = base->second;
                        copiedParentRecipe = true;
                        parentCombatProfile =
                            findCombatProfile("DefaultThingTemplate");
                    }
                }
            } else if (block.type == "ObjectReskin") {
                const auto header = parseObjectReskinHeader(block.name);
                if (!header) {
                    TD_LOG_WARN("[ThingFactory] Ignored malformed ObjectReskin header '{}' in {}", block.name, filePath);
                    valid = false;
                    continue;
                }
                targetName = header->first;
                const auto source = m_things.find(header->second);
                if (source == m_things.end()) {
                    TD_LOG_WARN("[ThingFactory] ObjectReskin '{}' references unavailable source '{}' in {}",
                                targetName, header->second, filePath);
                    valid = false;
                    continue;
                }
                templateData = source->second;
                templateData.legacyReskinRootName =
                    source->second.legacyReskinRootName.empty()
                        ? header->second
                        : source->second.legacyReskinRootName;
                copiedParentRecipe = true;
                reskin = true;
                parentCombatProfile = findCombatProfile(header->second);
            } else {
                continue;
            }

            templateData.name = targetName;
            templateData.loaded = false;
            TemplateRecipeParseState state{
                .copiedParentRecipe = copiedParentRecipe,
                .overlayLoad = loadMode != RecipeLoadMode::Base && !reskin,
                .strictCreateOverrides =
                    loadMode == RecipeLoadMode::StrictCreateOverrides &&
                    !reskin,
            };
            if (copiedParentRecipe) markModulesCopiedFromParent(templateData);
            for (const ModuleData& module : templateData.modules) {
                state.nextAuthoredOrder = std::max(state.nextAuthoredOrder, module.authoredOrder + 1u);
            }

            const auto entryApplyBegin = std::chrono::steady_clock::now();
            inheritanceMicros +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    entryApplyBegin - inheritanceBegin).count();
            applyObjectRecipeEntries(templateData, block, state, reskin);

            const auto combatBegin = std::chrono::steady_clock::now();
            entryApplyMicros +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    combatBegin - entryApplyBegin).count();
            container::SharedPtr<const CombatProfile> combatProfile;
            const bool declaresCombatSet = std::any_of(block.children.begin(), block.children.end(),
                [](const IniBlock& child) { return isCombatSetBlock(child.type); });
            if (reskin && parentCombatProfile) {
                // ObjectReskin is deliberately visual-only. Share the source
                // profile rather than reparsing or cloning its gameplay data.
                combatProfile = parentCombatProfile;
            } else if (!declaresCombatSet && parentCombatProfile) {
                // A derived object without a local Set preserves the exact
                // immutable profile handle from its copied parent.
                combatProfile = parentCombatProfile;
            } else {
                CombatProfileCompileOptions combatOptions;
                combatOptions.inherited = parentCombatProfile.get();
                CombatProfileCompileResult combat = compileCombatProfile(
                    container::Span<const IniBlock>{block.children}, combatOptions);
                combatProfile = std::move(combat.profile);
                for (const CombatProfileDiagnostic& diagnostic : combat.diagnostics) {
                    state.diagnostics.push_back({
                        .severity = diagnostic.severity == CombatProfileDiagnosticSeverity::Error
                            ? ObjectRecipeDiagnosticSeverity::Error
                            : ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "combat profile: " + diagnostic.message,
                    });
                }
            }
            stagedCombatProfiles.insert_or_assign(targetName, combatProfile);

            const auto projectionFinalizeBegin = std::chrono::steady_clock::now();
            combatMicros +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    projectionFinalizeBegin - combatBegin).count();
            // Preserve the early loader's compatibility projection only when
            // no Body module exists. A copied default InactiveBody stays
            // inactive until a real Body recipe replaces it.
            if (!templateData.body.fromBodyModule) {
                templateData.body.maximumHealth = templateData.maxHealth;
                templateData.body.initialHealth = templateData.startingHealth;
                templateData.body.normalize();
            }
            rebuildBodyAndDrawProjection(templateData, state);

            if (templateData.body.fromBodyModule) {
                templateData.maxHealth = templateData.body.maximumHealth;
                templateData.startingHealth = templateData.body.initialHealth;
            }
            templateData.geometry.normalize();
            templateData.structureRubbleHeightFixed =
                math::q32_32{templateData.structureRubbleHeight};
            templateData.placementViewAngleRadiansFixed =
                math::q32_32{templateData.placementViewAngleRadians};
            templateData.factoryExitWidthFixed =
                math::q32_32{templateData.factoryExitWidth};
            templateData.factoryExtraBibWidthFixed =
                math::q32_32{templateData.factoryExtraBibWidth};
            templateData.fenceWidthFixed =
                math::q32_32{templateData.fenceWidth};
            templateData.fenceXOffsetFixed =
                math::q32_32{templateData.fenceXOffset};
            templateData.sightRangeFixed =
                std::isfinite(templateData.sight) && templateData.sight > 0.0f
                ? math::q32_32{templateData.sight}
                : math::q32_32{};
            const float authoredShroudRange =
                templateData.shroudClearingRange >= 0.0f
                ? templateData.shroudClearingRange
                : templateData.sight;
            templateData.shroudClearingRangeFixed =
                std::isfinite(authoredShroudRange) && authoredShroudRange > 0.0f
                ? math::q32_32{authoredShroudRange}
                : math::q32_32{};
            templateData.shroudRevealToAllRangeFixed =
                std::isfinite(templateData.shroudRevealToAllRange)
                ? math::q32_32{templateData.shroudRevealToAllRange}
                : math::q32_32{};
            templateData.buildCostFixed =
                std::isfinite(templateData.buildCost)
                ? math::q32_32{templateData.buildCost}
                : math::q32_32{};
            ThingTemplate& runtimeTemplate = templateData;
            runtimeTemplate.body =
                static_cast<const ObjectBodyTemplate&>(templateData.body);
            runtimeTemplate.geometry =
                static_cast<const ObjectGeometryTemplate&>(
                    templateData.geometry);
            templateData.loaded = true;
            const auto recipeStageBegin = std::chrono::steady_clock::now();
            projectionFinalizeMicros +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    recipeStageBegin - projectionFinalizeBegin).count();
            auto [stored, inserted] = m_things.insert_or_assign(templateData.name, std::move(templateData));
            static_cast<void>(inserted);
            const size_t resultIndex = compiledRecipes.size();
            compiledRecipes.emplace_back();
            const container::String recipeSourcePath{
                block.source.pathView().empty()
                    ? filePath : block.source.pathView()};
            const uint32_t recipeSourceLine = block.source.line;
            recipeCompileJobs.emplace_back(
                [this, resultIndex, stagedName = stored->first,
                 combatProfile = std::move(combatProfile),
                 state = std::move(state),
                 recipeSourcePath, recipeSourceLine,
                 &compiledRecipes]() mutable {
            ContentDiagnosticProvenanceScope diagnosticScope{
                recipeSourcePath, recipeSourceLine};
            bool recipeValid = true;
            const auto stagedTemplate = m_things.find(stagedName);
            if (stagedTemplate == m_things.end()) {
                compiledRecipes[resultIndex].valid = false;
                return;
            }
            const ThingTemplate& finalTemplate = stagedTemplate->second;
            const container::SharedPtr<const ObjectPhysicsPlan> physicsPlan =
                compileObjectPhysicsPlan(finalTemplate);
            const container::SharedPtr<const ObjectAIBehaviorPlan>
                aiBehaviorPlan = compileObjectAIBehaviorPlan(finalTemplate);
            const container::SharedPtr<const engine::ObjectCombatInitializationPlan>
                combatInitializationPlan =
                    engine::compileObjectCombatInitializationPlan(
                        finalTemplate);
            const container::SharedPtr<const engine::ObjectProjectilePlan>
                projectilePlan =
                    engine::compileObjectProjectilePlan(finalTemplate);
            const container::SharedPtr<const ObjectDeathReactionPlan> deathReactionPlan =
                compileObjectDeathReactionPlan(finalTemplate);
            const container::SharedPtr<const ObjectTransitionDamageFxPlan>
                transitionDamageFxPlan =
                    compileObjectTransitionDamageFxPlan(finalTemplate);
            const container::SharedPtr<const ObjectBoneFxPlan> boneFxPlan =
                compileObjectBoneFxPlan(finalTemplate);
            const container::SharedPtr<const ObjectBridgeRailPlan>
                bridgeRailPlan = compileObjectBridgeRailPlan(finalTemplate);
            const container::SharedPtr<const ObjectSpawnSlavePlan>
                spawnSlavePlan = compileObjectSpawnSlavePlan(finalTemplate);
            const container::SharedPtr<const ObjectAutoDepositPlan> autoDepositPlan =
                compileObjectAutoDepositPlan(finalTemplate);
            const container::SharedPtr<const ObjectAutoHealPlan> autoHealPlan =
                compileObjectAutoHealPlan(finalTemplate);
            const container::SharedPtr<const ObjectAirfieldPlan> airfieldPlan =
                compileObjectAirfieldPlan(finalTemplate);
            const container::SharedPtr<const ObjectBaseRegeneratePlan> baseRegeneratePlan =
                compileObjectBaseRegeneratePlan(finalTemplate);
            const container::SharedPtr<const ObjectFloatPlan> floatPlan =
                compileObjectFloatPlan(finalTemplate);
            const container::SharedPtr<const ObjectHeightDiePlan> heightDiePlan =
                compileObjectHeightDiePlan(finalTemplate);
            const container::SharedPtr<const ObjectLifetimePlan> lifetimePlan =
                compileObjectLifetimePlan(finalTemplate);
            const container::SharedPtr<const ObjectOclUpdatePlan> oclUpdatePlan =
                compileObjectOclUpdatePlan(finalTemplate);
            const container::SharedPtr<const ObjectProductionPlan> productionPlan =
                compileObjectProductionPlan(finalTemplate);
            const container::SharedPtr<const ObjectProductionExitPlan> productionExitPlan =
                compileObjectProductionExitPlan(finalTemplate);
            const container::SharedPtr<const ObjectRadiusDecalPlan> radiusDecalPlan =
                compileObjectRadiusDecalPlan(finalTemplate);
            const container::SharedPtr<const ObjectTechBuildingPlan>
                techBuildingPlan = compileObjectTechBuildingPlan(finalTemplate);
            const container::SharedPtr<const ObjectUpgradePlan> objectUpgradePlan =
                compileObjectUpgradePlan(finalTemplate);
            const container::SharedPtr<const ObjectCreatePlan> createPlan =
                compileObjectCreatePlan(finalTemplate);
            const container::SharedPtr<const ObjectCrateCollidePlan> crateCollidePlan =
                compileObjectCrateCollidePlan(finalTemplate);
            const container::SharedPtr<const ObjectOnDeletePlan> onDeletePlan =
                compileObjectOnDeletePlan(finalTemplate);
            const container::SharedPtr<const engine::ObjectContainmentPlan>
                containmentPlan = compileObjectContainmentPlan(finalTemplate);
            const container::SharedPtr<const ObjectPoisonedPlan> poisonedPlan =
                compileObjectPoisonedPlan(finalTemplate);
            const container::SharedPtr<const ObjectOverchargePlan> overchargePlan =
                compileObjectOverchargePlan(finalTemplate);
            const container::SharedPtr<const ObjectWeaponBonusUpdatePlan>
                weaponBonusUpdatePlan =
                    compileObjectWeaponBonusUpdatePlan(finalTemplate);
            const container::SharedPtr<const ObjectFireWeaponWhenDamagedPlan>
                fireWeaponWhenDamagedPlan =
                    compileObjectFireWeaponWhenDamagedPlan(finalTemplate);
            const container::SharedPtr<const ObjectFireWeaponUpdatePlan>
                fireWeaponUpdatePlan =
                    compileObjectFireWeaponUpdatePlan(finalTemplate);
            const container::SharedPtr<const ObjectFireWeaponCollidePlan>
                fireWeaponCollidePlan =
                    compileObjectFireWeaponCollidePlan(finalTemplate);
            const container::SharedPtr<const ObjectSquishCollidePlan>
                squishCollidePlan =
                    compileObjectSquishCollidePlan(finalTemplate);
            const container::SharedPtr<const ObjectFlammablePlan> flammablePlan =
                compileObjectFlammablePlan(finalTemplate);
            const container::SharedPtr<const ObjectFireSpreadPlan> fireSpreadPlan =
                compileObjectFireSpreadPlan(finalTemplate);
            const container::SharedPtr<const ObjectFireOclAfterCooldownPlan>
                fireOclAfterCooldownPlan =
                    compileObjectFireOclAfterCooldownPlan(finalTemplate);
            const container::SharedPtr<const ObjectEmpPlan> empPlan =
                compileObjectEmpPlan(finalTemplate);
            const container::SharedPtr<const ObjectLeafletDropPlan>
                leafletDropPlan = compileObjectLeafletDropPlan(finalTemplate);
            const container::SharedPtr<const ObjectStealthPlan> stealthPlan =
                compileObjectStealthPlan(finalTemplate);
            const container::SharedPtr<const ObjectStealthDetectorPlan>
                stealthDetectorPlan =
                    compileObjectStealthDetectorPlan(finalTemplate);
            const container::SharedPtr<const ObjectGrantStealthPlan>
                grantStealthPlan =
                    compileObjectGrantStealthPlan(finalTemplate);
            const container::SharedPtr<const ObjectDynamicShroudPlan>
                dynamicShroudPlan =
                    compileObjectDynamicShroudPlan(finalTemplate);
            const container::SharedPtr<const ObjectDynamicGeometryPlan>
                dynamicGeometryPlan =
                    compileObjectDynamicGeometryPlan(finalTemplate);
            const container::SharedPtr<const ObjectEnemyNearPlan> enemyNearPlan =
                compileObjectEnemyNearPlan(finalTemplate);
            const container::SharedPtr<const ObjectAnimationSteeringPlan>
                animationSteeringPlan =
                    compileObjectAnimationSteeringPlan(finalTemplate);
            const container::SharedPtr<const ObjectTacticalPlan> tacticalPlan =
                compileObjectTacticalPlan(finalTemplate);
            const container::SharedPtr<const ObjectEconomyPlan> economyPlan =
                compileObjectEconomyPlan(finalTemplate);
            const container::SharedPtr<const ObjectBuilderPlan> builderPlan =
                compileObjectBuilderPlan(finalTemplate);
            const container::SharedPtr<const ObjectRebuildHolePlan>
                rebuildHolePlan = compileObjectRebuildHolePlan(finalTemplate);
            const container::SharedPtr<const ObjectCheckpointPlan>
                checkpointPlan = compileObjectCheckpointPlan(finalTemplate);
            const container::SharedPtr<const ObjectCleanupHazardPlan>
                cleanupHazardPlan =
                    compileObjectCleanupHazardPlan(finalTemplate);
            const container::SharedPtr<const ObjectMinefieldPlan>
                minefieldPlan = compileObjectMinefieldPlan(finalTemplate);
            const container::SharedPtr<const ObjectNeutronMissileSlowDeathPlan>
                neutronMissileSlowDeathPlan =
                    compileObjectNeutronMissileSlowDeathPlan(finalTemplate);
            const container::SharedPtr<const ObjectCountermeasuresPlan>
                countermeasuresPlan =
                    compileObjectCountermeasuresPlan(finalTemplate);
            const container::SharedPtr<const ObjectSmartBombPlan>
                smartBombPlan = compileObjectSmartBombPlan(finalTemplate);
            const container::SharedPtr<const ObjectStickyBombPlan>
                stickyBombPlan = compileObjectStickyBombPlan(finalTemplate);
            const container::SharedPtr<const ObjectWaveGuidePlan>
                waveGuidePlan = compileObjectWaveGuidePlan(finalTemplate);
            const container::SharedPtr<const ObjectSpyVisionPlan>
                spyVisionPlan = compileObjectSpyVisionPlan(finalTemplate);
            const container::SharedPtr<const ObjectSpecialPowerPlan>
                specialPowerPlan =
                    compileObjectSpecialPowerPlan(finalTemplate);
            const container::SharedPtr<
                const ObjectMissileLauncherBuildingPlan>
                missileLauncherBuildingPlan =
                    compileObjectMissileLauncherBuildingPlan(finalTemplate);
            const container::SharedPtr<const ObjectParticleUplinkCannonPlan>
                particleUplinkCannonPlan =
                    compileObjectParticleUplinkCannonPlan(finalTemplate);
            const container::SharedPtr<
                const ObjectSupplyWarehouseCripplingPlan>
                supplyWarehouseCripplingPlan =
                    compileObjectSupplyWarehouseCripplingPlan(finalTemplate);
            if (createPlan) {
                for (const container::String& diagnostic : createPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "create plan: " + diagnostic,
                    });
                }
            }
            if (deathReactionPlan) {
                for (const container::String& diagnostic :
                     deathReactionPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "death walk: " + diagnostic,
                    });
                }
            }
            if (transitionDamageFxPlan) {
                for (const container::String& diagnostic :
                     transitionDamageFxPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "transition-damage FX plan: " + diagnostic,
                    });
                }
            }
            if (stealthPlan) {
                for (const container::String& diagnostic :
                     stealthPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "stealth plan: " + diagnostic,
                    });
                }
            }
            if (cleanupHazardPlan) {
                for (const container::String& diagnostic :
                     cleanupHazardPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "cleanup-hazard plan: " + diagnostic,
                    });
                }
            }
            if (minefieldPlan) {
                for (const container::String& diagnostic :
                     minefieldPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "minefield plan: " + diagnostic,
                    });
                }
            }
            if (neutronMissileSlowDeathPlan) {
                for (const container::String& diagnostic :
                     neutronMissileSlowDeathPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "neutron slow-death plan: " + diagnostic,
                    });
                }
            }
            if (countermeasuresPlan) {
                for (const container::String& diagnostic :
                     countermeasuresPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "countermeasures plan: " + diagnostic,
                    });
                }
            }
            if (smartBombPlan) {
                for (const container::String& diagnostic :
                     smartBombPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "smart-bomb plan: " + diagnostic,
                    });
                }
            }
            if (stickyBombPlan) {
                for (const container::String& diagnostic :
                     stickyBombPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "sticky-bomb plan: " + diagnostic,
                    });
                }
            }
            if (waveGuidePlan) {
                for (const container::String& diagnostic :
                     waveGuidePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "wave-guide plan: " + diagnostic,
                    });
                }
            }
            if (stealthDetectorPlan) {
                for (const container::String& diagnostic :
                     stealthDetectorPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "stealth detector plan: " + diagnostic,
                    });
                }
            }
            if (grantStealthPlan) {
                for (const container::String& diagnostic :
                     grantStealthPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "grant stealth plan: " + diagnostic,
                    });
                }
            }
            if (dynamicShroudPlan) {
                for (const container::String& diagnostic :
                     dynamicShroudPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "dynamic shroud plan: " + diagnostic,
                    });
                }
            }
            if (dynamicGeometryPlan) {
                for (const container::String& diagnostic :
                     dynamicGeometryPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "dynamic geometry plan: " + diagnostic,
                    });
                }
            }
            if (supplyWarehouseCripplingPlan) {
                for (const container::String& diagnostic :
                     supplyWarehouseCripplingPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "supply warehouse crippling plan: " +
                            diagnostic,
                    });
                }
            }
            if (boneFxPlan) {
                for (const container::String& diagnostic :
                     boneFxPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "bone FX plan: " + diagnostic,
                    });
                }
            }
            if (autoDepositPlan) {
                for (const container::String& diagnostic : autoDepositPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "auto-deposit plan: " + diagnostic,
                    });
                }
            }
            if (crateCollidePlan) {
                for (const container::String& diagnostic : crateCollidePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "crate collide plan: " + diagnostic,
                    });
                }
            }
            if (containmentPlan) {
                for (const container::String& diagnostic :
                     containmentPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "containment plan: " + diagnostic,
                    });
                }
            }
            if (spyVisionPlan) {
                for (const container::String& diagnostic :
                     spyVisionPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "spy vision plan: " + diagnostic,
                    });
                }
            }
            if (specialPowerPlan) {
                for (const container::String& diagnostic :
                     specialPowerPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "special power plan: " + diagnostic,
                    });
                }
            }
            if (missileLauncherBuildingPlan) {
                for (const container::String& diagnostic :
                     missileLauncherBuildingPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "missile-launcher-building plan: " +
                            diagnostic,
                    });
                }
            }
            if (particleUplinkCannonPlan) {
                for (const container::String& diagnostic :
                     particleUplinkCannonPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "particle-uplink-cannon plan: " +
                            diagnostic,
                    });
                }
            }
            if (poisonedPlan) {
                for (const container::String& diagnostic : poisonedPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "poisoned plan: " + diagnostic,
                    });
                }
            }
            if (overchargePlan) {
                for (const container::String& diagnostic :
                     overchargePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "overcharge plan: " + diagnostic,
                    });
                }
            }
            if (weaponBonusUpdatePlan) {
                for (const container::String& diagnostic :
                     weaponBonusUpdatePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "weapon bonus update plan: " + diagnostic,
                    });
                }
            }
            if (fireWeaponWhenDamagedPlan) {
                for (const container::String& diagnostic :
                     fireWeaponWhenDamagedPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "fire-weapon-when-damaged plan: " + diagnostic,
                    });
                }
            }
            if (fireWeaponUpdatePlan) {
                for (const container::String& diagnostic :
                     fireWeaponUpdatePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "fire-weapon update plan: " + diagnostic,
                    });
                }
            }
            if (fireWeaponCollidePlan) {
                for (const container::String& diagnostic :
                     fireWeaponCollidePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "fire-weapon collide plan: " + diagnostic,
                    });
                }
            }
            if (flammablePlan) {
                for (const container::String& diagnostic : flammablePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "flammable plan: " + diagnostic,
                    });
                }
            }
            if (fireSpreadPlan) {
                for (const container::String& diagnostic : fireSpreadPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "fire-spread plan: " + diagnostic,
                    });
                }
            }
            if (fireOclAfterCooldownPlan) {
                for (const container::String& diagnostic :
                     fireOclAfterCooldownPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "fire-OCL cooldown plan: " + diagnostic,
                    });
                }
            }
            if (empPlan) {
                for (const container::String& diagnostic : empPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "EMP update plan: " + diagnostic,
                    });
                }
            }
            if (leafletDropPlan) {
                for (const container::String& diagnostic :
                     leafletDropPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "leaflet-drop plan: " + diagnostic,
                    });
                }
            }
            if (heightDiePlan) {
                for (const container::String& diagnostic : heightDiePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "height-die plan: " + diagnostic,
                    });
                }
            }
            if (floatPlan) {
                for (const container::String& diagnostic : floatPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "float plan: " + diagnostic,
                    });
                }
            }
            if (lifetimePlan) {
                for (const container::String& diagnostic : lifetimePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "lifetime plan: " + diagnostic,
                    });
                }
            }
            if (oclUpdatePlan) {
                for (const container::String& diagnostic : oclUpdatePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "OCL update plan: " + diagnostic,
                    });
                }
            }
            if (productionPlan) {
                for (const container::String& diagnostic : productionPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "production plan: " + diagnostic,
                    });
                }
            }
            if (productionExitPlan) {
                for (const container::String& diagnostic : productionExitPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Error,
                        .message = "production exit plan: " + diagnostic,
                    });
                }
            }
            if (objectUpgradePlan) {
                for (const container::String& diagnostic : objectUpgradePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "object upgrade plan: " + diagnostic,
                    });
                }
            }
            if (economyPlan) {
                for (const container::String& diagnostic : economyPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "economy plan: " + diagnostic,
                    });
                }
            }
            if (builderPlan) {
                for (const container::String& diagnostic : builderPlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "builder plan: " + diagnostic,
                    });
                }
            }
            if (rebuildHolePlan) {
                for (const container::String& diagnostic :
                     rebuildHolePlan->diagnostics) {
                    state.diagnostics.push_back({
                        .severity = ObjectRecipeDiagnosticSeverity::Warning,
                        .message = "rebuild-hole plan: " + diagnostic,
                    });
                }
            }
            ObjectKindOfMask kindOfMask;
            container::Vector<container::String> unknownKindOfTokens;
            static_cast<void>(compileObjectKindOfMask(
                finalTemplate.kindOf, kindOfMask, &unknownKindOfTokens));
            for (const container::String& token : unknownKindOfTokens) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Warning,
                    .message = "unknown KindOf token disabled: " + token,
                });
            }
            const uint64_t finalFingerprint = objectRecipeFingerprint(finalTemplate, combatProfile.get());
            for (ObjectRecipeDiagnostic& diagnostic : state.diagnostics) {
                if (diagnostic.sourcePath.empty()) {
                    diagnostic.sourcePath = recipeSourcePath;
                }
                if (diagnostic.sourceLine == 0) {
                    diagnostic.sourceLine = recipeSourceLine;
                }
            }
            for (const ObjectRecipeDiagnostic& diagnostic : state.diagnostics) {
                if (diagnostic.severity == ObjectRecipeDiagnosticSeverity::Error) {
                    recipeValid = false;
                }
            }
            const container::String recipeName = finalTemplate.name;
            const bool hasAiUpdate = std::any_of(
                finalTemplate.modules.begin(), finalTemplate.modules.end(),
                [](const ModuleData& module) { return module.isAiModule; });
            const engine::ai::AIRecipeResolution aiRecipe = hasAiUpdate
                ? engine::ai::resolveAIRecipe(finalTemplate.modules)
                : engine::ai::AIRecipeResolution{};
            engine::ai::ObjectAIOrderCapability initialAiCapabilities =
                engine::ai::ObjectAIOrderCapability::None;
            if (aiRecipe.resolved()) {
                if (engine::ai::aiRecipeUsesGenericMoveStop(aiRecipe.recipe)) {
                    initialAiCapabilities |=
                        engine::ai::ObjectAIOrderCapability::MoveStop;
                }
                if (engine::ai::aiRecipeUsesGenericAttack(aiRecipe.recipe)) {
                    initialAiCapabilities |=
                        engine::ai::ObjectAIOrderCapability::Attack;
                }
            }
            compiledRecipes[resultIndex] = CompiledRecipeResult{
                .name = recipeName,
                .archetype = std::make_shared<const ObjectArchetype>(ObjectArchetype{
                    .name = recipeName,
                    .templateData = finalTemplate,
                    .sightRangeFixed = finalTemplate.sightRangeFixed,
                    .shroudClearingRangeFixed =
                        finalTemplate.shroudClearingRangeFixed,
                    .kindOfMask = kindOfMask,
                    .aiBehaviorPlan = std::move(aiBehaviorPlan),
                    .combatProfile = std::move(combatProfile),
                    .combatInitializationPlan =
                        std::move(combatInitializationPlan),
                    .physicsPlan = std::move(physicsPlan),
                    .projectilePlan = std::move(projectilePlan),
                    .airfieldPlan = std::move(airfieldPlan),
                    .deathReactionPlan = std::move(deathReactionPlan),
                    .onDeletePlan = std::move(onDeletePlan),
                    .transitionDamageFxPlan =
                        std::move(transitionDamageFxPlan),
                    .boneFxPlan = std::move(boneFxPlan),
                    .bridgeRailPlan = std::move(bridgeRailPlan),
                    .spawnSlavePlan = std::move(spawnSlavePlan),
                    .autoDepositPlan = std::move(autoDepositPlan),
                    .autoHealPlan = std::move(autoHealPlan),
                    .baseRegeneratePlan = std::move(baseRegeneratePlan),
                    .crateCollidePlan = std::move(crateCollidePlan),
                    .containmentPlan = std::move(containmentPlan),
                    .poisonedPlan = std::move(poisonedPlan),
                    .overchargePlan = std::move(overchargePlan),
                    .weaponBonusUpdatePlan = std::move(weaponBonusUpdatePlan),
                    .fireWeaponWhenDamagedPlan =
                        std::move(fireWeaponWhenDamagedPlan),
                    .fireWeaponUpdatePlan = std::move(fireWeaponUpdatePlan),
                    .fireWeaponCollidePlan =
                        std::move(fireWeaponCollidePlan),
                    .squishCollidePlan = std::move(squishCollidePlan),
                    .flammablePlan = std::move(flammablePlan),
                    .fireSpreadPlan = std::move(fireSpreadPlan),
                    .fireOclAfterCooldownPlan =
                        std::move(fireOclAfterCooldownPlan),
                    .empPlan = std::move(empPlan),
                    .leafletDropPlan = std::move(leafletDropPlan),
                    .stealthPlan = std::move(stealthPlan),
                    .stealthDetectorPlan =
                        std::move(stealthDetectorPlan),
                    .grantStealthPlan = std::move(grantStealthPlan),
                    .dynamicShroudPlan = std::move(dynamicShroudPlan),
                    .dynamicGeometryPlan =
                        std::move(dynamicGeometryPlan),
                    .enemyNearPlan = std::move(enemyNearPlan),
                    .animationSteeringPlan =
                        std::move(animationSteeringPlan),
                    .tacticalPlan = std::move(tacticalPlan),
                    .economyPlan = std::move(economyPlan),
                    .builderPlan = std::move(builderPlan),
                    .rebuildHolePlan = std::move(rebuildHolePlan),
                    .checkpointPlan = std::move(checkpointPlan),
                    .cleanupHazardPlan = std::move(cleanupHazardPlan),
                    .minefieldPlan = std::move(minefieldPlan),
                    .neutronMissileSlowDeathPlan =
                        std::move(neutronMissileSlowDeathPlan),
                    .countermeasuresPlan =
                        std::move(countermeasuresPlan),
                    .smartBombPlan = std::move(smartBombPlan),
                    .stickyBombPlan = std::move(stickyBombPlan),
                    .waveGuidePlan = std::move(waveGuidePlan),
                    .spyVisionPlan = std::move(spyVisionPlan),
                    .specialPowerPlan = std::move(specialPowerPlan),
                    .missileLauncherBuildingPlan =
                        std::move(missileLauncherBuildingPlan),
                    .particleUplinkCannonPlan =
                        std::move(particleUplinkCannonPlan),
                    .supplyWarehouseCripplingPlan =
                        std::move(supplyWarehouseCripplingPlan),
                    .floatPlan = std::move(floatPlan),
                    .heightDiePlan = std::move(heightDiePlan),
                    .lifetimePlan = std::move(lifetimePlan),
                    .oclUpdatePlan = std::move(oclUpdatePlan),
                    .productionPlan = std::move(productionPlan),
                    .productionExitPlan = std::move(productionExitPlan),
                    .radiusDecalPlan = std::move(radiusDecalPlan),
                    .techBuildingPlan = std::move(techBuildingPlan),
                    .objectUpgradePlan = std::move(objectUpgradePlan),
                    .createPlan = std::move(createPlan),
                    .hasAiUpdate = hasAiUpdate,
                    .aiRecipe = aiRecipe.recipe,
                    .initialAiOrderCapabilities = initialAiCapabilities,
                    .recipeFingerprint = finalFingerprint,
                    .requiresInterfaceResolution = state.requiresInterfaceResolution,
                    .diagnostics = std::move(state.diagnostics),
                }),
                .valid = recipeValid,
            };
            });
            recipeStageMicros +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - recipeStageBegin).count();
        }
    };

    for (size_t index = 0; index < parseJobs.size(); ++index) {
        const ParseJob& job = parseJobs[index];
        // NOTE: `layerIndex` is always 0 today, so the Overlay arm is dead.
        // Each ThingIniSource holds exactly one layer (see the readAll call
        // that fills source.layers) because INI loading resolves a single VFS
        // winner per logical path — MOD, else ZH, else Generals — rather than
        // replaying every layer.  Base is the correct mode for that single
        // winner: it is a complete definition, not an overlay onto a lower
        // layer.  The arm is retained for the multi-layer shape, not a bug.
        compileBlocks(parsedLayers[index].blocks,
                      ini::createsOverrides(loadType)
                          ? RecipeLoadMode::StrictCreateOverrides
                          : job.layerIndex != 0u
                              ? RecipeLoadMode::Overlay
                              : RecipeLoadMode::Base,
                      sources[job.sourceIndex].path);
    }

    const auto recipeProjectionEnd = std::chrono::steady_clock::now();
    if (!recipeCompileJobs.empty()) {
        if (platform::runtime::sceneResourceWorkerCount() <= 1u ||
            recipeCompileJobs.size() == 1u) {
            for (const auto& job : recipeCompileJobs) job();
        } else {
            tf::Taskflow recipeFlow;
            recipeFlow.for_each_index(
                size_t{0}, recipeCompileJobs.size(), size_t{1},
                [&recipeCompileJobs](size_t index) {
                    recipeCompileJobs[index]();
                });
            try {
                platform::runtime::sceneResourceExecutor()
                    .run(recipeFlow).get();
            } catch (...) {
                return false;
            }
        }
    }
    const auto recipePlanEnd = std::chrono::steady_clock::now();
    for (CompiledRecipeResult& result : compiledRecipes) {
        if (!result.archetype) {
            valid = false;
            continue;
        }
        valid &= result.valid;
        for (const ObjectRecipeDiagnostic& diagnostic :
             result.archetype->diagnostics) {
            processContentDiagnostics().warn({
                .source = diagnostic.sourcePath.empty()
                    ? "data/ini/Object" : diagnostic.sourcePath,
                .sourceLine = diagnostic.sourceLine,
                .block = "Object",
                .definition = result.name,
                .module = "ThingFactory",
                .adoptedValue = diagnostic.severity ==
                        ObjectRecipeDiagnosticSeverity::Error
                    ? "partial recipe; invalid branch disabled"
                    : "authored value retained where safe",
                .reason = diagnostic.message,
            });
        }
        m_archetypes.insert_or_assign(
            result.name, std::move(result.archetype));
    }

    const auto compileEnd = std::chrono::steady_clock::now();
    const auto parseMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        parseEnd - loadBegin).count();
    const auto projectionMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            recipeProjectionEnd - parseEnd).count();
    const auto planMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        recipePlanEnd - recipeProjectionEnd).count();
    const auto commitMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        compileEnd - recipePlanEnd).count();
    TD_LOG_INFO(
        "[ThingFactory] Loaded {} object recipes from {} sources/{} layers "
        "(parse={}us projection={}us [inherit={}us entries={}us combat={}us "
        "finalize={}us stage={}us] plans={}us commit={}us workers={})",
        m_things.size(), sources.size(), parseJobs.size(), parseMicros,
        projectionMicros, inheritanceMicros, entryApplyMicros, combatMicros,
        projectionFinalizeMicros, recipeStageMicros, planMicros, commitMicros,
        platform::runtime::sceneResourceWorkerCount());
    return valid;
}

bool ThingFactory::loadFromIni(
    const container::String& filePath, ini::LegacyIniLoadType loadType) {
    ThingIniSource source;
    source.path = filePath;

    auto& vfs = io::VFS::instance();
    if (vfs.exists(filePath)) {
        source.layers.push_back(vfs.readAll(filePath));
    } else {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return false;
        source.layers.emplace_back(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>());
    }
    if (source.layers.empty()) return false;
    return loadFromIniSources(
        container::Span<const ThingIniSource>{&source, 1u}, loadType);
}


} // namespace game
