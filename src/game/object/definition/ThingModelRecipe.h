#pragma once

#include "core/container/hash_containers.h"
#include "core/math/fixed/q32_32.h"
#include "ModelConditionState.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace game {

struct ThingTemplate;

struct ModelAnimationCandidate final {
    container::String resource;
    float distanceCovered = 0.0f;
    // RefCode expands the third token into repeated vector entries. Keeping
    // the equivalent weight explicitly preserves authored probability without
    // allocating duplicate strings.
    uint32_t selectionWeight = 1;
    bool idle = false;
};

struct ModelSubObjectVisibility final {
    container::String name;
    bool visible = true;
};

struct ModelTurretBoneDefinition final {
    container::String yawBone;
    container::String pitchBone;
    float artYawRadians = 0.0f;
    float artPitchRadians = 0.0f;
    math::q32_32 artYawRadiansFixed{};
    math::q32_32 artPitchRadiansFixed{};
};

struct ModelWeaponBoneDefinition final {
    container::String launchBone;
    container::String fireFxBone;
    container::String recoilBone;
    container::String muzzleFlash;
    container::String hideShowBone;
};

struct ModelWeaponRecoilProfile final {
    float initialSpeed = 2.0f;
    float maximumDistance = 3.0f;
    float damping = 0.4f;
    float settleSpeed = 0.065f;
};

struct ModelParticleSystemBoneDefinition final {
    container::String boneName;
    container::String particleSystem;
};

struct ModelConditionVisualRule {
    container::Vector<ModelConditionMask> acceptedConditions;
    container::String model;
    container::String animation;
    ModelAnimationMode animationMode = ModelAnimationMode::Once;
    ModelAnimationFlags animationFlags = 0;
    // W3DModelDraw's pristine-bone query samples the first animation at frame
    // zero unless this per-state flag requests its final frame instead.
    bool pristineBonePositionInFinalFrame = false;
    // W3DModelDraw's OkToChangeModelColor is authored on the containing
    // Draw module.  Preserve the resolved rule-local permission so renderer
    // extraction can carry a script indicator colour only to models which
    // RefCode would recreate with a house-colour override.
    bool allowsModelColorChange = false;
    // Logical names used to select a TransitionState when this normal state
    // replaces another normal state. Kept as names rather than RefCode's
    // global NameKey integer so templates are self-contained data.
    container::String transitionKey;
    container::Vector<ModelAnimationCandidate> animationCandidates;
    float animationSpeedFactorMinimum = 1.0f;
    float animationSpeedFactorMaximum = 1.0f;
    container::String waitForStateToFinishKey;
    container::Vector<ModelSubObjectVisibility> subObjectVisibility;
    container::Array<ModelTurretBoneDefinition, 2> turrets;
    container::Array<ModelWeaponBoneDefinition, 3> weaponBones;
    ModelWeaponRecoilProfile recoil;
    container::Vector<ModelParticleSystemBoneDefinition> particleSystemBones;
};

struct ModelConditionTransitionRule {
    container::String sourceKey;
    container::String destinationKey;
    container::String model;
    container::String animation;
    ModelAnimationMode animationMode = ModelAnimationMode::Once;
    ModelAnimationFlags animationFlags = 0;
    container::Vector<ModelAnimationCandidate> animationCandidates;
    float animationSpeedFactorMinimum = 1.0f;
    float animationSpeedFactorMaximum = 1.0f;
    container::String waitForStateToFinishKey;
    container::Vector<ModelSubObjectVisibility> subObjectVisibility;
    container::Array<ModelTurretBoneDefinition, 2> turrets;
    container::Array<ModelWeaponBoneDefinition, 3> weaponBones;
    ModelWeaponRecoilProfile recoil;
    container::Vector<ModelParticleSystemBoneDefinition> particleSystemBones;
};

enum class ModelDrawMinimumLod : uint8_t {
    Low,
    Medium,
    High,
};

enum class VehicleDrawKind : uint8_t {
    None,
    Truck,
    Tank,
    TankTruck,
};

// Typed value projection of W3DTruckDraw/W3DTankDraw/W3DTankTruckDraw. The
// renderer consumes only the confirmed runtime sample derived from this
// recipe; it never reparses Draw properties or guesses locomotor state.
struct VehicleDrawVisualRecipe final {
    VehicleDrawKind kind = VehicleDrawKind::None;
    container::String dustParticleSystem;
    container::String dirtParticleSystem;
    container::String powerslideParticleSystem;
    container::Array<container::String, 10> tireBones{};
    container::String cabBone;
    container::String trailerBone;
    float tireRotationMultiplier = 0.0f;
    float powerslideRotationAddition = 0.0f;
    float cabRotationMultiplier = 0.0f;
    float trailerRotationMultiplier = 0.0f;
    float rotationDamping = 0.0f;
    container::String treadDebrisLeft;
    container::String treadDebrisRight;
    float treadAnimationRatePerSecond = 0.0f;
    float treadPivotSpeedFraction = 0.6f;
    float treadDriveSpeedFraction = 0.3f;

    [[nodiscard]] bool enabled() const noexcept {
        return kind != VehicleDrawKind::None;
    }
};

// Typed values unique to W3DSupplyDraw/W3DPoliceCarDraw. They remain beside
// the ordinary W3DModelDraw channel because both legacy subclasses extend the
// same model, animation and condition-state ownership.
struct SupplyDrawVisualRecipe final {
    container::String bonePrefix;

    [[nodiscard]] bool enabled() const noexcept {
        return !bonePrefix.empty();
    }
};

struct PoliceCarDrawVisualRecipe final {
    bool active = false;
};

// Immutable, value-only equivalent of one RefCode Draw module. A Thing may
// own several channels simultaneously (body, doors, construction scaffolds,
// upgrades, dependency models). Keeping source identity and module policy on
// the channel prevents one Draw's condition selection from replacing every
// other Draw on the object.
struct ModelDrawVisualChannel final {
    container::String sourceModuleClass;
    container::String sourceModuleTag;
    container::String defaultModel;
    container::Vector<ModelConditionVisualRule> conditionVisuals;
    container::Vector<ModelConditionTransitionRule> transitions;
    ModelConditionMask ignoredConditions;
    container::Vector<container::String> extraPublicBones;
    container::String attachToBoneInAnotherModule;
    container::String attachToBoneInContainer;
    ModelDrawMinimumLod minimumLod = ModelDrawMinimumLod::Low;
    uint8_t projectileBoneFeedbackEnabledSlots = 0;
    bool animationsRequirePower = true;
    bool particlesAttachedToAnimatedBones = false;
    bool receivesDynamicLights = true;
    VehicleDrawVisualRecipe vehicleDraw;
    SupplyDrawVisualRecipe supplyDraw;
    PoliceCarDrawVisualRecipe policeCarDraw;
};

struct ModelAnimationSelection final {
    size_t candidateIndex = std::numeric_limits<size_t>::max();
    float speedFactor = 1.0f;
};

[[nodiscard]] std::optional<ModelAnimationCandidate>
parseModelAnimationDirective(container::StringView authored, bool idle);

[[nodiscard]] ModelAnimationSelection selectModelAnimation(
    const ModelConditionVisualRule& rule,
    uint64_t objectId,
    const ModelConditionMask& conditions,
    uint64_t stateGeneration = 0) noexcept;
[[nodiscard]] ModelAnimationSelection selectModelAnimation(
    const ModelConditionTransitionRule& rule,
    uint64_t objectId,
    const ModelConditionMask& conditions,
    uint64_t stateGeneration = 0) noexcept;
[[nodiscard]] size_t selectModelConditionVisualRuleIndex(
    const ThingTemplate& templateData,
    ModelConditionMask conditions) noexcept;
[[nodiscard]] size_t selectModelConditionVisualRuleIndex(
    const ModelDrawVisualChannel& channel,
    ModelConditionMask conditions) noexcept;

// The legacy object template owns a GeometryInfo value.  Keep the same
// authored shapes and derived radii as immutable content, rather than
// reconstructing ad-hoc bounds from a W3D asset at every gameplay query.
// Geometry is simulation data: it drives selection, broad phase and later
// collision; renderer bounds are only a presentation optimisation.

} // namespace game
