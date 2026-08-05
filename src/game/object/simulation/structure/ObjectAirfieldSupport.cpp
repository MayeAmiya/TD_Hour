#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
#include "core/container/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/base/SimulationRandom.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
namespace airfield_detail
{

[[nodiscard]] uint64_t millisecondsToFrames(uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept
{
    if (milliseconds == 0)
        return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    if (left > std::numeric_limits<uint64_t>::max() - right)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] uint64_t chinookDelayToFrames(
    uint32_t authoredMillisecondsOrLegacySentinel,
    uint32_t logicFramesPerSecond) noexcept
{
    // ChinookAI's constructor default is already a logic-frame sentinel;
    // explicit INI durations are authored milliseconds in the modern frozen
    // plan. Treating 0x7fffffff as milliseconds would multiply the legacy
    // "effectively never" deadline by the session FPS a second time.
    if (authoredMillisecondsOrLegacySentinel == 0x7fffffffu)
        return 0x7fffffffu;
    return millisecondsToFrames(authoredMillisecondsOrLegacySentinel,
                                logicFramesPerSecond);
}

[[nodiscard]] uint64_t chinookRopeIdentity(ObjectId object,
                                           uint32_t authoredOrder,
                                           uint32_t generation,
                                           uint32_t ropeIndex) noexcept
{
    uint64_t value = static_cast<uint64_t>(object.value) |
        (static_cast<uint64_t>(authoredOrder) << 32u);
    value ^= (static_cast<uint64_t>(generation) << 17u) |
        static_cast<uint64_t>(ropeIndex + 1u);
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return value != 0 ? value : 1u;
}

[[nodiscard]] uint64_t aircraftSlowDeathBladeDelayFrames(
    ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick,
    uint32_t minimumMilliseconds, uint32_t maximumMilliseconds,
    uint32_t logicFramesPerSecond) noexcept
{
    // RefCode HelicopterSlowDeathBehavior::beginSlowDeath() draws
    // GameLogicRandomValueReal(MinBladeFlyOffDelay, MaxBladeFlyOffDelay). This
    // port cannot consume the shared gameplay stream at the same point in the
    // same order (death reactions are resolved through a sorted journal rather
    // than in module-visit order), so the draw becomes the same per-identity
    // splitmix projection used by chinookRopeIdentity above: no draw is taken
    // from the shared stream and every peer samples the identical value.
    const uint32_t minimum =
        std::min(minimumMilliseconds, maximumMilliseconds);
    const uint32_t maximum =
        std::max(minimumMilliseconds, maximumMilliseconds);
    const uint64_t minimumFrames =
        millisecondsToFrames(minimum, logicFramesPerSecond);
    const uint64_t maximumFrames =
        millisecondsToFrames(maximum, logicFramesPerSecond);
    if (maximumFrames <= minimumFrames)
        return maximumFrames;
    uint64_t value = static_cast<uint64_t>(object.value) |
        (static_cast<uint64_t>(authoredOrder) << 32u);
    value ^= confirmedTick + 0x9e3779b97f4a7c15ull +
        (value << 6u) + (value >> 2u);
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return minimumFrames + value % (maximumFrames - minimumFrames + 1u);
}

[[nodiscard]] uint64_t randomChinookDelayFrames(
    SimulationRandom& random, uint32_t minimumMilliseconds,
    uint32_t maximumMilliseconds, uint32_t logicFramesPerSecond,
    bool firstDrop) noexcept
{
    const uint32_t minimum =
        std::min(minimumMilliseconds, maximumMilliseconds);
    const uint32_t maximum =
        std::max(minimumMilliseconds, maximumMilliseconds);
    const uint64_t minimumFrames = chinookDelayToFrames(
        minimum, logicFramesPerSecond);
    const uint64_t maximumFrames = chinookDelayToFrames(
        maximum, logicFramesPerSecond);
    const uint32_t boundedMinimum = static_cast<uint32_t>(std::min<uint64_t>(
        minimumFrames, static_cast<uint64_t>(
            std::numeric_limits<int32_t>::max())));
    const uint32_t boundedMaximum = static_cast<uint32_t>(std::min<uint64_t>(
        maximumFrames, static_cast<uint64_t>(
            std::numeric_limits<int32_t>::max())));
    const uint32_t sampled = static_cast<uint32_t>(random.integerInclusive(
        static_cast<int32_t>(boundedMinimum),
        static_cast<int32_t>(boundedMaximum)));
    // RefCode deliberately subtracts PerRopeDelayMin only for the initial
    // drop, making the first occupant jitter over 0..(max-min). Subsequent
    // occupants use the complete authored interval.
    return firstDrop ? sampled - boundedMinimum : sampled;
}

[[nodiscard]] math::q32_32 ropeGravityPerFrame(
    const ObjectSimulationRules& rules) noexcept
{
    const math::q32_32 framesPerSecond{static_cast<int32_t>(
        std::max<uint32_t>(1u, rules.logicFramesPerSecond))};
    return rules.gravityUnitsPerSecondSq /
        (framesPerSecond * framesPerSecond);
}

[[nodiscard]] math::q32_32 legacyAuthoredPerFrameAtSessionRate(
    math::q32_32 valuePerLegacyFrame,
    const ObjectSimulationRules& rules) noexcept
{
    const math::q32_32 sessionFramesPerSecond{static_cast<int32_t>(
        std::max<uint32_t>(1u, rules.logicFramesPerSecond))};
    return valuePerLegacyFrame *
        math::q32_32{int32_t{30}} / sessionFramesPerSecond;
}

void advanceRopeWobble(ObjectChinookAiRuntime::Rope& rope,
                       math::q32_32 ratePerFrame) noexcept
{
    constexpr math::q32_32 kTwoPi =
        math::q32_32::from_raw(26986075409ll);
    rope.wobblePhase += ratePerFrame;
    if (rope.wobblePhase > kTwoPi || rope.wobblePhase < -kTwoPi)
    {
        rope.wobblePhase = math::q32_32::from_raw(
            rope.wobblePhase.raw() % kTwoPi.raw());
    }
}

[[nodiscard]] ObjectChinookRopePresentationEvent chinookRopeEvent(
    ObjectChinookRopePresentationControl control, ObjectId object,
    const game::ObjectChinookAiRule& rule,
    const ObjectChinookAiRuntime::Rope& rope,
    uint64_t confirmedTick) {
    return {
        .control = control,
        .object = object,
        .authoredOrder = rule.authoredOrder,
        .ropeIndex = rope.ropeIndex,
        .ropeIdentity = rope.identity,
        .ropeName = rule.ropeName,
        .anchor = rope.endpoint.ropeStart,
        .maximumLength = math::q32_32::max(
            math::q32_32{int32_t{1}}, rope.targetLength).to_float(),
        .currentLength = rope.presentedLength.to_float(),
        .width = rule.ropeWidth,
        .color = {rule.ropeColorRed, rule.ropeColorGreen,
                  rule.ropeColorBlue},
        .wobbleLength = math::q32_32::min(
            math::q32_32::max(math::q32_32{int32_t{1}},
                              rope.targetLength),
            rule.ropeWobbleLengthFixed).to_float(),
        .wobbleAmplitude = rule.ropeWobbleAmplitude,
        .wobbleRatePerFrame = rope.wobbleRatePerFrame.to_float(),
        .wobblePhase = rope.wobblePhase.to_float(),
        .verticalOffset = rope.verticalOffset.to_float(),
        .currentSpeedPerFrame = rope.currentSpeedPerFrame.to_float(),
        .maximumSpeedPerFrame = rope.maximumSpeedPerFrame.to_float(),
        .accelerationPerFrame = rope.accelerationPerFrame.to_float(),
        .confirmedTick = confirmedTick,
    };
}

[[nodiscard]] bool objectAlive(const ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ObjectId object) noexcept
{
    if (!object || lifecycle.isPendingDestroy(object))
        return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;
    const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, *entity);
    return !health || !health->effectivelyDead;
}

[[nodiscard]] bool parkedForAirfieldHealing(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft) noexcept
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(aircraft);
    if (!entity)
        return false;
    if (const ObjectAirfieldComponent* aircraftRuntime =
            ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
        aircraftRuntime && !aircraftRuntime->jetAi.empty())
    {
        return std::any_of(
            aircraftRuntime->jetAi.begin(), aircraftRuntime->jetAi.end(),
            [](const ObjectJetAiRuntime& jet)
            {
                return jet.state == ObjectAircraftRuntimeState::Parked ||
                       jet.state == ObjectAircraftRuntimeState::Reloading;
            });
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, *entity);
    return !airborne || !airborne->isAirborne;
}

[[nodiscard]] PlayerId ownerOf(const ecs::registry& registry,
                               ecs::entity entity) noexcept
{
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, entity);
    return owner ? owner->player : INVALID_PLAYER_ID;
}

[[nodiscard]] LogicFixedVec3 constrainedSpectreTarget(
    LogicFixedVec3 initialTarget, LogicFixedVec3 requestedTarget,
    const game::ObjectSpectreGunshipRule& rule) noexcept
{
    using Fixed = math::q32_32;
    const Fixed deltaX = requestedTarget.x - initialTarget.x;
    const Fixed deltaY = requestedTarget.y - initialTarget.y;
    const Fixed distance = Fixed::sqrt(deltaX * deltaX + deltaY * deltaY);
    const Fixed radius = Fixed::max(
        Fixed{}, rule.attackAreaRadiusFixed -
            rule.targetingReticleRadiusFixed);
    if (distance > radius && distance > Fixed{}) {
        const Fixed scale = radius / distance;
        requestedTarget.x = initialTarget.x + deltaX * scale;
        requestedTarget.y = initialTarget.y + deltaY * scale;
    }
    return requestedTarget;
}

[[nodiscard]] LogicFixedVec3 spectrePosition(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->hasAuthoritativePosition) {
        return physics->position;
    }
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    return transform
        ? readAuthoritativeObjectPosition(registry, entity, *transform)
        : LogicFixedVec3{};
}

[[nodiscard]] math::q32_32 spectreDistanceSquared2D(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept {
    const math::q32_32 dx = left.x - right.x;
    const math::q32_32 dy = left.y - right.y;
    return dx * dx + dy * dy;
}

void setSpectreMoveOrder(ecs::registry& registry, ecs::entity entity,
                         PlayerId player, const game::ObjectSpectreGunshipRule& rule,
                         const LogicFixedVec3& destination,
                         uint64_t confirmedTick) {
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue)
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
    const bool same = queue->orders.size() == 1 &&
        queue->orders.front().kind == ObjectOrderKind::Move &&
        queue->orders.front().source == ObjectOrderSource::System &&
        queue->orders.front().systemPurpose ==
            ObjectOrderSystemPurpose::Generic &&
        queue->orders.front().sourceSequence == rule.authoredOrder &&
        queue->orders.front().contentName == rule.specialPowerTemplate &&
        queue->orders.front().hasTargetPosition &&
        queue->orders.front().targetX == destination.x &&
        queue->orders.front().targetY == destination.y &&
        queue->orders.front().targetZ == destination.z;
    if (same) return;
    queue->orders.clear();
    queue->orders.push_back({
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .contextPlayer = player,
        .issuedTick = confirmedTick,
        .sourceSequence = rule.authoredOrder,
        .targetX = destination.x,
        .targetY = destination.y,
        .targetZ = destination.z,
        .hasTargetPosition = true,
        .contentName = rule.specialPowerTemplate,
        .systemPurpose = ObjectOrderSystemPurpose::Generic,
        .systemPurposeInstance = rule.authoredOrder,
    });
    ++queue->revision;
}

void publishSpectreModelState(
    ecs::registry& registry, ecs::entity entity,
    ObjectSpectreGunshipPhase phase, uint64_t confirmedTick,
    uint64_t sequence) {
    const bool transit = phase == ObjectSpectreGunshipPhase::Inserting ||
        phase == ObjectSpectreGunshipPhase::Departing;
    const game::ModelConditionMask afterburner =
        game::modelConditionMaskOf(game::ModelConditionFlag::JetAfterburner);
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Airfield,
        afterburner, transit ? afterburner : game::ModelConditionMask{},
        confirmedTick, sequence);
    publishObjectModelConditionDoor(
        registry, entity, ObjectModelConditionDoorSource::Airfield, 0,
        transit ? ObjectModelConditionDoorPhase::Opening
                : ObjectModelConditionDoorPhase::Closing,
        confirmedTick, sequence);
}

[[nodiscard]] ObjectRadiusDecalEvent spectreDecalEvent(
    ObjectRadiusDecalEventKind kind, ObjectRadiusDecalEventSource source,
    const ecs::registry& registry, ecs::entity entity, ObjectId object,
    const game::ObjectSpectreGunshipRule& rule,
    const game::ObjectSpectreRadiusDecalRule& decal,
    LogicFixedVec3 position, math::q32_32 radius,
    const ObjectSimulationRules& rules, uint64_t confirmedTick)
{
    return {
        .kind = kind,
        .source = source,
        .object = object,
        .owner = ownerOf(registry, entity),
        .authoredOrder = rule.authoredOrder,
        .texture = decal.texture,
        .position = position,
        .radius = math::q32_32::max(math::q32_32{}, radius),
        .shadowTypeMask = decal.shadowTypeMask,
        .minimumOpacity = math::q32_32::clamp(
            decal.minimumOpacity, math::q32_32{},
            math::q32_32{int32_t{1}}),
        .maximumOpacity = math::q32_32::clamp(
            decal.maximumOpacity, math::q32_32{},
            math::q32_32{int32_t{1}}),
        .opacityThrobTicks = millisecondsToFrames(
            decal.opacityThrobMilliseconds, rules.logicFramesPerSecond),
        .color = decal.color,
        .usesPlayerColor = decal.usesPlayerColor,
        .onlyVisibleToOwningPlayer = decal.onlyVisibleToOwningPlayer,
        .confirmedTick = confirmedTick,
    };
}

[[nodiscard]] bool objectEffectivelyDead(const ecs::registry& registry,
                                         const ObjectLifecycle& lifecycle,
                                         ObjectId object) noexcept
{
    if (!object)
        return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromIdIncludingPending(object);
    if (!entity)
        return false;
    const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, *entity);
    return health && health->effectivelyDead;
}

[[nodiscard]] std::optional<size_t> flightDeckRunwayForSlot(
    const game::ObjectFlightDeckRule& rule, size_t slotIndex) noexcept;

[[nodiscard]] math::q32_32 fixedQuaternionYaw(
    const data::w3d::FixedQuaternion& rotation) noexcept
{
    const math::q32_32 two{int32_t{2}};
    const math::q32_32 numerator = two *
        (rotation.w * rotation.z + rotation.x * rotation.y);
    const math::q32_32 denominator = math::q32_32{int32_t{1}} - two *
        (rotation.y * rotation.y + rotation.z * rotation.z);
    return math::fixed_atan2(numerator, denominator);
}

[[nodiscard]] LogicFixedVec3 worldJetBonePosition(
    const LogicFixedVec3& position, math::q32_32 yaw,
    const data::w3d::FixedRigidTransform& local) noexcept
{
    const math::q32_32_sincos direction = math::fixed_sincos(yaw);
    const math::q32_32 x = local.translation.x;
    const math::q32_32 y = local.translation.y;
    return {
        .x = position.x +
             x * direction.cosine - y * direction.sine,
        .y = position.y +
             x * direction.sine + y * direction.cosine,
        .z = position.z +
             local.translation.z,
    };
}

[[nodiscard]] std::optional<data::w3d::FixedRigidTransform> jetBone(
    const ecs::registry& registry, ecs::entity airfield,
    const GameContentSnapshot* content,
    container::StringView name) noexcept
{
    if (!content || name.empty()) return std::nullopt;
    const game::W3dPristineBoneCatalog* catalog =
        content->pristineBoneCatalog();
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, airfield);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, airfield);
    if (!catalog || !catalog->isLoaded() || !type || !type->archetype ||
        !visual) {
        return std::nullopt;
    }
    const game::ThingTemplate& templateData =
        type->archetype->templateData;
    const size_t visualRule = game::selectModelConditionVisualRuleIndex(
        templateData, visual->modelConditionFlags);
    if (visualRule >= templateData.modelConditionVisuals.size())
        return std::nullopt;
    return catalog->find(type->archetype->name, visualRule, name);
}

[[nodiscard]] container::String numberedJetBone(
    container::StringView prefix, size_t first, container::StringView middle,
    size_t second = 0)
{
    container::String result(prefix);
    result += std::to_string(first);
    result += middle;
    if (second != 0) result += std::to_string(second);
    return result;
}

[[nodiscard]] JetParkingGeometry resolveJetParkingGeometry(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot* content,
    const ObjectAirfieldReservation& reservation,
    math::q32_32 parkingOffset) noexcept
{
    JetParkingGeometry result;
    const std::optional<ecs::entity> airfield =
        lifecycle.entityFromId(reservation.airfield);
    if (!airfield) return result;
    const ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *airfield);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *airfield);
    if (!component || !component->plan || !transform) return result;
    const LogicFixedVec3 origin = readAuthoritativeObjectPosition(
        registry, *airfield, *transform);
    const math::q32_32 airfieldYaw = readAuthoritativeObjectYaw(
        registry, *airfield, *transform);

    const auto resolve = [&](container::StringView name)
        -> std::optional<data::w3d::FixedRigidTransform> {
        return jetBone(registry, *airfield, content, name);
    };
    const auto world = [&](container::StringView name,
                           LogicFixedVec3 fallback,
                           math::q32_32* yaw = nullptr) -> LogicFixedVec3 {
        const auto bone = resolve(name);
        if (!bone) return fallback;
        if (yaw) *yaw = airfieldYaw + fixedQuaternionYaw(bone->rotation);
        return worldJetBonePosition(origin, airfieldYaw, *bone);
    };

    math::q32_32 approachHeight{};
    math::q32_32 deckHeight{};
    if (reservation.slotKind == ObjectAirfieldSlotKind::ParkingPlace &&
        reservation.moduleIndex < component->plan->parkingPlaces.size()) {
        const game::ObjectParkingPlaceRule& rule =
            component->plan->parkingPlaces[reservation.moduleIndex];
        if (rule.cols <= 0) return result;
        const size_t column = reservation.slotIndex %
            static_cast<size_t>(rule.cols);
        const size_t row = reservation.slotIndex /
            static_cast<size_t>(rule.cols);
        const size_t runwayOrdinal = column + 1u;
        const size_t parkingOrdinal = row + 1u;
        const container::String parkingName = rule.parkInHangars
            ? numberedJetBone("Runway", runwayOrdinal, "Park", parkingOrdinal) + "Han"
            : numberedJetBone("Runway", runwayOrdinal, "Parking", parkingOrdinal);
        result.parking = world(parkingName, origin,
                               &result.parkingOrientationRadians);
        result.creation.push_back(world(numberedJetBone(
            "Runway", runwayOrdinal, "Park", parkingOrdinal) + "Han",
            result.parking, &result.creationOrientationRadians));
        result.runwayPrep = world(numberedJetBone(
            "Runway", runwayOrdinal, "Prep", parkingOrdinal),
            result.parking);
        result.runwayStart = world(numberedJetBone(
            "RunwayStart", runwayOrdinal, ""), result.runwayPrep);
        result.runwayEnd = world(numberedJetBone(
            "RunwayEnd", runwayOrdinal, ""), result.runwayStart);
        result.landingStart = result.runwayEnd;
        result.landingEnd = result.runwayStart;
        approachHeight = rule.approachHeightFixed;
        deckHeight = rule.landingDeckHeightOffsetFixed;
    } else if (reservation.slotKind == ObjectAirfieldSlotKind::FlightDeck &&
               reservation.moduleIndex < component->plan->flightDecks.size()) {
        const game::ObjectFlightDeckRule& rule =
            component->plan->flightDecks[reservation.moduleIndex];
        const std::optional<size_t> runway =
            flightDeckRunwayForSlot(rule, reservation.slotIndex);
        if (!runway || *runway >= rule.runwayDefinitions.size()) return result;
        const game::ObjectFlightDeckRunwayRule& authored =
            rule.runwayDefinitions[*runway];
        size_t firstSlot = 0;
        for (size_t index = 0; index < *runway; ++index) {
            firstSlot += !rule.runwayDefinitions[index].spaceBones.empty()
                ? rule.runwayDefinitions[index].spaceBones.size()
                : static_cast<size_t>(std::max(0, rule.spacesPerRunway));
        }
        const size_t localSlot = reservation.slotIndex >= firstSlot
            ? reservation.slotIndex - firstSlot : 0;
        const container::StringView parkingBone =
            localSlot < authored.spaceBones.size()
                ? container::StringView{authored.spaceBones[localSlot]}
                : container::StringView{};
        result.parking = world(parkingBone, origin,
                               &result.parkingOrientationRadians);
        result.runwayPrep = result.parking;
        result.runwayStart = world(authored.takeoffBones[0], result.parking);
        result.runwayEnd = world(authored.takeoffBones[1], result.runwayStart);
        result.landingStart = world(authored.landingBones[0], result.runwayEnd);
        result.landingEnd = world(authored.landingBones[1], result.runwayStart);
        for (const container::String& name : authored.taxiBones)
            result.taxi.push_back(world(name, result.parking));
        for (const container::String& name : authored.creationBones) {
            math::q32_32* creationYaw = result.creation.empty()
                ? &result.creationOrientationRadians : nullptr;
            result.creation.push_back(world(name, result.parking,
                                            creationYaw));
        }
        approachHeight = rule.approachHeightFixed;
        deckHeight = rule.landingDeckHeightOffsetFixed;
        result.flightDeck = true;
    } else {
        return result;
    }

    const math::q32_32_sincos parkingDirection = math::fixed_sincos(
        result.parkingOrientationRadians);
    result.parking.x += parkingOffset * parkingDirection.cosine;
    result.parking.y += parkingOffset * parkingDirection.sine;
    if (!result.flightDeck) {
        // JetOrHeliTaxiState does not cut diagonally from RunwayPrep into a
        // parking slot whose authored heading differs. It intersects the
        // parking heading with the perpendicular through RunwayPrep. In fixed
        // arithmetic that intersection is the projection of RunwayPrep onto
        // the parking-heading line.
        const math::q32_32 dx = result.runwayPrep.x - result.parking.x;
        const math::q32_32 dy = result.runwayPrep.y - result.parking.y;
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > math::q32_32{}) {
            const math::q32_32 distance =
                math::q32_32::sqrt(distanceSquared);
            constexpr math::q32_32 kPi =
                math::q32_32::from_raw(13'493'037'705ll);
            const math::q32_32 alignmentThreshold =
                math::fixed_cos(kPi / math::q32_32{int32_t{128}});
            const math::q32_32 alignment =
                (dx * parkingDirection.cosine +
                 dy * parkingDirection.sine) / distance;
            if (alignment < alignmentThreshold) {
                const math::q32_32 projection =
                    dx * parkingDirection.cosine +
                    dy * parkingDirection.sine;
                result.intermediate = {
                    .x = result.parking.x +
                         parkingDirection.cosine * projection,
                    .y = result.parking.y +
                         parkingDirection.sine * projection,
                    .z = (result.parking.z + result.runwayPrep.z) /
                         math::q32_32{int32_t{2}},
                };
                result.hasIntermediate = true;
            }
        }
    }
    const math::q32_32 approachScale =
        math::q32_32::from_fraction(3, 4);
    result.approach = {
        .x = result.landingStart.x +
             (result.landingStart.x - result.landingEnd.x) * approachScale,
        .y = result.landingStart.y +
             (result.landingStart.y - result.landingEnd.y) * approachScale,
        .z = result.landingStart.z + approachHeight + deckHeight,
    };
    result.runwayExit = {
        .x = result.runwayEnd.x +
             (result.runwayEnd.x - result.runwayStart.x) * approachScale,
        .y = result.runwayEnd.y +
             (result.runwayEnd.y - result.runwayStart.y) * approachScale,
        .z = result.runwayEnd.z + approachHeight + deckHeight,
    };
    result.valid = true;
    return result;
}

[[nodiscard]] HelicopterLandingGeometry resolveHelicopterLandingGeometry(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic* terrain, ObjectId airfield,
    ObjectId aircraft, math::q32_32 approachHeight) noexcept
{
    HelicopterLandingGeometry result;
    const std::optional<ecs::entity> airfieldEntity =
        lifecycle.entityFromId(airfield);
    const std::optional<ecs::entity> aircraftEntity =
        lifecycle.entityFromId(aircraft);
    if (!airfieldEntity || !aircraftEntity) return result;
    const TransformComponent* airfieldTransform =
        ecs::try_get<TransformComponent>(registry, *airfieldEntity);
    if (!airfieldTransform) return result;
    const LogicFixedVec3 origin = readAuthoritativeObjectPosition(
        registry, *airfieldEntity, *airfieldTransform);
    const ObjectGeometryComponent* airfieldGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *airfieldEntity);
    const ObjectGeometryComponent* aircraftGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *aircraftEntity);
    const math::q32_32 airfieldRadius = airfieldGeometry
        ? math::q32_32::max({}, airfieldGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{30}};
    const math::q32_32 aircraftRadius = aircraftGeometry
        ? math::q32_32::max({}, aircraftGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{8}};
    const math::q32_32 baseDistance = airfieldRadius + aircraftRadius +
        math::q32_32{int32_t{6}};
    const math::q32_32 one{int32_t{1}};
    const math::q32_32 diagonal =
        math::q32_32::from_raw(INT64_C(3037000500));
    const container::Array<container::Array<math::q32_32, 2>, 8>
        directions{{
            {{one, {}}}, {{diagonal, diagonal}}, {{{}, one}},
            {{-diagonal, diagonal}}, {{-one, {}}},
            {{-diagonal, -diagonal}}, {{{}, -one}},
            {{diagonal, -diagonal}},
        }};
    const size_t firstDirection = aircraft.value % directions.size();
    const auto occupied = [&](math::q32_32 x, math::q32_32 y) {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const TransformComponent,
            const ObjectGeometryComponent>(registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            if (!identity.id || identity.id == aircraft ||
                identity.id == airfield ||
                !objectAlive(registry, lifecycle, identity.id)) {
                continue;
            }
            if (const ObjectAirborneComponent* airborne =
                    ecs::try_get<ObjectAirborneComponent>(registry, entity);
                airborne && airborne->isAirborne) {
                continue;
            }
            const TransformComponent& transform =
                view.template get<const TransformComponent>(entity);
            const ObjectGeometryComponent& geometry =
                view.template get<const ObjectGeometryComponent>(entity);
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                registry, entity, transform);
            const math::q32_32 radius = aircraftRadius +
                math::q32_32::max({}, geometry.boundingCircleRadiusFixed) +
                math::q32_32{int32_t{2}};
            const math::q32_32 dx = position.x - x;
            const math::q32_32 dy = position.y - y;
            if (dx * dx + dy * dy < radius * radius) return true;
        }
        return false;
    };
    LogicFixedVec3 selected = origin;
    bool found = false;
    for (int32_t ring = 1; ring <= 10 && !found; ++ring) {
        const math::q32_32 distance = baseDistance *
            math::q32_32{ring};
        for (size_t offset = 0; offset < directions.size(); ++offset) {
            const auto& direction = directions[
                (firstDirection + offset) % directions.size()];
            const math::q32_32 x = origin.x + direction[0] * distance;
            const math::q32_32 y = origin.y + direction[1] * distance;
            if (occupied(x, y)) continue;
            selected.x = x;
            selected.y = y;
            found = true;
            break;
        }
    }
    if (!found) {
        const auto& direction = directions[firstDirection];
        selected.x = origin.x + direction[0] * baseDistance;
        selected.y = origin.y + direction[1] * baseDistance;
    }
    selected.z = terrain
        ? math::q32_32::from_raw(terrain->groundHeightRaw(
              selected.x.raw(), selected.y.raw()))
        : origin.z;
    result.landing = selected;
    result.approach = selected;
    result.approach.z += math::q32_32::max(
        math::q32_32{int32_t{1}}, approachHeight);
    result.valid = true;
    return result;
}

bool setAirfieldHealee(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, bool add) noexcept
{
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(airfield);
    ObjectAirfieldComponent* component = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    if (!component || component->parkingPlaces.empty() || !aircraft)
        return false;
    bool changed = false;
    for (ObjectAirfieldParkingRuntime& parking : component->parkingPlaces) {
        auto found = std::find(parking.healees.begin(),
                               parking.healees.end(), aircraft);
        if (add) {
            if (found == parking.healees.end()) {
                parking.healees.push_back(aircraft);
                std::sort(parking.healees.begin(), parking.healees.end());
                changed = true;
            }
            break;
        }
        if (found != parking.healees.end()) {
            parking.healees.erase(found);
            changed = true;
        }
    }
    return changed;
}

void publishJetPosition(ecs::registry& registry, ecs::entity entity,
                        LogicFixedVec3 position,
                        std::optional<math::q32_32> yaw) noexcept
{
    writeAuthoritativeObjectPosition(registry, entity, position);
    if (yaw) writeAuthoritativeObjectYaw(registry, entity, *yaw);
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        physics->position = position;
        physics->lastPublishedPosition = position;
        physics->velocityUnitsPerSecond = {};
        if (yaw) {
            physics->yaw = *yaw;
            physics->lastPublishedYaw = physics->yaw;
        }
    }
}

[[nodiscard]] bool advanceJetRoute(
    ecs::registry& registry, ecs::entity entity,
    ObjectJetAiRuntime& runtime, const ObjectSimulationRules& rules,
    bool taxiing) noexcept
{
    if (runtime.nextRoutePoint >= runtime.route.size()) return true;
    TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    if (!transform) {
        runtime.nextRoutePoint = runtime.route.size();
        return true;
    }
    const LogicFixedVec3 current = readAuthoritativeObjectPosition(
        registry, entity, *transform);
    const LogicFixedVec3 target = runtime.route[runtime.nextRoutePoint];
    const LogicFixedVec3 delta{
        target.x - current.x, target.y - current.y, target.z - current.z};
    const math::q32_32 distance = math::q32_32::sqrt(
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    math::q32_32 speed{};
    if (const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
        speed = math::q32_32::max(
            math::q32_32{}, locomotion->maximumSpeed);
    }
    if (speed <= math::q32_32{})
        speed = math::q32_32{taxiing ? 30 : 120};
    if (taxiing) speed = math::q32_32::min(speed, math::q32_32{60});
    const math::q32_32 step = speed /
        math::q32_32{static_cast<int32_t>(std::max<uint32_t>(
            1u, rules.logicFramesPerSecond))};
    const math::q32_32 yaw =
        (delta.x.raw() != 0 || delta.y.raw() != 0)
        ? math::fixed_atan2(delta.y, delta.x)
        : readAuthoritativeObjectYaw(registry, entity, *transform);
    const auto publishVelocity = [&](LogicFixedVec3 velocity,
                                     bool moving) {
        if (ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
            locomotion->hasActiveMove = moving;
            locomotion->forwardSpeed = moving
                ? math::q32_32::sqrt(
                      velocity.x * velocity.x + velocity.y * velocity.y)
                : math::q32_32{};
            locomotion->verticalSpeed = moving
                ? velocity.z : math::q32_32{};
            locomotion->state = moving
                ? ObjectLocomotionState::Moving
                : ObjectLocomotionState::Idle;
        }
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
            physics->velocityUnitsPerSecond = moving
                ? velocity : LogicFixedVec3{};
        }
    };
    const math::q32_32 frameRate{static_cast<int32_t>(
        std::max<uint32_t>(1u, rules.logicFramesPerSecond))};
    if (distance <= step || distance.raw() <= 0) {
        publishJetPosition(registry, entity, target, yaw);
        ++runtime.nextRoutePoint;
        const bool complete = runtime.nextRoutePoint >= runtime.route.size();
        publishVelocity({
            .x = delta.x * frameRate,
            .y = delta.y * frameRate,
            .z = delta.z * frameRate,
        }, !complete && distance.raw() > 0);
        return complete;
    }
    const math::q32_32 scale = step / distance;
    const LogicFixedVec3 movement{
        .x = delta.x * scale,
        .y = delta.y * scale,
        .z = delta.z * scale,
    };
    publishJetPosition(registry, entity, {
        .x = current.x + delta.x * scale,
        .y = current.y + delta.y * scale,
        .z = current.z + delta.z * scale,
    }, yaw);
    publishVelocity({
        .x = movement.x * frameRate,
        .y = movement.y * frameRate,
        .z = movement.z * frameRate,
    }, true);
    return false;
}

bool advanceChinookFlightRoute(
    ecs::registry& registry, ecs::entity entity,
    ObjectChinookAiRuntime& runtime, const ObjectSimulationRules& rules)
    noexcept
{
    ObjectJetAiRuntime route;
    route.route = runtime.flightRoute;
    route.nextRoutePoint = runtime.nextFlightRoutePoint;
    const bool complete = advanceJetRoute(
        registry, entity, route, rules, false);
    runtime.nextFlightRoutePoint = route.nextRoutePoint;
    return complete;
}

[[nodiscard]] bool isResumableJetOrder(
    const ObjectOrderIntent& order) noexcept
{
    if (order.kind != ObjectOrderKind::TacticalAttack) return false;
    return order.tacticalAttackSubtype == ObjectTacticalAttackSubtype::Hunt ||
        order.tacticalAttackSubtype == ObjectTacticalAttackSubtype::Guard ||
        order.tacticalAttackSubtype == ObjectTacticalAttackSubtype::GuardRetaliate;
}

bool purgeDeadSlots(const ecs::registry& registry, const ObjectLifecycle& lifecycle,
                    container::Vector<ObjectId>& slots)
{
    bool changed = false;
    for (ObjectId& object : slots)
    {
        if (object && !objectAlive(registry, lifecycle, object))
        {
            object = INVALID_OBJECT_ID;
            changed = true;
        }
    }
    return changed;
}

std::optional<size_t> findSlot(const container::Vector<ObjectId>& slots,
                               ObjectId object) noexcept
{
    for (size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index] == object)
            return index;
    }
    return std::nullopt;
}

std::optional<size_t> findFreeSlot(
    const container::Vector<ObjectId>& slots) noexcept
{
    for (size_t index = 0; index < slots.size(); ++index)
    {
        if (!slots[index])
            return index;
    }
    return std::nullopt;
}

[[nodiscard]] ObjectAirfieldEvent makeSlotEvent(ObjectAirfieldEventKind kind,
                                                ObjectId airfield,
                                                ObjectId aircraft,
                                                ObjectAirfieldSlotKind slotKind,
                                                size_t moduleIndex,
                                                size_t slotIndex,
                                                uint32_t authoredOrder,
                                                container::String moduleClass,
                                                uint32_t slotCount,
                                                uint32_t runwayCount,
                                                uint64_t confirmedTick)
{
    return {
        .kind = kind,
        .object = airfield,
        .aircraft = aircraft,
        .slotKind = slotKind,
        .moduleIndex = moduleIndex,
        .slotIndex = slotIndex,
        .authoredOrder = authoredOrder,
        .moduleClass = std::move(moduleClass),
        .slotCount = slotCount,
        .runwayCount = runwayCount,
        .confirmedTick = confirmedTick,
    };
}

[[nodiscard]] std::optional<size_t> parkingRunwayForSlot(const game::ObjectParkingPlaceRule& rule,
                                                         size_t slotIndex) noexcept
{
    if (!rule.hasRunways || rule.cols <= 0)
        return std::nullopt;
    return slotIndex % static_cast<size_t>(rule.cols);
}

[[nodiscard]] std::optional<size_t> flightDeckRunwayForSlot(const game::ObjectFlightDeckRule& rule,
                                                            size_t slotIndex) noexcept
{
    size_t firstSlot = 0;
    for (size_t runway = 0; runway < rule.runwayDefinitions.size(); ++runway)
    {
        const size_t count = !rule.runwayDefinitions[runway].spaceBones.empty()
                                 ? rule.runwayDefinitions[runway].spaceBones.size()
                                 : static_cast<size_t>(std::max(0, rule.spacesPerRunway));
        if (slotIndex >= firstSlot && slotIndex < firstSlot + count)
        {
            return runway;
        }
        firstSlot += count;
    }
    if (rule.spacesPerRunway <= 0)
        return std::nullopt;
    const size_t runway = slotIndex / static_cast<size_t>(rule.spacesPerRunway);
    return runway < static_cast<size_t>(std::max(0, rule.runways)) ? std::optional<size_t>{runway} : std::nullopt;
}

void rememberAircraftParkingReservation(ecs::registry& registry,
                                        const ObjectLifecycle& lifecycle,
                                        ObjectId aircraft,
                                        const ObjectAirfieldReservation& reservation,
                                        uint64_t confirmedTick)
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(aircraft);
    if (!entity)
        return;
    // Object::getCarrierDeckHeight resolves the first authored
    // ParkingPlaceBehaviorInterface on the producer.  Freeze that same value
    // at reservation time so Physics never walks live module state.
    math::q32_32 deckHeightOffset{};
    uint32_t firstAuthoredOrder = std::numeric_limits<uint32_t>::max();
    if (const std::optional<ecs::entity> carrierEntity =
            lifecycle.entityFromId(reservation.airfield)) {
        const ObjectAirfieldComponent* carrier =
            ecs::try_get<ObjectAirfieldComponent>(registry, *carrierEntity);
        if (carrier && carrier->plan) {
            for (const game::ObjectParkingPlaceRule& rule :
                 carrier->plan->parkingPlaces) {
                if (rule.authoredOrder < firstAuthoredOrder) {
                    firstAuthoredOrder = rule.authoredOrder;
                    deckHeightOffset = rule.landingDeckHeightOffsetFixed;
                }
            }
            for (const game::ObjectFlightDeckRule& rule :
                 carrier->plan->flightDecks) {
                if (rule.authoredOrder < firstAuthoredOrder) {
                    firstAuthoredOrder = rule.authoredOrder;
                    deckHeightOffset = rule.landingDeckHeightOffsetFixed;
                }
            }
        }
    }
    if (deckHeightOffset != math::q32_32{}) {
        ObjectCarrierDeckComponent value{
            .carrier = reservation.airfield,
            .heightOffset = deckHeightOffset,
        };
        if (ObjectCarrierDeckComponent* existing =
                ecs::try_get<ObjectCarrierDeckComponent>(registry, *entity)) {
            *existing = value;
        } else {
            ecs::emplace<ObjectCarrierDeckComponent>(registry, *entity,
                                                      value);
        }
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.setMask = game::objectStatusBit(
                 game::ObjectStatusFlag::DeckHeightOffset),
             .confirmedTick = confirmedTick}));
    } else {
        ecs::remove<ObjectCarrierDeckComponent>(registry, *entity);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.clearMask = game::objectStatusBit(
                 game::ObjectStatusFlag::DeckHeightOffset),
             .confirmedTick = confirmedTick}));
    }
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component)
        return;
    for (ObjectJetAiRuntime& runtime : component->jetAi)
    {
        runtime.reservedAirfield = reservation.airfield;
        runtime.parkingReservation = reservation;
        if (runtime.state == ObjectAircraftRuntimeState::Idle) {
            runtime.state = ObjectAircraftRuntimeState::Parked;
            runtime.phase = ObjectJetAirfieldPhase::Parked;
        }
    }
}

void rememberAircraftRunwayReservation(ecs::registry& registry,
                                       const ObjectLifecycle& lifecycle,
                                       ObjectId aircraft,
                                       const ObjectAirfieldReservation& reservation)
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(aircraft);
    if (!entity)
        return;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component)
        return;
    for (ObjectJetAiRuntime& runtime : component->jetAi)
    {
        runtime.reservedAirfield = reservation.airfield;
        runtime.runwayReservation = reservation;
        if (runtime.state == ObjectAircraftRuntimeState::Idle ||
            runtime.state == ObjectAircraftRuntimeState::Parked) {
            runtime.state = !reservation.active
                ? ObjectAircraftRuntimeState::Parked
                : reservation.slotKind == ObjectAirfieldSlotKind::LandingRunway
                    ? ObjectAircraftRuntimeState::Landing
                    : ObjectAircraftRuntimeState::Taxiing;
            runtime.phase = !reservation.active
                ? ObjectJetAirfieldPhase::AwaitTakeoffClearance
                : reservation.slotKind == ObjectAirfieldSlotKind::LandingRunway
                    ? ObjectJetAirfieldPhase::Landing
                    : ObjectJetAirfieldPhase::TaxiToTakeoff;
        }
    }
}

void clearAircraftParkingReservation(ecs::registry& registry,
                                     const ObjectLifecycle& lifecycle,
                                     ObjectId airfield,
                                     ObjectId aircraft,
                                     uint64_t confirmedTick)
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromIdIncludingPending(aircraft);
    if (!entity)
        return;
    const ObjectCarrierDeckComponent* deck =
        ecs::try_get<ObjectCarrierDeckComponent>(registry, *entity);
    if (deck && deck->carrier == airfield) {
        ecs::remove<ObjectCarrierDeckComponent>(registry, *entity);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.clearMask = game::objectStatusBit(
                 game::ObjectStatusFlag::DeckHeightOffset),
             .confirmedTick = confirmedTick}));
    }
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component)
        return;
    for (ObjectJetAiRuntime& runtime : component->jetAi)
    {
        if (runtime.parkingReservation.airfield == airfield)
            runtime.parkingReservation = {};
        if (runtime.reservedAirfield == airfield && !runtime.runwayReservation.airfield)
            runtime.reservedAirfield = INVALID_OBJECT_ID;
    }
}

void clearAircraftRunwayReservation(ecs::registry& registry,
                                    const ObjectLifecycle& lifecycle,
                                    ObjectId airfield,
                                    ObjectId aircraft)
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromIdIncludingPending(aircraft);
    if (!entity)
        return;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component)
        return;
    for (ObjectJetAiRuntime& runtime : component->jetAi)
    {
        if (runtime.runwayReservation.airfield == airfield)
            runtime.runwayReservation = {};
        if (runtime.reservedAirfield == airfield && !runtime.parkingReservation.airfield)
            runtime.reservedAirfield = INVALID_OBJECT_ID;
    }
}

[[nodiscard]] ObjectAirfieldEvent makeRuntimeEvent(ObjectAirfieldEventKind kind,
                                                   ObjectId object,
                                                   uint32_t authoredOrder,
                                                   container::String moduleClass,
                                                   size_t moduleIndex,
                                                   uint64_t confirmedTick,
                                                   ObjectAircraftRuntimeState state,
                                                   ObjectJetAirfieldPhase phase)
{
    return {
        .kind = kind,
        .object = object,
        .moduleIndex = moduleIndex,
        .authoredOrder = authoredOrder,
        .moduleClass = std::move(moduleClass),
        .aircraftState = state,
        .jetPhase = phase,
        .confirmedTick = confirmedTick,
    };
}

[[nodiscard]] ObjectAirfieldEvent makeSlowDeathEvent(ObjectId object,
                                                     uint32_t sourcePathfindLayer,
                                                     const game::ObjectAircraftSlowDeathRule& rule,
                                                     ObjectAircraftSlowDeathPhase phase,
                                                     uint64_t dueTick,
                                                     uint64_t confirmedTick,
                                                     container::String fx,
                                                     container::String ocl,
                                                     container::String audio,
                                                     container::String payloadTemplate)
{
    return {
        .kind = ObjectAirfieldEventKind::AircraftSlowDeathPhase,
        .object = object,
        .authoredOrder = rule.authoredOrder,
        .sourcePathfindLayer = sourcePathfindLayer,
        .moduleClass = rule.moduleClass,
        .slowDeathPhase = phase,
        .dueTick = dueTick,
        .fx = std::move(fx),
        .ocl = std::move(ocl),
        .audio = std::move(audio),
        .payloadTemplate = std::move(payloadTemplate),
        .confirmedTick = confirmedTick,
    };
}

} // namespace airfield_detail

void ObjectAirfieldSystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId object,
    uint64_t confirmedTick,
    container::Vector<ObjectChinookRopePresentationEvent>& outRopeEvents,
    container::Vector<ObjectRadiusDecalEvent>& outRadiusDecalEvents) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectAirfieldComponent* airfield = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    if (!airfield) return;
    const size_t chinookCount = airfield->chinookAi.size();
    const size_t spectreCount = airfield->spectreGunships.size();
    for (size_t moduleIndex = 0; moduleIndex < chinookCount; ++moduleIndex) {
        static_cast<void>(endChinookCombatDrop(
            registry, lifecycle, rules, object, moduleIndex,
            confirmedTick, true, outRopeEvents));
    }
    for (size_t moduleIndex = 0; moduleIndex < spectreCount; ++moduleIndex) {
        static_cast<void>(endSpectreGunshipTargeting(
            registry, lifecycle, rules, object, moduleIndex,
            confirmedTick, outRadiusDecalEvents));
    }
    for (ObjectJetAiRuntime& jet : airfield->jetAi) {
        if (jet.helipadHealingRegistered && jet.reservedAirfield) {
            static_cast<void>(airfield_detail::setAirfieldHealee(
                registry, lifecycle, jet.reservedAirfield, object, false));
        }
        jet.helipadHealingRegistered = false;
        jet.pendingOrder.reset();
        jet.pendingOrderTail.clear();
        jet.route.clear();
    }
    for (ObjectChinookAiRuntime& chinook : airfield->chinookAi) {
        if (chinook.healingRegistered && chinook.healingAirfield) {
            static_cast<void>(airfield_detail::setAirfieldHealee(
                registry, lifecycle, chinook.healingAirfield, object,
                false));
        }
        chinook.healingRegistered = false;
        chinook.pendingOrder.reset();
        chinook.pendingOrderTail.clear();
        chinook.flightRoute.clear();
    }
}

} // namespace engine
