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
#include "game/object/component/ObjectComponentsPresentationModel.h"
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
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/FactionTemplate.h"
#include "game/script/runtime/ScriptProgram.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "game/terrain/TerrainLogic.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include "GameRenderExtractionDetail.h"
namespace engine::render_extraction_detail {

struct WeaponBoneStateSelection final {
    const game::ModuleData* state = nullptr;
    const game::ModuleData* defaultState = nullptr;
};

// Select the raw W3D condition state with the same SparseMatchFinder rule as
// the flattened visual model. Keeping this renderer extraction helper outside
// ThingTemplate avoids changing the concurrently migrated ECS/content layout,
// while still preserving condition-specific WeaponLaunchBone authoring.
[[nodiscard]] WeaponBoneStateSelection selectWeaponBoneState(
    const game::ThingTemplate& tmpl, game::ModelConditionMask conditions) {
    conditions.clear(tmpl.ignoredModelConditions);
    WeaponBoneStateSelection best;
    uint32_t bestYes = 0;
    uint32_t bestExtraneous = UINT32_MAX;

    const auto consider = [&](const game::ModuleData& state,
                              const game::ModuleData* defaultState,
                              const game::ModelConditionMask& required) {
        const uint32_t yes = conditions.intersectionCount(required);
        const uint32_t extraneous = required.extraneousCountAgainst(conditions);
        if (yes > bestYes || (yes >= bestYes && extraneous < bestExtraneous)) {
            best = {.state = &state, .defaultState = defaultState};
            bestYes = yes;
            bestExtraneous = extraneous;
        }
    };

    for (const game::ModuleData& draw : tmpl.modules) {
        if (!equalsInsensitive(draw.type, "Draw")) continue;
        const game::ModuleData* defaultState = nullptr;
        const game::ModuleData* previousState = nullptr;
        for (const game::ModuleData& child : draw.children) {
            if (equalsInsensitive(child.type, "DefaultConditionState")) {
                if (!defaultState) defaultState = &child;
                previousState = &child;
                consider(child, defaultState, {});
            } else if (equalsInsensitive(child.type, "ConditionState")) {
                previousState = &child;
                consider(child, defaultState,
                         game::parseModelConditionMask(child.tag));
            } else if (equalsInsensitive(child.type, "AliasConditionState") &&
                       previousState) {
                consider(*previousState, defaultState,
                         game::parseModelConditionMask(child.tag));
            }
        }
    }
    return best;
}

void applyWeaponLaunchBones(
    const game::ModuleData* state,
    container::Array<container::String, render::kRenderWeaponSlotCount>& output) {
    if (!state) return;
    for (const auto& [key, authored] : state->values) {
        const bool launchBone = equalsInsensitive(key, "WeaponLaunchBone");
        if (!launchBone) continue;
        const size_t slotBegin = authored.find_first_not_of(" \t");
        if (slotBegin == container::String::npos) continue;
        const size_t slotEnd = authored.find_first_of(" \t", slotBegin);
        const container::StringView slot = container::StringView(authored).substr(
            slotBegin, slotEnd - slotBegin);
        const size_t boneBegin = slotEnd == container::String::npos
            ? container::String::npos : authored.find_first_not_of(" \t", slotEnd);
        if (boneBegin == container::String::npos) continue;
        const size_t boneEnd = authored.find_first_of(" \t", boneBegin);
        const container::StringView bone = container::StringView(authored).substr(
            boneBegin, boneEnd - boneBegin);
        size_t slotIndex = render::kRenderWeaponSlotCount;
        if (equalsInsensitive(slot, "PRIMARY")) slotIndex = 0;
        else if (equalsInsensitive(slot, "SECONDARY")) slotIndex = 1;
        else if (equalsInsensitive(slot, "TERTIARY")) slotIndex = 2;
        if (slotIndex >= output.size()) continue;
        output[slotIndex] = equalsInsensitive(bone, "NONE")
            ? container::String{} : container::String(bone);
    }
}

[[nodiscard]] container::Array<container::String, render::kRenderWeaponSlotCount>
extractWeaponLaunchBones(const game::ThingTemplate& tmpl,
                         game::ModelConditionMask conditions) {
    container::Array<container::String, render::kRenderWeaponSlotCount> result;
    const WeaponBoneStateSelection selection =
        selectWeaponBoneState(tmpl, conditions);
    applyWeaponLaunchBones(selection.defaultState, result);
    if (selection.state != selection.defaultState) {
        applyWeaponLaunchBones(selection.state, result);
    }
    return result;
}

[[nodiscard]] container::Vector<render::RenderBoneControl> extractTurretControls(
    const ObjectWeaponComponent* weapons,
    const container::Array<game::ModelTurretBoneDefinition, 2>& definitions) {
    container::Vector<render::RenderBoneControl> result;
    result.reserve(definitions.size() * 2u);
    for (size_t index = 0; index < definitions.size(); ++index) {
        const game::ModelTurretBoneDefinition& definition = definitions[index];
        const float turretYaw = weapons
            ? weapons->turrets[index].yawRadians.to_float() : 0.0f;
        const float turretPitch = weapons
            ? weapons->turrets[index].pitchRadians.to_float() : 0.0f;
        if (!definition.yawBone.empty()) {
            result.push_back({
                .boneName = definition.yawBone,
                .rotation = math::quat::from_axis_angle(
                    {0.0f, 0.0f, 1.0f},
                    turretYaw + definition.artYawRadians),
            });
        }
        if (!definition.pitchBone.empty()) {
            result.push_back({
                .boneName = definition.pitchBone,
                .rotation = math::quat::from_axis_angle(
                    {0.0f, 1.0f, 0.0f},
                    -(turretPitch + definition.artPitchRadians)),
            });
        }
    }
    return result;
}

void appendVehicleDrawControls(
    const game::VehicleDrawVisualRecipe& recipe,
    const VehicleDrawChannelPresentationState& state,
    container::Vector<render::RenderBoneControl>& controls,
    render::RenderVehicleTreadState& treads) {
    const auto appendWheel = [&controls](container::StringView bone,
                                         float steering,
                                         float rotation,
                                         float height) {
        if (bone.empty()) return;
        controls.push_back({
            .boneName = container::String{bone},
            .translation = {0.0f, 0.0f, height},
            .rotation = math::quat::from_axis_angle(
                {0.0f, 0.0f, 1.0f}, steering) *
                math::quat::from_axis_angle(
                    {0.0f, 1.0f, 0.0f}, rotation),
        });
    };
    if (recipe.kind == game::VehicleDrawKind::Truck ||
        recipe.kind == game::VehicleDrawKind::TankTruck) {
        appendWheel(recipe.tireBones[0], state.wheelSteeringAngle,
                    state.frontWheelRotation, state.frontLeftWheelHeight);
        appendWheel(recipe.tireBones[1], state.wheelSteeringAngle,
                    state.frontWheelRotation, state.frontRightWheelHeight);
        appendWheel(recipe.tireBones[2], 0.0f, state.rearWheelRotation,
                    state.rearLeftWheelHeight);
        appendWheel(recipe.tireBones[3], 0.0f, state.rearWheelRotation,
                    state.rearRightWheelHeight);
        appendWheel(recipe.tireBones[4], state.wheelSteeringAngle,
                    state.frontWheelRotation, state.frontLeftWheelHeight);
        appendWheel(recipe.tireBones[5], state.wheelSteeringAngle,
                    state.frontWheelRotation, state.frontRightWheelHeight);
        appendWheel(recipe.tireBones[6], 0.0f, state.rearWheelRotation,
                    state.rearLeftWheelHeight);
        appendWheel(recipe.tireBones[7], 0.0f, state.rearWheelRotation,
                    state.rearRightWheelHeight);
        appendWheel(recipe.tireBones[8], 0.0f, state.rearWheelRotation,
                    state.rearLeftWheelHeight);
        appendWheel(recipe.tireBones[9], 0.0f, state.rearWheelRotation,
                    state.rearRightWheelHeight);
    }
    if (recipe.kind == game::VehicleDrawKind::Truck) {
        if (!recipe.cabBone.empty()) {
            controls.push_back({
                .boneName = recipe.cabBone,
                .rotation = math::quat::from_axis_angle(
                    {0.0f, 0.0f, 1.0f}, state.cabRotation),
            });
        }
        if (!recipe.trailerBone.empty()) {
            controls.push_back({
                .boneName = recipe.trailerBone,
                .rotation = math::quat::from_axis_angle(
                    {0.0f, 0.0f, 1.0f}, state.trailerRotation),
            });
        }
    }
    if (recipe.kind == game::VehicleDrawKind::Tank ||
        recipe.kind == game::VehicleDrawKind::TankTruck) {
        treads = {
            .leftOffset = state.treadLeftOffset,
            .rightOffset = state.treadRightOffset,
            .middleOffset = state.treadMiddleOffset,
            .enabled = recipe.treadAnimationRatePerSecond != 0.0f,
        };
    }
}

[[nodiscard]] container::Array<container::String, render::kRenderWeaponSlotCount>
typedWeaponLaunchBones(
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions) {
    container::Array<container::String, render::kRenderWeaponSlotCount> result;
    for (size_t slot = 0; slot < result.size(); ++slot) {
        result[slot] = definitions[slot].launchBone;
    }
    return result;
}

[[nodiscard]] const container::Array<game::W3dWeaponBarrelTable, 3>*
resolveWeaponBarrelTables(
    const WeaponPresentationSource& source,
    container::StringView archetypeName,
    size_t poseRuleIndex,
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions) {
    const game::W3dPristineBoneCatalog* catalog =
        source.content.pristineBoneCatalog();
    if (!catalog || !catalog->isLoaded() || archetypeName.empty()) {
        return nullptr;
    }

    struct BarrelTableCache final {
        const void* owner = nullptr;
        const game::W3dPristineBoneCatalog* catalog = nullptr;
        uint64_t presentationEpoch = 0;
        uint64_t sourceFingerprint = 0;
        container::HashMap<
            uintptr_t,
            container::UniquePtr<
                container::Array<game::W3dWeaponBarrelTable, 3>>>
            entries;
    };
    thread_local BarrelTableCache cache;
    const uint64_t presentationEpoch = source.presentationEpoch;
    const uint64_t sourceFingerprint = catalog->sourceFingerprint();
    if (cache.owner != source.cacheOwner || cache.catalog != catalog ||
        cache.presentationEpoch != presentationEpoch ||
        cache.sourceFingerprint != sourceFingerprint) {
        cache = {
            .owner = source.cacheOwner,
            .catalog = catalog,
            .presentationEpoch = presentationEpoch,
            .sourceFingerprint = sourceFingerprint,
        };
    }

    // Model rule storage is immutable for a presentation epoch. Its weapon
    // definition array therefore forms a stable intern key without rebuilding
    // four strings or rescanning up to 99 numbered bones for every object.
    const uintptr_t key = reinterpret_cast<uintptr_t>(&definitions);
    if (const auto found = cache.entries.find(key);
        found != cache.entries.end()) {
        return found->second.get();
    }

    auto result = std::make_unique<
        container::Array<game::W3dWeaponBarrelTable, 3>>();
    for (size_t slot = 0; slot < definitions.size(); ++slot) {
        const game::ModelWeaponBoneDefinition& bones = definitions[slot];
        (*result)[slot] = catalog->resolveWeaponBarrels(
            archetypeName, poseRuleIndex, bones.fireFxBone,
            bones.recoilBone, bones.muzzleFlash, bones.launchBone);
    }
    auto [stored, inserted] = cache.entries.emplace(key, std::move(result));
    static_cast<void>(inserted);
    return stored->second.get();
}

void appendWeaponPresentationControls(
    const WeaponPresentationSource& source,
    const ObjectWeaponComponent* weapons,
    const RenderModelComponent& visual,
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions,
    const container::Array<game::W3dWeaponBarrelTable, 3>* barrelTables,
    uint8_t projectileBoneFeedbackEnabledSlots,
    const game::ModelWeaponRecoilProfile& recoil,
    uint64_t simulationFrame,
    container::Vector<render::RenderSubObjectVisibility>& visibility,
    container::Vector<render::RenderWeaponImpulse>& weaponImpulses) {
    const ObjectWeaponSetRuntime* activeSet = nullptr;
    if (weapons && weapons->activeWeaponSetIndex &&
        *weapons->activeWeaponSetIndex < weapons->sets.size()) {
        activeSet = &weapons->sets[*weapons->activeWeaponSetIndex];
    }
    for (size_t slot = 0; slot < definitions.size(); ++slot) {
        const game::ModelWeaponBoneDefinition& definition = definitions[slot];
        const bool fired = visual.lastWeaponFireSequences[slot] != 0 &&
            simulationFrame >= visual.lastWeaponFireTicks[slot];
        const game::W3dWeaponBarrelEntry* exactBarrel = nullptr;
        if (barrelTables && !(*barrelTables)[slot].barrels.empty()) {
            const uint32_t sequence = std::max<uint32_t>(
                1u, visual.lastWeaponFireSequences[slot]);
            const size_t barrelIndex =
                sequence - 1u < (*barrelTables)[slot].barrels.size()
                ? static_cast<size_t>(sequence - 1u) : 0u;
            exactBarrel = &(*barrelTables)[slot].barrels[
                barrelIndex];
        }
        const bool hasExactRecoilBone = exactBarrel &&
            !exactBarrel->recoilBone.empty();
        const container::StringView recoilBone = hasExactRecoilBone
            ? container::StringView{exactBarrel->recoilBone}
            : container::StringView{definition.recoilBone};
        const bool hasExactMuzzleFlash = exactBarrel &&
            !exactBarrel->muzzleFlash.empty();
        const container::StringView muzzleFlash = hasExactMuzzleFlash
            ? container::StringView{exactBarrel->muzzleFlash}
            : container::StringView{definition.muzzleFlash};
        if (fired && (!recoilBone.empty() || !muzzleFlash.empty())) {
            weaponImpulses.push_back({
                .recoilBone = container::String(recoilBone),
                .muzzleFlash = container::String(muzzleFlash),
                .fireTick = visual.lastWeaponFireTicks[slot],
                .sequenceOrdinal = visual.lastWeaponFireSequences[slot],
                .initialSpeed = recoil.initialSpeed,
                .damping = recoil.damping,
                .maximumDistance = recoil.maximumDistance,
                .settleSpeed = recoil.settleSpeed,
                .recoilBoneIsPrefix = !hasExactRecoilBone,
                .muzzleFlashIsPrefix = !hasExactMuzzleFlash,
            });
        }
        // Muzzle meshes are authored visible in several shipped infantry W3Ds.
        // Hide the complete numbered family every frame, then reveal only the
        // selected barrel during the confirmed fire frame. A single selected
        // hide leaves every other MUZZLEFX member at its authored default.
        if (!definition.muzzleFlash.empty()) {
            visibility.push_back({
                .name = definition.muzzleFlash,
                .visible = false,
                .nameIsPrefix = true,
                .nameSequenceOrdinal = 0,
                .namePrefixFallsBackToBare = true,
            });
        } else if (barrelTables) {
            for (const game::W3dWeaponBarrelEntry& barrel :
                 (*barrelTables)[slot].barrels) {
                if (barrel.muzzleFlash.empty()) continue;
                visibility.push_back({
                    .name = barrel.muzzleFlash,
                    .visible = false,
                });
            }
        }
        if ((projectileBoneFeedbackEnabledSlots & (1u << slot)) != 0u &&
            activeSet) {
            const ObjectWeaponSlotRuntime& runtime = activeSet->slots[slot];
            const game::WeaponTemplate* authored =
                source.content.findWeapon(runtime.content);
            if (authored && authored->clipSize > 0) {
                const uint32_t maximum =
                    static_cast<uint32_t>(authored->clipSize);
                const uint32_t remaining = std::min(runtime.ammoInClip, maximum);
                const uint32_t hiddenFromFront = maximum - remaining;
                if (!definition.hideShowBone.empty()) {
                    visibility.push_back({
                        .name = definition.hideShowBone,
                        .visible = hiddenFromFront == 0,
                    });
                } else if (!definition.launchBone.empty()) {
                    for (uint32_t projectile = 0; projectile < maximum;
                         ++projectile) {
                        container::String subObject = definition.launchBone;
                        const uint32_t ordinal = projectile + 1u;
                        if (ordinal < 10u) subObject.push_back('0');
                        subObject += std::to_string(ordinal);
                        visibility.push_back({
                            .name = std::move(subObject),
                            .visible = projectile >= hiddenFromFront,
                        });
                    }
                }
            }
        }
    }
}



} // namespace engine::render_extraction_detail
