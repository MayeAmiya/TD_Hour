#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/combat/ObjectCombatDetail.h"

namespace engine::object_combat_detail {

[[nodiscard]] std::optional<PristineWeaponPresentation>
pristineWeaponPresentation(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, game::WeaponSlot slot,
    const game::ModelConditionMask* presentationConditions) {
    const game::W3dPristineBoneCatalog* catalog =
        content.pristineBoneCatalog();
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, sourceEntity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, sourceEntity);
    const size_t slotIndex = static_cast<size_t>(slot);
    if (!catalog || !catalog->isLoaded() || !source ||
        !source->archetype || !visual ||
        slotIndex >= game::kWeaponSlotCount) {
        return std::nullopt;
    }

    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const game::ModelConditionMask& selectedConditions =
        presentationConditions ? *presentationConditions
                               : visual->modelConditionFlags;
    size_t flattenedVisualOffset = 0;
    const auto attachOffsetForChannel = [&] (
            size_t selectedChannel,
            container::StringView attachBone) -> LogicFixedVec3 {
        if (attachBone.empty()) return {};
        size_t otherOffset = 0;
        for (size_t other = 0;
             other < templateData.drawVisualChannels.size(); ++other) {
            const game::ModelDrawVisualChannel& otherChannel =
                templateData.drawVisualChannels[other];
            const size_t otherRule =
                game::selectModelConditionVisualRuleIndex(
                    otherChannel, selectedConditions);
            if (other != selectedChannel &&
                otherRule < otherChannel.conditionVisuals.size()) {
                if (const auto pose = catalog->find(
                        source->archetype->name,
                        otherOffset + otherRule, attachBone)) {
                    return {
                        .x = pose->translation.x,
                        .y = pose->translation.y,
                        .z = pose->translation.z,
                    };
                }
            }
            otherOffset += otherChannel.conditionVisuals.size();
        }
        return {};
    };
    const auto selectFromRule = [&] (
            size_t flattenedRuleIndex, size_t channelIndex,
            const game::ModelConditionVisualRule& rule,
            container::StringView attachBone)
        -> std::optional<PristineWeaponPresentation> {
        game::W3dWeaponBarrelTable table =
            catalog->resolveWeaponBarrels(
                source->archetype->name, flattenedRuleIndex,
                rule.weaponBones[slotIndex].fireFxBone,
                rule.weaponBones[slotIndex].recoilBone,
                rule.weaponBones[slotIndex].muzzleFlash,
                rule.weaponBones[slotIndex].launchBone);
        if (table.barrels.empty()) return std::nullopt;
        PristineWeaponPresentation result{
            .barrels = std::move(table),
            .turrets = rule.turrets,
            .attachOffset = attachOffsetForChannel(
                channelIndex, attachBone),
        };
        for (size_t turret = 0; turret < result.turrets.size(); ++turret) {
            const game::ModelTurretBoneDefinition& definition =
                result.turrets[turret];
            if (!definition.yawBone.empty()) {
                result.yawPivots[turret] = catalog->find(
                    source->archetype->name, flattenedRuleIndex,
                    definition.yawBone);
            }
            if (!definition.pitchBone.empty()) {
                result.pitchPivots[turret] = catalog->find(
                    source->archetype->name, flattenedRuleIndex,
                    definition.pitchBone);
            }
        }
        return result;
    };

    for (size_t channelIndex = 0;
         channelIndex < templateData.drawVisualChannels.size();
         ++channelIndex) {
        const game::ModelDrawVisualChannel& channel =
            templateData.drawVisualChannels[channelIndex];
        const size_t ruleIndex =
            game::selectModelConditionVisualRuleIndex(
                channel, selectedConditions);
        if (ruleIndex < channel.conditionVisuals.size()) {
            if (auto resolved = selectFromRule(
                    flattenedVisualOffset + ruleIndex, channelIndex,
                    channel.conditionVisuals[ruleIndex],
                    channel.attachToBoneInAnotherModule)) {
                return resolved;
            }
        }
        flattenedVisualOffset += channel.conditionVisuals.size();
    }
    if (templateData.drawVisualChannels.empty()) {
        const size_t ruleIndex = game::selectModelConditionVisualRuleIndex(
            templateData, selectedConditions);
        if (ruleIndex < templateData.modelConditionVisuals.size()) {
            if (auto resolved = selectFromRule(
                    ruleIndex, 0,
                    templateData.modelConditionVisuals[ruleIndex], {})) {
                return resolved;
            }
        }
    }
    return std::nullopt;
}

std::optional<game::ModelConditionMask> firingPresentationConditions(
    const ecs::registry& registry, ecs::entity sourceEntity,
    game::WeaponSlot slot) noexcept {
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, sourceEntity);
    if (!visual || static_cast<size_t>(slot) >= game::kWeaponSlotCount) {
        return std::nullopt;
    }
    return game::weaponFiringModelConditions(
        visual->modelConditionFlags, static_cast<uint32_t>(slot));
}

[[nodiscard]] LogicFixedVec3 rotateZ(
    const LogicFixedVec3& point, const LogicFixedVec3& pivot,
    math::q32_32 radians) {
    const math::q32_32_sincos angle = math::fixed_sincos(radians);
    const math::q32_32 x = point.x - pivot.x;
    const math::q32_32 y = point.y - pivot.y;
    return {
        .x = pivot.x + x * angle.cosine - y * angle.sine,
        .y = pivot.y + x * angle.sine + y * angle.cosine,
        .z = point.z,
    };
}

[[nodiscard]] LogicFixedVec3 rotateY(
    const LogicFixedVec3& point, const LogicFixedVec3& pivot,
    math::q32_32 radians) {
    const math::q32_32_sincos angle = math::fixed_sincos(radians);
    const math::q32_32 x = point.x - pivot.x;
    const math::q32_32 z = point.z - pivot.z;
    return {
        .x = pivot.x + x * angle.cosine + z * angle.sine,
        .y = point.y,
        .z = pivot.z - x * angle.sine + z * angle.cosine,
    };
}

struct ContainmentFirePointPresentation final {
    LogicFixedVec3 local{};
    LogicFixedVec3 attachOffset{};
    game::ModelTurretBoneDefinition mainTurret;
    std::optional<data::w3d::FixedRigidTransform> yawPivot;
    std::optional<data::w3d::FixedRigidTransform> pitchPivot;
};

// Resolve the same flattened Draw channel/rule selected by the renderer, but
// keep the result in the immutable pristine-pose catalog. Containment combat
// must not ask a Drawable for an animated matrix or make gameplay depend on
// whether a render object currently exists.
[[nodiscard]] std::optional<ContainmentFirePointPresentation>
pristineContainmentFirePoint(
    const ecs::registry& registry, ecs::entity hostEntity,
    const GameContentSnapshot& content, container::StringView boneName) {
    const game::W3dPristineBoneCatalog* catalog =
        content.pristineBoneCatalog();
    const ThingTemplateComponent* host =
        ecs::try_get<ThingTemplateComponent>(registry, hostEntity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, hostEntity);
    if (!catalog || !catalog->isLoaded() || !host || !host->archetype ||
        !visual || boneName.empty()) {
        return std::nullopt;
    }

    const game::ThingTemplate& templateData =
        host->archetype->templateData;
    const auto attachOffsetForChannel = [&] (
            size_t selectedChannel,
            container::StringView attachBone) -> LogicFixedVec3 {
        if (attachBone.empty()) return {};
        size_t otherOffset = 0;
        for (size_t other = 0;
             other < templateData.drawVisualChannels.size(); ++other) {
            const game::ModelDrawVisualChannel& otherChannel =
                templateData.drawVisualChannels[other];
            const size_t otherRule =
                game::selectModelConditionVisualRuleIndex(
                    otherChannel, visual->modelConditionFlags);
            if (other != selectedChannel &&
                otherRule < otherChannel.conditionVisuals.size()) {
                if (const auto pose = catalog->find(
                        host->archetype->name, otherOffset + otherRule,
                        attachBone)) {
                    return {
                        .x = pose->translation.x,
                        .y = pose->translation.y,
                        .z = pose->translation.z,
                    };
                }
            }
            otherOffset += otherChannel.conditionVisuals.size();
        }
        return {};
    };
    const auto selectFromRule = [&] (
            size_t flattenedRuleIndex, size_t channelIndex,
            const game::ModelConditionVisualRule& rule,
            container::StringView attachBone)
        -> std::optional<ContainmentFirePointPresentation> {
        const auto firePoint = catalog->find(
            host->archetype->name, flattenedRuleIndex, boneName);
        if (!firePoint) return std::nullopt;
        ContainmentFirePointPresentation result{
            .local = {
                .x = firePoint->translation.x,
                .y = firePoint->translation.y,
                .z = firePoint->translation.z,
            },
            .attachOffset = attachOffsetForChannel(
                channelIndex, attachBone),
            .mainTurret = rule.turrets[0],
        };
        if (!result.mainTurret.yawBone.empty()) {
            result.yawPivot = catalog->find(
                host->archetype->name, flattenedRuleIndex,
                result.mainTurret.yawBone);
        }
        if (!result.mainTurret.pitchBone.empty()) {
            result.pitchPivot = catalog->find(
                host->archetype->name, flattenedRuleIndex,
                result.mainTurret.pitchBone);
        }
        return result;
    };

    size_t flattenedVisualOffset = 0;
    for (size_t channelIndex = 0;
         channelIndex < templateData.drawVisualChannels.size();
         ++channelIndex) {
        const game::ModelDrawVisualChannel& channel =
            templateData.drawVisualChannels[channelIndex];
        const size_t ruleIndex =
            game::selectModelConditionVisualRuleIndex(
                channel, visual->modelConditionFlags);
        if (ruleIndex < channel.conditionVisuals.size()) {
            if (auto resolved = selectFromRule(
                    flattenedVisualOffset + ruleIndex, channelIndex,
                    channel.conditionVisuals[ruleIndex],
                    channel.attachToBoneInAnotherModule)) {
                return resolved;
            }
        }
        flattenedVisualOffset += channel.conditionVisuals.size();
    }
    if (templateData.drawVisualChannels.empty()) {
        const size_t ruleIndex =
            game::selectModelConditionVisualRuleIndex(
                templateData, visual->modelConditionFlags);
        if (ruleIndex < templateData.modelConditionVisuals.size()) {
            return selectFromRule(
                ruleIndex, 0, templateData.modelConditionVisuals[ruleIndex],
                {});
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<LogicFixedVec3>
containmentFirePointWorldPosition(
    const ecs::registry& registry, ecs::entity hostEntity,
    const GameContentSnapshot& content, container::StringView boneName,
    bool passengersInTurret, const LogicFixedVec3& hostPosition,
    const TransformComponent& hostTransform) {
    const auto presentation = pristineContainmentFirePoint(
        registry, hostEntity, content, boneName);
    if (!presentation) return std::nullopt;

    LogicFixedVec3 local = presentation->local;
    if (passengersInTurret) {
        // getSingleLogicalBonePositionOnTurret first removes the authored art
        // rest angles, then applies the current main-turret yaw/pitch around
        // its real pivots. This is the same fixed-point transform order used
        // by weapon launch bones above.
        local = rotateZ(
            local, {}, presentation->mainTurret.artYawRadiansFixed);
        local = rotateY(
            local, {}, -presentation->mainTurret.artPitchRadiansFixed);
    }
    local.x += presentation->attachOffset.x;
    local.y += presentation->attachOffset.y;
    local.z += presentation->attachOffset.z;
    if (passengersInTurret) {
        if (const ObjectWeaponComponent* hostWeapons =
                ecs::try_get<ObjectWeaponComponent>(registry, hostEntity)) {
            const ObjectTurretRuntime& turret = hostWeapons->turrets[0];
            LogicFixedVec3 pitchPivot = presentation->attachOffset;
            if (presentation->pitchPivot) {
                pitchPivot.x += presentation->pitchPivot->translation.x;
                pitchPivot.y += presentation->pitchPivot->translation.y;
                pitchPivot.z += presentation->pitchPivot->translation.z;
            }
            LogicFixedVec3 yawPivot = presentation->attachOffset;
            if (presentation->yawPivot) {
                yawPivot.x += presentation->yawPivot->translation.x;
                yawPivot.y += presentation->yawPivot->translation.y;
                yawPivot.z += presentation->yawPivot->translation.z;
            }
            local = rotateY(local, pitchPivot, -turret.pitchRadians);
            local = rotateZ(local, yawPivot, turret.yawRadians);
        }
    }

    const math::q32_32 yaw = readAuthoritativeObjectYaw(
        registry, hostEntity, hostTransform);
    const math::q32_32_sincos angle = math::fixed_sincos(yaw);
    return LogicFixedVec3{
        .x = hostPosition.x +
            local.x * angle.cosine - local.y * angle.sine,
        .y = hostPosition.y +
            local.x * angle.sine + local.y * angle.cosine,
        .z = hostPosition.z + local.z,
    };
}

void releaseGarrisonFirePoint(
    ecs::registry& registry, ecs::entity hostEntity, ObjectId occupant) {
    ObjectGarrisonFirePointComponent* state =
        ecs::try_get<ObjectGarrisonFirePointComponent>(registry, hostEntity);
    if (!state || !occupant) return;
    const auto found = std::lower_bound(
        state->assignments.begin(), state->assignments.end(), occupant,
        [](const ObjectGarrisonFirePointAssignment& assignment, ObjectId id) {
            return assignment.occupant < id;
        });
    if (found == state->assignments.end() || found->occupant != occupant)
        return;
    state->assignments.erase(found);
    ++state->revision;
    markObjectDirty(
        registry, hostEntity, ObjectDirtyDomain::RenderExtraction);
}

[[nodiscard]] std::optional<size_t> assignGarrisonFirePoint(
    ObjectGarrisonFirePointComponent& state, ObjectId occupant,
    ObjectId target, const LogicFixedVec3& targetPosition,
    const container::Vector<LogicFixedVec3>& points) {
    if (!occupant || points.empty()) return std::nullopt;

    const auto findAssignment = std::lower_bound(
        state.assignments.begin(), state.assignments.end(), occupant,
        [](const ObjectGarrisonFirePointAssignment& assignment, ObjectId id) {
            return assignment.occupant < id;
        });
    std::optional<size_t> existing;
    if (findAssignment != state.assignments.end() &&
        findAssignment->occupant == occupant) {
        existing = static_cast<size_t>(findAssignment - state.assignments.begin());
        if (findAssignment->pointIndex >= points.size()) {
            state.assignments.erase(findAssignment);
            ++state.revision;
            existing.reset();
        }
    }

    const auto distanceSquared = [&](size_t point) {
        const LogicFixedVec3 delta{
            .x = points[point].x - targetPosition.x,
            .y = points[point].y - targetPosition.y,
            .z = points[point].z - targetPosition.z,
        };
        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    };
    const auto pointOccupied = [&](size_t point, ObjectId except) {
        return std::any_of(
            state.assignments.begin(), state.assignments.end(),
            [&](const ObjectGarrisonFirePointAssignment& assignment) {
                return assignment.occupant != except &&
                    assignment.pointIndex == point;
            });
    };
    const auto nearestFreePoint = [&]() -> std::optional<size_t> {
        std::optional<size_t> nearest;
        math::q32_32 nearestDistance{};
        for (size_t point = 0; point < points.size(); ++point) {
            if (pointOccupied(point, occupant)) continue;
            const math::q32_32 distance = distanceSquared(point);
            if (!nearest || distance < nearestDistance ||
                (distance == nearestDistance && point < *nearest)) {
                nearest = point;
                nearestDistance = distance;
            }
        }
        return nearest;
    };

    if (existing) {
        ObjectGarrisonFirePointAssignment& assignment =
            state.assignments[*existing];
        const bool targetChanged = assignment.target != target ||
            assignment.targetPosition.x != targetPosition.x ||
            assignment.targetPosition.y != targetPosition.y ||
            assignment.targetPosition.z != targetPosition.z;
        const size_t currentPoint = assignment.pointIndex;
        assignment.target = target;
        assignment.targetPosition = targetPosition;
        // GarrisonContain::trackTargets keeps the current point unless a
        // currently-free point is strictly closer. A target change therefore
        // naturally re-evaluates the free list without stealing another
        // occupant's point; ties remain stable by point index.
        const std::optional<size_t> nearest = nearestFreePoint();
        if (nearest &&
            distanceSquared(*nearest) < distanceSquared(currentPoint)) {
            assignment.pointIndex = static_cast<uint16_t>(*nearest);
        }
        if (targetChanged || assignment.pointIndex != currentPoint)
            ++state.revision;
        return static_cast<size_t>(assignment.pointIndex);
    }

    const std::optional<size_t> nearest = nearestFreePoint();
    if (!nearest) return std::nullopt;
    const auto insertAt = std::lower_bound(
        state.assignments.begin(), state.assignments.end(), occupant,
        [](const ObjectGarrisonFirePointAssignment& assignment, ObjectId id) {
            return assignment.occupant < id;
        });
    state.assignments.insert(insertAt, {
        .occupant = occupant,
        .target = target,
        .targetPosition = targetPosition,
        .pointIndex = static_cast<uint16_t>(*nearest),
    });
    ++state.revision;
    return nearest;
}

[[nodiscard]] LogicFixedQuaternion normalizedQuaternion(
    LogicFixedQuaternion value) noexcept {
    const math::q32_32 lengthSquared =
        value.x * value.x + value.y * value.y + value.z * value.z +
        value.w * value.w;
    if (lengthSquared <= math::q32_32{}) return {};
    const math::q32_32 length = math::q32_32::sqrt(lengthSquared);
    if (length <= math::q32_32{}) return {};
    value.x /= length;
    value.y /= length;
    value.z /= length;
    value.w /= length;
    return value;
}

[[nodiscard]] LogicFixedQuaternion multiplyQuaternion(
    const LogicFixedQuaternion& left,
    const LogicFixedQuaternion& right) noexcept {
    return normalizedQuaternion({
        .x = left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        .y = left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        .z = left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        .w = left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    });
}

[[nodiscard]] LogicFixedQuaternion rotationZ(
    math::q32_32 radians) noexcept {
    const math::q32_32_sincos angle = math::fixed_sincos(
        radians / math::q32_32{int32_t{2}});
    return {.z = angle.sine, .w = angle.cosine};
}

[[nodiscard]] LogicFixedQuaternion rotationY(
    math::q32_32 radians) noexcept {
    const math::q32_32_sincos angle = math::fixed_sincos(
        radians / math::q32_32{int32_t{2}});
    return {.y = angle.sine, .w = angle.cosine};
}

[[nodiscard]] WeaponLaunchTransform pristineWeaponLaunchTransform(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, game::WeaponSlot slot,
    uint32_t barrelSequenceOrdinal,
    const LogicFixedVec3& fallback,
    const game::ModelConditionMask* presentationConditions) {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, sourceEntity);
    const auto presentation = pristineWeaponPresentation(
        registry, sourceEntity, content, slot, presentationConditions);
    WeaponLaunchTransform result{.position = fallback};
    if (transform) {
        result.orientation = rotationZ(readAuthoritativeObjectYaw(
            registry, sourceEntity, *transform));
        result.hasOrientation = true;
    }
    if (!transform || !presentation || presentation->barrels.barrels.empty()) {
        return result;
    }
    const uint32_t sequence = std::max<uint32_t>(1u, barrelSequenceOrdinal);
    const size_t barrelIndex =
        sequence - 1u < presentation->barrels.barrels.size()
        ? static_cast<size_t>(sequence - 1u) : 0u;
    const game::W3dWeaponBarrelEntry& barrel =
        presentation->barrels.barrels[barrelIndex];
    if (!barrel.hasLaunchBone) return result;
    LogicFixedVec3 local{
        .x = barrel.launchLocal.translation.x,
        .y = barrel.launchLocal.translation.y,
        .z = barrel.launchLocal.translation.z,
    };
    LogicFixedQuaternion localOrientation{
        .x = barrel.launchLocal.rotation.x,
        .y = barrel.launchLocal.rotation.y,
        .z = barrel.launchLocal.rotation.z,
        .w = barrel.launchLocal.rotation.w,
    };
    std::optional<size_t> turretIndex;
    if (weapons) {
        const uint8_t slotBit = static_cast<uint8_t>(
            1u << static_cast<size_t>(slot));
        for (size_t index = 0; index < weapons->turrets.size(); ++index) {
            if ((weapons->turrets[index].controlledWeaponSlots & slotBit) != 0) {
                turretIndex = index;
                break;
            }
        }
    }
    if (turretIndex && *turretIndex < presentation->turrets.size()) {
        const game::ModelTurretBoneDefinition& authored =
            presentation->turrets[*turretIndex];
        local = rotateZ(local, {}, authored.artYawRadiansFixed);
        localOrientation = multiplyQuaternion(
            rotationZ(authored.artYawRadiansFixed),
            localOrientation);
        local = rotateY(local, {}, -authored.artPitchRadiansFixed);
        localOrientation = multiplyQuaternion(
            rotationY(-authored.artPitchRadiansFixed),
            localOrientation);
    }
    local.x += presentation->attachOffset.x;
    local.y += presentation->attachOffset.y;
    local.z += presentation->attachOffset.z;
    if (turretIndex && weapons) {
        const ObjectTurretRuntime& turret = weapons->turrets[*turretIndex];
        LogicFixedVec3 pitchPivot = presentation->attachOffset;
        if (const auto& pose = presentation->pitchPivots[*turretIndex]) {
            pitchPivot.x += pose->translation.x;
            pitchPivot.y += pose->translation.y;
            pitchPivot.z += pose->translation.z;
        }
        LogicFixedVec3 yawPivot = presentation->attachOffset;
        if (const auto& pose = presentation->yawPivots[*turretIndex]) {
            yawPivot.x += pose->translation.x;
            yawPivot.y += pose->translation.y;
            yawPivot.z += pose->translation.z;
        }
        local = rotateY(local, pitchPivot, -turret.pitchRadians);
        localOrientation = multiplyQuaternion(
            rotationY(-turret.pitchRadians), localOrientation);
        local = rotateZ(local, yawPivot, turret.yawRadians);
        localOrientation = multiplyQuaternion(
            rotationZ(turret.yawRadians), localOrientation);
    }
    const math::q32_32 objectYaw = readAuthoritativeObjectYaw(
        registry, sourceEntity, *transform);
    const math::q32_32_sincos yaw = math::fixed_sincos(objectYaw);
    result.position = LogicFixedVec3{
        .x = fallback.x + local.x * yaw.cosine - local.y * yaw.sine,
        .y = fallback.y + local.x * yaw.sine + local.y * yaw.cosine,
        .z = fallback.z + local.z,
    };
    result.orientation = multiplyQuaternion(
        rotationZ(objectYaw), localOrientation);
    result.hasOrientation = true;
    return result;
}

} // namespace engine::object_combat_detail
