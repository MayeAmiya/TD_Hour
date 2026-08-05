#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "CombatProfile.h"
#include "ObjectModuleCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include "ThingRecipeDetail.h"

namespace game::detail {

[[nodiscard]] std::optional<std::pair<container::String, container::String>>
parseObjectReskinHeader(container::StringView value) {
    const container::Vector<container::StringView> tokens = splitWhitespace(value);
    if (tokens.size() != 2 || tokens[0].empty() || tokens[1].empty()) return std::nullopt;
    return std::pair<container::String, container::String>{container::String(tokens[0]), container::String(tokens[1])};
}

[[nodiscard]] VehicleDrawVisualRecipe compileVehicleDrawRecipe(
    const ModuleData& module) {
    VehicleDrawVisualRecipe recipe;
    const container::String moduleClass = lowerAscii(module.moduleClass);
    if (moduleClass == "w3dtruckdraw" ||
        moduleClass == "w3doverlordtruckdraw") {
        recipe.kind = VehicleDrawKind::Truck;
    } else if (moduleClass == "w3dtankdraw" ||
               moduleClass == "w3doverlordtankdraw") {
        recipe.kind = VehicleDrawKind::Tank;
        // RefCode's W3DTankDraw default remains enabled for retail INI data.
        recipe.treadDebrisLeft = "TrackDebrisDirtLeft";
        recipe.treadDebrisRight = "TrackDebrisDirtRight";
    } else if (moduleClass == "w3dtanktruckdraw") {
        recipe.kind = VehicleDrawKind::TankTruck;
    } else {
        return recipe;
    }

    const auto copyText = [&module](container::StringView key,
                                    container::String& destination) {
        if (const container::String* value = firstValue(module, key)) {
            destination = *value;
            if (lowerAscii(destination) == "none") destination.clear();
        }
    };
    const auto copyFloat = [&module](container::StringView key,
                                     float& destination) {
        if (const container::String* value = firstValue(module, key)) {
            const float parsed = parseFloat(*value);
            if (std::isfinite(parsed)) destination = parsed;
        }
    };
    copyText("Dust", recipe.dustParticleSystem);
    copyText("DirtSpray", recipe.dirtParticleSystem);
    copyText("PowerslideSpray", recipe.powerslideParticleSystem);
    constexpr container::Array<container::StringView, 10> tireFields{
        "LeftFrontTireBone", "RightFrontTireBone",
        "LeftRearTireBone", "RightRearTireBone",
        "MidLeftFrontTireBone", "MidRightFrontTireBone",
        "MidLeftRearTireBone", "MidRightRearTireBone",
        "MidLeftMidTireBone", "MidRightMidTireBone",
    };
    for (size_t index = 0; index < tireFields.size(); ++index) {
        copyText(tireFields[index], recipe.tireBones[index]);
    }
    copyText("CabBone", recipe.cabBone);
    copyText("TrailerBone", recipe.trailerBone);
    copyFloat("TireRotationMultiplier", recipe.tireRotationMultiplier);
    copyFloat("PowerslideRotationAddition",
              recipe.powerslideRotationAddition);
    copyFloat("CabRotationMultiplier", recipe.cabRotationMultiplier);
    copyFloat("TrailerRotationMultiplier", recipe.trailerRotationMultiplier);
    copyFloat("RotationDamping", recipe.rotationDamping);
    copyText("TreadDebrisLeft", recipe.treadDebrisLeft);
    copyText("TreadDebrisRight", recipe.treadDebrisRight);
    copyFloat("TreadAnimationRate", recipe.treadAnimationRatePerSecond);
    copyFloat("TreadPivotSpeedFraction", recipe.treadPivotSpeedFraction);
    copyFloat("TreadDriveSpeedFraction", recipe.treadDriveSpeedFraction);
    recipe.treadPivotSpeedFraction = std::max(
        0.0f, recipe.treadPivotSpeedFraction);
    recipe.treadDriveSpeedFraction = std::max(
        0.0f, recipe.treadDriveSpeedFraction);
    return recipe;
}

[[nodiscard]] bool hasModuleClass(
    const ThingAuthoringTemplate& templateData,
    container::StringView moduleClass) {
    const container::String expected = lowerAscii(
        container::String{moduleClass});
    return std::any_of(
        templateData.modules.begin(), templateData.modules.end(),
        [&expected](const ModuleData& module) {
            return lowerAscii(module.moduleClass) == expected;
        });
}

void appendDemoTrapDyingModelAnimation(
    ModelDrawVisualChannel& channel) {
    if (channel.defaultModel.empty() || channel.conditionVisuals.empty()) {
        return;
    }

    const game::ModelConditionMask dying =
        game::modelConditionMaskOf(game::ModelConditionFlag::Dying);
    const bool hasAuthoredDyingState = std::any_of(
        channel.conditionVisuals.begin(), channel.conditionVisuals.end(),
        [&dying](const ModelConditionVisualRule& rule) {
            return std::any_of(
                rule.acceptedConditions.begin(),
                rule.acceptedConditions.end(),
                [&dying](const game::ModelConditionMask& accepted) {
                    return accepted.intersectionCount(dying) != 0;
                });
        });
    if (hasAuthoredDyingState) return;

    const auto source = std::find_if(
        channel.conditionVisuals.begin(), channel.conditionVisuals.end(),
        [&channel](const ModelConditionVisualRule& rule) {
            return rule.model == channel.defaultModel &&
                std::any_of(
                    rule.acceptedConditions.begin(),
                    rule.acceptedConditions.end(),
                    [](const game::ModelConditionMask& accepted) {
                        return accepted.empty();
                    });
        });
    if (source == channel.conditionVisuals.end()) return;

    ModelConditionVisualRule rule = *source;
    rule.acceptedConditions = {dying};
    rule.animation = rule.model + "." + rule.model;
    rule.animationCandidates = {{
        .resource = rule.animation,
    }};
    rule.animationMode = ModelAnimationMode::Once;
    rule.animationFlags = modelAnimationFlagBit(
        ModelAnimationFlag::StartFrameFirst);
    rule.pristineBonePositionInFinalFrame = false;
    rule.transitionKey.clear();
    rule.waitForStateToFinishKey.clear();
    channel.conditionVisuals.push_back(std::move(rule));
}

void rebuildBodyAndDrawProjection(ThingAuthoringTemplate& templateData,
                                  TemplateRecipeParseState& state) {
    // Structural recipe operations (RemoveModule/ReplaceModule) can remove a
    // Body or Draw after the incremental parser saw it. Rebuild the two typed
    // projections consumed by Stage-0 ECS from the final raw recipe so a
    // removed module never leaves stale HP or a stale W3D model behind.
    ObjectBodyAuthoringTemplate rebuiltBody;
    bool hasBody = false;
    for (const ModuleData& module : templateData.modules) {
        if (const std::optional<ObjectBodyKind> kind = bodyKindFor(module)) {
            applyBodyModule(rebuiltBody, *kind, module);
            hasBody = true;
        }
    }
    if (hasBody) {
        templateData.body = std::move(rebuiltBody);
    } else if (templateData.body.fromBodyModule) {
        // An explicit Body removal produces the safe modern counterpart of no
        // live Body interface: non-damageable, zero-health data. The full
        // archetype validator can later reject content that requires a Body.
        templateData.body = {};
        templateData.body.kind = ObjectBodyKind::Inactive;
        templateData.body.maximumHealth = 0.0f;
        templateData.body.initialHealth = 0.0f;
        templateData.body.normalize();
    }

    templateData.drawModule.clear();
    templateData.defaultW3dModel.clear();
    templateData.modelConditionVisuals.clear();
    templateData.modelConditionTransitions.clear();
    templateData.drawVisualChannels.clear();
    templateData.ignoredModelConditions = {};
    templateData.trackMarksVisuals.clear();
    const bool demoTrapModelAnimation =
        hasModuleClass(templateData, "DemoTrapUpdate");
    for (const ModuleData& module : templateData.modules) {
        if (module.type != "Draw") continue;
        if (templateData.drawModule.empty()) templateData.drawModule = module.tag;

        ModelDrawVisualChannel channel;
        channel.sourceModuleClass = module.moduleClass;
        channel.sourceModuleTag = module.moduleTag;
        channel.vehicleDraw = compileVehicleDrawRecipe(module);
        if (lowerAscii(module.moduleClass) == "w3dsupplydraw") {
            if (const container::String* value =
                    firstValue(module, "SupplyBonePrefix")) {
                channel.supplyDraw.bonePrefix = normalizedOptionalName(*value);
            }
        }
        channel.policeCarDraw.active =
            lowerAscii(module.moduleClass) == "w3dpolicecardraw";
        if (templateData.defaultW3dModel.empty()) {
            if (const container::String* model = findDefaultModel(module)) {
                if (lowerAscii(*model) != "none") {
                    templateData.defaultW3dModel = *model;
                }
            }
        }
        if (const container::String* model = findDefaultModel(module)) {
            channel.defaultModel = lowerAscii(*model) == "none"
                ? container::String{} : *model;
        }
        appendVisualRules(
            module, channel.conditionVisuals, channel.transitions);
        if (demoTrapModelAnimation &&
            channel.defaultModel == templateData.defaultW3dModel) {
            // DemoTrapUpdate's warning motion is the current model's own W3D
            // clip.  Keep it on the ordinary DYING ConditionState path:
            // health/model-condition authority selects the state, while the
            // renderer samples the model-authored hierarchy animation.  No
            // object-name switch, FX transform or ad-hoc wobble is involved.
            appendDemoTrapDyingModelAnimation(channel);
        }
        if (const container::String* ignored = firstValue(module, "IgnoreConditionStates")) {
            channel.ignoredConditions = parseModelConditionMask(*ignored);
            for (size_t word = 0;
                 word < templateData.ignoredModelConditions.words.size(); ++word) {
                templateData.ignoredModelConditions.words[word] |=
                    channel.ignoredConditions.words[word];
            }
        }
        if (const container::String* value = firstValue(
                module, "AnimationsRequirePower")) {
            channel.animationsRequirePower = parseBool(*value);
        }
        if (const container::String* value = firstValue(
                module, "ParticlesAttachedToAnimatedBones")) {
            channel.particlesAttachedToAnimatedBones = parseBool(*value);
        }
        if (const container::String* value = firstValue(
                module, "ProjectileBoneFeedbackEnabledSlots")) {
            for (container::StringView token : splitWhitespace(*value)) {
                const container::String slot = lowerAscii(
                    container::String{token});
                if (slot == "primary") {
                    channel.projectileBoneFeedbackEnabledSlots |= 1u << 0u;
                } else if (slot == "secondary") {
                    channel.projectileBoneFeedbackEnabledSlots |= 1u << 1u;
                } else if (slot == "tertiary") {
                    channel.projectileBoneFeedbackEnabledSlots |= 1u << 2u;
                }
            }
        }
        if (const container::String* value = firstValue(
                module, "ReceivesDynamicLights")) {
            channel.receivesDynamicLights = parseBool(*value);
        }
        if (const container::String* value = firstValue(
                module, "AttachToBoneInAnotherModule")) {
            channel.attachToBoneInAnotherModule = normalizedOptionalName(*value);
        }
        if (const container::String* value = firstValue(
                module, "AttachToBoneInContainer")) {
            channel.attachToBoneInContainer = normalizedOptionalName(*value);
        }
        if (const container::String* value = firstValue(module, "MinLODRequired")) {
            const container::String lod = lowerAscii(*value);
            channel.minimumLod = lod == "high"
                ? ModelDrawMinimumLod::High
                : lod == "medium"
                    ? ModelDrawMinimumLod::Medium
                    : ModelDrawMinimumLod::Low;
        }
        for (const auto& [key, value] : module.values) {
            if (key != "ExtraPublicBone") continue;
            const container::Vector<container::StringView> names =
                whitespaceTokens(value);
            for (const container::StringView name : names) {
                if (!name.empty() && lowerAscii(container::String(name)) != "none") {
                    channel.extraPublicBones.emplace_back(name);
                }
            }
        }

        // Keep the flattened compatibility projection until all gameplay and
        // script producers have moved to per-channel indices. New renderer
        // extraction consumes drawVisualChannels and never treats this legacy
        // vector as simultaneous Draw ownership.
        templateData.modelConditionVisuals.insert(
            templateData.modelConditionVisuals.end(),
            channel.conditionVisuals.begin(), channel.conditionVisuals.end());
        templateData.modelConditionTransitions.insert(
            templateData.modelConditionTransitions.end(),
            channel.transitions.begin(), channel.transitions.end());
        templateData.drawVisualChannels.push_back(std::move(channel));

        const container::String* trackMarks = nullptr;
        bool authoredTrackMarks = false;
        for (const auto& [key, value] : module.values) {
            if (key != "TrackMarks") continue;
            authoredTrackMarks = true;
            trackMarks = &value;
        }
        if (!authoredTrackMarks) continue;
        if (!trackMarks || trackMarks->empty() ||
            lowerAscii(*trackMarks) == "none") {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Warning,
                .message = "Draw module '" + module.moduleTag +
                    "' ignored empty TrackMarks texture",
            });
            continue;
        }
        templateData.trackMarksVisuals.push_back({
            .sourceModuleClass = module.moduleClass,
            .sourceModuleTag = module.moduleTag,
            .textureName = *trackMarks,
            // RefCode bindTrack(renderObject, 1 * MAP_XY_FACTOR, texture).
            // Model bone spacing and the 4-unit tread-width addition are
            // resolved only once the selected W3D pose is available.
            .leftWidthBone = container::String{
                track_marks::visual_defaults::kLeftWidthBone},
            .rightWidthBone = container::String{
                track_marks::visual_defaults::kRightWidthBone},
            .fallbackWidth = track_marks::visual_defaults::kFallbackWidth,
            .additionalTreadWidth =
                track_marks::visual_defaults::kAdditionalTreadWidth,
            .segmentLength = track_marks::visual_defaults::kSegmentLength,
        });
    }
}

void markModulesCopiedFromParent(ThingTemplate& templateData) {
    for (ModuleData& module : templateData.modules) {
        module.copiedFromParent = true;
    }
}

} // namespace game::detail
