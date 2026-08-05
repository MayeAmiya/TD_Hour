#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
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

ModelAnimationMode parseAnimationMode(container::StringView value) {
    const container::String mode = lowerAscii(container::String(value));
    if (mode == "manual") return ModelAnimationMode::Manual;
    if (mode == "loop") return ModelAnimationMode::Loop;
    if (mode == "loop_pingpong") return ModelAnimationMode::LoopPingPong;
    if (mode == "loop_backwards") return ModelAnimationMode::LoopBackwards;
    if (mode == "once_backwards") return ModelAnimationMode::OnceBackwards;
    return ModelAnimationMode::Once;
}

container::Vector<container::StringView> whitespaceTokens(container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n", cursor);
        result.push_back(value.substr(
            cursor, end == container::StringView::npos ? value.size() - cursor
                                                   : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end;
    }
    return result;
}

std::optional<ModelAnimationCandidate> parseModelAnimationCandidate(
    container::StringView authored, bool idle) {
    const container::Vector<container::StringView> tokens = whitespaceTokens(authored);
    if (tokens.empty() || lowerAscii(container::String(tokens[0])) == "none") {
        return std::nullopt;
    }
    ModelAnimationCandidate result;
    result.resource = container::String(tokens[0]);
    result.idle = idle;
    if (tokens.size() >= 2) result.distanceCovered = parseFloat(tokens[1]);
    if (tokens.size() >= 3) {
        result.selectionWeight = std::max<uint32_t>(1u,
            static_cast<uint32_t>(std::max<int32_t>(1, parseSigned(tokens[2]))));
    }
    return result;
}

container::Vector<ModelAnimationCandidate> parseModelAnimationCandidates(
    const ModuleData& state, bool& authoredAnimations) {
    container::Vector<ModelAnimationCandidate> result;
    authoredAnimations = false;
    for (const auto& [key, value] : state.values) {
        const bool idle = key == "IdleAnimation";
        if (key != "Animation" && !idle) continue;
        authoredAnimations = true;
        if (auto candidate = parseModelAnimationCandidate(value, idle)) {
            result.push_back(std::move(*candidate));
        }
    }
    return result;
}

void parseAnimationSpeedFactorRange(
    const ModuleData& state, float& minimum, float& maximum) {
    const container::String* authored = firstValue(state, "AnimationSpeedFactorRange");
    if (!authored) return;
    const container::Vector<container::StringView> tokens = whitespaceTokens(*authored);
    if (tokens.empty()) return;
    minimum = parseFloat(tokens[0]);
    maximum = tokens.size() >= 2 ? parseFloat(tokens[1]) : minimum;
    if (!std::isfinite(minimum)) minimum = 1.0f;
    if (!std::isfinite(maximum)) maximum = minimum;
    if (minimum > maximum) std::swap(minimum, maximum);
}

void applySubObjectVisibility(
    const ModuleData& state,
    container::Vector<ModelSubObjectVisibility>& visibility) {
    for (const auto& [key, authored] : state.values) {
        const bool visible = key == "ShowSubObject";
        if (!visible && key != "HideSubObject") continue;
        for (container::StringView name : whitespaceTokens(authored)) {
            if (lowerAscii(container::String(name)) == "none") {
                visibility.clear();
                continue;
            }
            const container::String canonical = lowerAscii(container::String(name));
            const auto existing = std::find_if(
                visibility.begin(), visibility.end(),
                [&canonical](const ModelSubObjectVisibility& value) {
                    return lowerAscii(value.name) == canonical;
                });
            if (existing == visibility.end()) {
                visibility.push_back({container::String(name), visible});
            } else {
                existing->visible = visible;
            }
        }
    }
}

void applyTurretDefinitions(
    const ModuleData& state,
    container::Array<ModelTurretBoneDefinition, 2>& turrets) {
    constexpr float kDegreesToRadians =
        3.14159265358979323846f / 180.0f;
    const auto apply = [&state, &turrets, kDegreesToRadians](
            size_t slot, container::StringView yawKey,
            container::StringView pitchKey, container::StringView artYawKey,
            container::StringView artPitchKey) {
        if (const container::String* value = firstValue(state, yawKey)) {
            turrets[slot].yawBone = lowerAscii(*value) == "none"
                ? container::String{} : *value;
        }
        if (const container::String* value = firstValue(state, pitchKey)) {
            turrets[slot].pitchBone = lowerAscii(*value) == "none"
                ? container::String{} : *value;
        }
        if (const container::String* value = firstValue(state, artYawKey)) {
            const float angle = parseFloat(*value) * kDegreesToRadians;
            turrets[slot].artYawRadians =
                std::isfinite(angle) ? angle : 0.0f;
            turrets[slot].artYawRadiansFixed =
                math::q32_32{turrets[slot].artYawRadians};
        }
        if (const container::String* value = firstValue(state, artPitchKey)) {
            const float angle = parseFloat(*value) * kDegreesToRadians;
            turrets[slot].artPitchRadians =
                std::isfinite(angle) ? angle : 0.0f;
            turrets[slot].artPitchRadiansFixed =
                math::q32_32{turrets[slot].artPitchRadians};
        }
    };
    apply(0, "Turret", "TurretPitch", "TurretArtAngle", "TurretArtPitch");
    apply(1, "AltTurret", "AltTurretPitch",
          "AltTurretArtAngle", "AltTurretArtPitch");
}

[[nodiscard]] size_t weaponSlotIndex(container::StringView authored) noexcept {
    const container::String slot = lowerAscii(container::String(authored));
    if (slot == "primary") return 0;
    if (slot == "secondary") return 1;
    if (slot == "tertiary") return 2;
    return 3;
}

void applyWeaponBoneDefinitions(
    const ModuleData& state,
    container::Array<ModelWeaponBoneDefinition, 3>& definitions) {
    for (const auto& [key, authored] : state.values) {
        if (key != "WeaponLaunchBone" && key != "WeaponFireFXBone" &&
            key != "WeaponRecoilBone" && key != "WeaponMuzzleFlash" &&
            key != "WeaponHideShowBone") {
            continue;
        }
        const container::Vector<container::StringView> tokens = whitespaceTokens(authored);
        if (tokens.size() < 2) continue;
        const size_t slot = weaponSlotIndex(tokens[0]);
        if (slot >= definitions.size()) continue;
        const container::String value = lowerAscii(container::String(tokens[1])) == "none"
            ? container::String{} : container::String(tokens[1]);
        if (key == "WeaponLaunchBone") definitions[slot].launchBone = value;
        else if (key == "WeaponFireFXBone") definitions[slot].fireFxBone = value;
        else if (key == "WeaponRecoilBone") definitions[slot].recoilBone = value;
        else if (key == "WeaponMuzzleFlash") definitions[slot].muzzleFlash = value;
        else definitions[slot].hideShowBone = value;
    }
}

void appendParticleSystemBones(
    const ModuleData& state,
    container::Vector<ModelParticleSystemBoneDefinition>& definitions) {
    for (const auto& [key, authored] : state.values) {
        if (key != "ParticleSysBone") continue;
        const container::Vector<container::StringView> tokens = whitespaceTokens(authored);
        if (tokens.size() < 2 || lowerAscii(container::String(tokens[1])) == "none") {
            continue;
        }
        definitions.push_back({
            .boneName = lowerAscii(container::String(tokens[0])) == "none"
                ? container::String{} : container::String(tokens[0]),
            .particleSystem = container::String(tokens[1]),
        });
    }
}

[[nodiscard]] ModelWeaponRecoilProfile recoilProfile(const ModuleData& draw) {
    ModelWeaponRecoilProfile profile;
    const auto assignFinite = [&draw](container::StringView key, float& output,
                                      float authoredScale = 1.0f) {
        if (const container::String* value = firstValue(draw, key)) {
            const float parsed = parseFloat(*value);
            if (std::isfinite(parsed)) output = parsed * authoredScale;
        }
    };
    // RefCode parses velocity authoring as world units per second and stores
    // world units per fixed 30 Hz logic frame. Defaults are already stored in
    // per-frame units, so scale only explicitly authored velocity fields.
    constexpr float kVelocityPerSecondToLogicFrame = 1.0f / 30.0f;
    assignFinite(
        "InitialRecoilSpeed", profile.initialSpeed,
        kVelocityPerSecondToLogicFrame);
    assignFinite("MaxRecoilDistance", profile.maximumDistance);
    assignFinite("RecoilDamping", profile.damping);
    assignFinite(
        "RecoilSettleSpeed", profile.settleSpeed,
        kVelocityPerSecondToLogicFrame);
    profile.initialSpeed = std::max(0.0f, profile.initialSpeed);
    profile.maximumDistance = std::max(0.0f, profile.maximumDistance);
    profile.damping = std::clamp(profile.damping, 0.0f, 1.0f);
    profile.settleSpeed = std::max(0.0f, profile.settleSpeed);
    return profile;
}

[[nodiscard]] ModelAnimationFlags parseModelAnimationFlags(
    container::StringView authored) {
    ModelAnimationFlags result = 0;
    const auto add = [&result, authored](container::StringView name,
                                         ModelAnimationFlag flag) {
        if (hasAsciiToken(authored, name)) result |= modelAnimationFlagBit(flag);
    };
    add("RANDOMSTART", ModelAnimationFlag::RandomStart);
    add("START_FRAME_FIRST", ModelAnimationFlag::StartFrameFirst);
    add("START_FRAME_LAST", ModelAnimationFlag::StartFrameLast);
    add("ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT",
        ModelAnimationFlag::AdjustHeightByConstructionPercent);
    add("PRISTINE_BONE_POS_IN_FINAL_FRAME",
        ModelAnimationFlag::PristineBonePositionInFinalFrame);
    add("MAINTAIN_FRAME_ACROSS_STATES",
        ModelAnimationFlag::MaintainFrameAcrossStates);
    add("RESTART_ANIM_WHEN_COMPLETE",
        ModelAnimationFlag::RestartAnimationWhenComplete);
    add("MAINTAIN_FRAME_ACROSS_STATES2",
        ModelAnimationFlag::MaintainFrameAcrossStates2);
    add("MAINTAIN_FRAME_ACROSS_STATES3",
        ModelAnimationFlag::MaintainFrameAcrossStates3);
    add("MAINTAIN_FRAME_ACROSS_STATES4",
        ModelAnimationFlag::MaintainFrameAcrossStates4);
    return result;
}

[[nodiscard]] container::String normalizedOptionalName(
    container::StringView authored) {
    container::String result = lowerAscii(container::String(authored));
    if (result == "none") result.clear();
    return result;
}

bool parseTransitionEndpoints(container::StringView text, container::String& source, container::String& destination) {
    const size_t split = text.find_first_of(" \t");
    if (split == container::StringView::npos) return false;
    source = normalizedOptionalName(text.substr(0, split));
    const size_t destinationStart = text.find_first_not_of(" \t", split);
    if (destinationStart == container::StringView::npos) return false;
    const size_t destinationEnd = text.find_first_of(" \t", destinationStart);
    destination = normalizedOptionalName(
        text.substr(destinationStart, destinationEnd - destinationStart));
    return !source.empty() && !destination.empty() && source != destination;
}

void appendVisualRules(
    const ModuleData& draw,
    container::Vector<ModelConditionVisualRule>& conditionVisuals,
    container::Vector<ModelConditionTransitionRule>& transitions) {
    // `modelConditionVisuals` grows while the INI is parsed.  Pointers into
    // that vector become invalid on reallocation, which used to corrupt
    // large Draw blocks as soon as a later ConditionState inherited from the
    // default state.  Keep stable indices and reacquire references only for
    // the operation that needs them.
    std::optional<size_t> previousIndex;
    std::optional<size_t> defaultRuleIndex;
    // In RefCode W3DModelDraw::replaceIndicatorColor() only rebuilds a
    // render object when this module-level opt-in is true.  Keep that
    // permission on every visual rule generated from this Draw module;
    // different Draw modules may legitimately have different settings.
    const bool allowsModelColorChange = [&draw]() {
        const container::String* value = firstValue(draw, "OkToChangeModelColor");
        return value && parseBool(*value);
    }();
    const ModelWeaponRecoilProfile drawRecoil = recoilProfile(draw);
    for (const ModuleData& state : draw.children) {
        if (state.type == "DefaultConditionState" || state.type == "ConditionState") {
            ModelConditionVisualRule rule;
            if (state.type == "ConditionState" && defaultRuleIndex) {
                // RefCode clones the complete DefaultConditionState and then
                // replaces only the condition mask. This includes TransitionKey
                // and every animation flag; selectively copying fields loses
                // authored inheritance for construction and transition states.
                rule = conditionVisuals[*defaultRuleIndex];
            }
            rule.allowsModelColorChange = allowsModelColorChange;
            rule.recoil = drawRecoil;
            rule.acceptedConditions.clear();
            rule.acceptedConditions.push_back(
                state.type == "DefaultConditionState" ? ModelConditionMask{}
                                                      : parseModelConditionMask(state.tag));
            if (const container::String* model = firstValue(state, "Model")) rule.model = *model;
            bool authoredAnimations = false;
            container::Vector<ModelAnimationCandidate> animations =
                parseModelAnimationCandidates(state, authoredAnimations);
            if (authoredAnimations) {
                rule.animationCandidates = std::move(animations);
                rule.animation = rule.animationCandidates.empty()
                    ? container::String{}
                    : rule.animationCandidates.front().resource;
            }
            if (const container::String* transitionKey = firstValue(state, "TransitionKey")) {
                rule.transitionKey = normalizedOptionalName(*transitionKey);
            }
            if (const container::String* animationMode = firstValue(state, "AnimationMode")) {
                rule.animationMode = parseAnimationMode(*animationMode);
            }
            if (const container::String* flags = firstValue(state, "Flags")) {
                rule.animationFlags = parseModelAnimationFlags(*flags);
                rule.pristineBonePositionInFinalFrame =
                    (rule.animationFlags & modelAnimationFlagBit(
                        ModelAnimationFlag::PristineBonePositionInFinalFrame)) != 0;
            }
            parseAnimationSpeedFactorRange(
                state, rule.animationSpeedFactorMinimum,
                rule.animationSpeedFactorMaximum);
            if (const container::String* wait = firstValue(
                    state, "WaitForStateToFinishIfPossible")) {
                rule.waitForStateToFinishKey = normalizedOptionalName(*wait);
            }
            applySubObjectVisibility(state, rule.subObjectVisibility);
            applyTurretDefinitions(state, rule.turrets);
            applyWeaponBoneDefinitions(state, rule.weaponBones);
            appendParticleSystemBones(state, rule.particleSystemBones);
            if (lowerAscii(rule.model) == "none") rule.model.clear();
            conditionVisuals.push_back(std::move(rule));
            previousIndex = conditionVisuals.size() - 1;
            if (state.type == "DefaultConditionState") defaultRuleIndex = previousIndex;
        } else if (state.type == "AliasConditionState" && previousIndex) {
            conditionVisuals[*previousIndex].acceptedConditions.push_back(
                parseModelConditionMask(state.tag));
        } else if (state.type == "TransitionState") {
            ModelConditionTransitionRule transition;
            transition.recoil = drawRecoil;
            if (!parseTransitionEndpoints(state.tag, transition.sourceKey, transition.destinationKey)) continue;
            if (defaultRuleIndex) {
                const ModelConditionVisualRule& defaultRule =
                    conditionVisuals[*defaultRuleIndex];
                transition.model = defaultRule.model;
                transition.animation = defaultRule.animation;
                transition.animationCandidates = defaultRule.animationCandidates;
                transition.animationFlags = defaultRule.animationFlags;
                transition.animationSpeedFactorMinimum =
                    defaultRule.animationSpeedFactorMinimum;
                transition.animationSpeedFactorMaximum =
                    defaultRule.animationSpeedFactorMaximum;
                transition.subObjectVisibility =
                    defaultRule.subObjectVisibility;
                transition.turrets = defaultRule.turrets;
                transition.weaponBones = defaultRule.weaponBones;
                transition.particleSystemBones =
                    defaultRule.particleSystemBones;
            }
            if (const container::String* model = firstValue(state, "Model")) transition.model = *model;
            bool authoredAnimations = false;
            container::Vector<ModelAnimationCandidate> animations =
                parseModelAnimationCandidates(state, authoredAnimations);
            if (authoredAnimations) {
                transition.animationCandidates = std::move(animations);
                transition.animation = transition.animationCandidates.empty()
                    ? container::String{}
                    : transition.animationCandidates.front().resource;
            }
            if (const container::String* animationMode = firstValue(state, "AnimationMode")) {
                transition.animationMode = parseAnimationMode(*animationMode);
            }
            if (const container::String* flags = firstValue(state, "Flags")) {
                transition.animationFlags = parseModelAnimationFlags(*flags);
            }
            parseAnimationSpeedFactorRange(
                state, transition.animationSpeedFactorMinimum,
                transition.animationSpeedFactorMaximum);
            if (const container::String* wait = firstValue(
                    state, "WaitForStateToFinishIfPossible")) {
                transition.waitForStateToFinishKey = normalizedOptionalName(*wait);
            }
            applySubObjectVisibility(
                state, transition.subObjectVisibility);
            applyTurretDefinitions(state, transition.turrets);
            applyWeaponBoneDefinitions(state, transition.weaponBones);
            appendParticleSystemBones(
                state, transition.particleSystemBones);
            // RefCode only permits one-shot transition animations. Invalid
            // legacy content is ignored here instead of leaking an undefined
            // looping transition into the modern renderer.
            if (transition.animationMode == ModelAnimationMode::Once ||
                transition.animationMode == ModelAnimationMode::OnceBackwards) {
                if (lowerAscii(transition.model) == "none") transition.model.clear();
                transitions.push_back(std::move(transition));
            }
        }
    }
}

const container::String* findDefaultModel(const ModuleData& module) {
    // RefCode permits a W3DModelDraw with no DefaultConditionState when its
    // first ordinary ConditionState is the empty/NONE condition.  Shipped
    // bridge-tower reskins use that form, so treating only the explicit
    // DefaultConditionState as the channel default drops their model asset.
    const bool isDefaultState =
        module.type == "DefaultConditionState" ||
        (module.type == "ConditionState" &&
         lowerAscii(module.tag) == "none");
    if (isDefaultState) {
        if (const auto found = module.properties.find("Model"); found != module.properties.end() &&
            !found->second.empty()) {
            return &found->second;
        }
    }
    // W3DTreeDraw is a terrain-buffer Draw module rather than a
    // W3DModelDraw subclass. It authors one static ModelName directly on the
    // Draw block and has no DefaultConditionState child. Preserve it in the
    // same immutable visual-channel slot so client-only optimized trees can
    // enter the ordinary static W3D snapshot/upload path.
    if (module.type == "Draw" &&
        lowerAscii(module.moduleClass) == "w3dtreedraw") {
        if (const container::String* model = firstValue(module, "ModelName");
            model && !model->empty()) {
            return model;
        }
    }
    for (const ModuleData& child : module.children) {
        if (const container::String* model = findDefaultModel(child)) return model;
    }
    return nullptr;
}


} // namespace game::detail
