#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace engine {

namespace {
using Fixed = math::q32_32;
} // namespace

std::optional<ObjectBridgeScaffoldSpawnPlan>
buildObjectBridgeScaffoldSpawnPlan(
    const ObjectBridgeScaffoldLayoutRequest& request) {
    constexpr size_t kMaximumTopScaffolds = 16384;
    constexpr size_t kMaximumSupportLayersPerTop = 4096;
    constexpr size_t kMaximumSpawnObjects = 65536;
    constexpr Fixed kLegacyMovementFudge = Fixed::from_fraction(1, 10);
    if (!request.bridge || request.scaffoldTemplateName.empty() ||
        request.scaffoldSupportTemplateName.empty() ||
        request.requestSequence == 0 ||
        request.scaffoldSpacing <= Fixed{} ||
        request.scaffoldHeight <= Fixed{} ||
        request.scaffoldSupportHeight <= Fixed{} ||
        request.lateralSpeedPerFrame < Fixed{} ||
        request.verticalSpeedPerFrame < Fixed{}) {
        return std::nullopt;
    }

    const Fixed dx = request.toPosition.x - request.fromPosition.x;
    const Fixed dy = request.toPosition.y - request.fromPosition.y;
    const Fixed dz = request.toPosition.z - request.fromPosition.z;
    const Fixed tileDistance = Fixed::sqrt(dx * dx + dy * dy + dz * dz);
    if (tileDistance <= Fixed{}) return std::nullopt;

    const int64_t distanceRaw = tileDistance.raw();
    const int64_t spacingRaw = request.scaffoldSpacing.raw();
    const uint64_t quotient = static_cast<uint64_t>(distanceRaw / spacingRaw);
    const uint64_t remainder = static_cast<uint64_t>(distanceRaw % spacingRaw);
    const uint64_t topCount64 = quotient + (remainder != 0 ? 1u : 0u) + 1u;
    if (topCount64 == 0 || topCount64 > kMaximumTopScaffolds) {
        return std::nullopt;
    }
    const size_t topCount = static_cast<size_t>(topCount64);
    const size_t iterations = (topCount + 1u) / 2u;
    const LogicFixedVec3 leftStep{
        dx / tileDistance,
        dy / tileDistance,
        dz / tileDistance,
    };
    const LogicFixedVec3 rightStep{-leftStep.x, -leftStep.y, -leftStep.z};
    const Fixed orientation = math::fixed_atan2(dy, dx);

    ObjectBridgeScaffoldSpawnPlan plan;
    size_t topCreated = 0;
    const auto appendColumn = [&](const LogicFixedVec3& risePosition,
                                  const LogicFixedVec3& direction,
                                  size_t index) -> bool {
        const Fixed distance = request.scaffoldSpacing *
            Fixed{static_cast<int32_t>(index)};
        LogicFixedVec3 buildPosition{
            risePosition.x + direction.x * distance + kLegacyMovementFudge,
            risePosition.y + direction.y * distance,
            risePosition.z + direction.z * distance,
        };
        if (plan.objects.size() >= kMaximumSpawnObjects) return false;
        plan.objects.push_back(detail::makeScaffoldSpec(
            request, request.scaffoldTemplateName, risePosition,
            buildPosition, request.bridgeCenter, request.scaffoldHeight,
            orientation));

        // RefCode creates one support below a bridge endpoint at Z==0, then
        // continues downward in exact template-height layers while the
        // pre-decrement offset remains non-negative.
        Fixed offset = risePosition.z;
        LogicFixedVec3 supportRise = risePosition;
        LogicFixedVec3 supportBuild = buildPosition;
        LogicFixedVec3 supportCenter = request.bridgeCenter;
        size_t supportCount = 0;
        while (offset >= Fixed{}) {
            if (++supportCount > kMaximumSupportLayersPerTop) return false;
            if (plan.objects.size() >= kMaximumSpawnObjects) return false;
            supportRise.z -= request.scaffoldSupportHeight;
            supportBuild.z -= request.scaffoldSupportHeight;
            supportCenter.z -= request.scaffoldSupportHeight;
            plan.objects.push_back(detail::makeScaffoldSpec(
                request, request.scaffoldSupportTemplateName, supportRise,
                supportBuild, supportCenter,
                request.scaffoldSupportHeight, orientation));
            offset -= request.scaffoldSupportHeight;
        }
        return true;
    };

    for (size_t index = 0; index < iterations; ++index) {
        if (!appendColumn(request.fromPosition, leftStep, index)) {
            return std::nullopt;
        }
        ++topCreated;
        if (topCreated < topCount) {
            if (!appendColumn(request.toPosition, rightStep, index)) {
                return std::nullopt;
            }
            ++topCreated;
        }
    }
    return plan;
}

bool ObjectBridgeSystem::applyScaffoldMotionRequest(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBridgeScaffoldMotionRequest& request) const {
    if (!request.scaffold || request.sequence == 0 ||
        lifecycle.isPendingDestroy(request.scaffold)) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(request.scaffold);
    if (!entity) return false;
    ObjectBridgeScaffoldComponent* scaffold =
        ecs::try_get<ObjectBridgeScaffoldComponent>(registry, *entity);
    if (!scaffold || request.sequence <= scaffold->acceptedRequestSequence) {
        return false;
    }

    if (request.kind == ObjectBridgeScaffoldRequestKind::CreateAndBuild) {
        if (scaffold->configured || !request.bridge ||
            !lifecycle.entityFromId(request.bridge) ||
            request.lateralSpeedPerFrame < Fixed{} ||
            request.verticalSpeedPerFrame < Fixed{}) {
            return false;
        }
        scaffold->bridge = request.bridge;
        scaffold->createPosition = request.createPosition;
        scaffold->risePosition = request.risePosition;
        scaffold->buildPosition = request.buildPosition;
        scaffold->position = request.createPosition;
        scaffold->lateralSpeedPerFrame = request.lateralSpeedPerFrame;
        scaffold->verticalSpeedPerFrame = request.verticalSpeedPerFrame;
        scaffold->motion = ObjectBridgeScaffoldMotion::Rise;
        scaffold->configured = true;
        scaffold->destroyRequested = false;
        writeAuthoritativeObjectPosition(registry, *entity, scaffold->position);
        writeAuthoritativeObjectYaw(
            registry, *entity, request.orientationRadians);
    } else {
        if (!scaffold->configured || scaffold->destroyRequested) return false;
        detail::reverseScaffoldMotion(*scaffold);
    }
    scaffold->acceptedRequestSequence = request.sequence;
    ++scaffold->revision;
    return true;
}

} // namespace engine
