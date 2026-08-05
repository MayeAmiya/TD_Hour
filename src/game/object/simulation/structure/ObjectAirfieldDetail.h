#pragma once

#include "game/object/simulation/structure/ObjectAirfield.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"

namespace engine
{
namespace airfield_detail
{

inline constexpr auto asciiEqualInsensitive =
    container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept;
[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept;
[[nodiscard]] uint64_t chinookDelayToFrames(
    uint32_t authoredMillisecondsOrLegacySentinel,
    uint32_t logicFramesPerSecond) noexcept;
[[nodiscard]] uint64_t chinookRopeIdentity(
    ObjectId object, uint32_t authoredOrder, uint32_t generation,
    uint32_t ropeIndex) noexcept;
[[nodiscard]] uint64_t aircraftSlowDeathBladeDelayFrames(
    ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick,
    uint32_t minimumMilliseconds, uint32_t maximumMilliseconds,
    uint32_t logicFramesPerSecond) noexcept;
[[nodiscard]] uint64_t randomChinookDelayFrames(
    SimulationRandom& random, uint32_t minimumMilliseconds,
    uint32_t maximumMilliseconds, uint32_t logicFramesPerSecond,
    bool firstDrop) noexcept;
[[nodiscard]] math::q32_32 ropeGravityPerFrame(
    const ObjectSimulationRules& rules) noexcept;
[[nodiscard]] math::q32_32 legacyAuthoredPerFrameAtSessionRate(
    math::q32_32 valuePerLegacyFrame,
    const ObjectSimulationRules& rules) noexcept;
void advanceRopeWobble(ObjectChinookAiRuntime::Rope& rope,
                       math::q32_32 ratePerFrame) noexcept;
[[nodiscard]] ObjectChinookRopePresentationEvent chinookRopeEvent(
    ObjectChinookRopePresentationControl control, ObjectId object,
    const game::ObjectChinookAiRule& rule,
    const ObjectChinookAiRuntime::Rope& rope, uint64_t confirmedTick);

[[nodiscard]] bool objectAlive(const ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ObjectId object) noexcept;
[[nodiscard]] bool parkedForAirfieldHealing(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft) noexcept;
[[nodiscard]] PlayerId ownerOf(const ecs::registry& registry,
                               ecs::entity entity) noexcept;
[[nodiscard]] LogicFixedVec3 constrainedSpectreTarget(
    LogicFixedVec3 initialTarget, LogicFixedVec3 requestedTarget,
    const game::ObjectSpectreGunshipRule& rule) noexcept;
[[nodiscard]] LogicFixedVec3 spectrePosition(
    const ecs::registry& registry, ecs::entity entity) noexcept;
[[nodiscard]] math::q32_32 spectreDistanceSquared2D(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept;
void setSpectreMoveOrder(
    ecs::registry& registry, ecs::entity entity, PlayerId player,
    const game::ObjectSpectreGunshipRule& rule,
    const LogicFixedVec3& destination, uint64_t confirmedTick);
void publishSpectreModelState(
    ecs::registry& registry, ecs::entity entity,
    ObjectSpectreGunshipPhase phase, uint64_t confirmedTick,
    uint64_t sequence);
[[nodiscard]] ObjectRadiusDecalEvent spectreDecalEvent(
    ObjectRadiusDecalEventKind kind, ObjectRadiusDecalEventSource source,
    const ecs::registry& registry, ecs::entity entity, ObjectId object,
    const game::ObjectSpectreGunshipRule& rule,
    const game::ObjectSpectreRadiusDecalRule& decal,
    LogicFixedVec3 position, math::q32_32 radius,
    const ObjectSimulationRules& rules, uint64_t confirmedTick);
[[nodiscard]] bool objectEffectivelyDead(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept;

struct JetParkingGeometry final
{
    LogicFixedVec3 parking{};
    LogicFixedVec3 runwayPrep{};
    LogicFixedVec3 intermediate{};
    LogicFixedVec3 runwayStart{};
    LogicFixedVec3 runwayEnd{};
    LogicFixedVec3 landingStart{};
    LogicFixedVec3 landingEnd{};
    LogicFixedVec3 approach{};
    LogicFixedVec3 runwayExit{};
    container::Vector<LogicFixedVec3> taxi;
    container::Vector<LogicFixedVec3> creation;
    math::q32_32 parkingOrientationRadians{};
    math::q32_32 creationOrientationRadians{};
    bool flightDeck = false;
    bool hasIntermediate = false;
    bool valid = false;
};

struct HelicopterLandingGeometry final
{
    LogicFixedVec3 landing{};
    LogicFixedVec3 approach{};
    bool valid = false;
};

[[nodiscard]] JetParkingGeometry resolveJetParkingGeometry(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot* content,
    const ObjectAirfieldReservation& reservation,
    math::q32_32 parkingOffset) noexcept;
[[nodiscard]] HelicopterLandingGeometry resolveHelicopterLandingGeometry(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic* terrain, ObjectId airfield,
    ObjectId aircraft, math::q32_32 approachHeight) noexcept;
[[nodiscard]] bool setAirfieldHealee(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, bool add) noexcept;
void publishJetPosition(
    ecs::registry& registry, ecs::entity entity, LogicFixedVec3 position,
    std::optional<math::q32_32> yaw = std::nullopt) noexcept;
[[nodiscard]] bool advanceJetRoute(
    ecs::registry& registry, ecs::entity entity,
    ObjectJetAiRuntime& runtime, const ObjectSimulationRules& rules,
    bool taxiing) noexcept;
[[nodiscard]] bool advanceChinookFlightRoute(
    ecs::registry& registry, ecs::entity entity,
    ObjectChinookAiRuntime& runtime, const ObjectSimulationRules& rules)
    noexcept;
[[nodiscard]] bool isResumableJetOrder(
    const ObjectOrderIntent& order) noexcept;

bool purgeDeadSlots(const ecs::registry& registry,
                    const ObjectLifecycle& lifecycle,
                    container::Vector<ObjectId>& slots);
[[nodiscard]] std::optional<size_t> findSlot(
    const container::Vector<ObjectId>& slots, ObjectId object) noexcept;
[[nodiscard]] std::optional<size_t> findFreeSlot(
    const container::Vector<ObjectId>& slots) noexcept;
[[nodiscard]] ObjectAirfieldEvent makeSlotEvent(
    ObjectAirfieldEventKind kind, ObjectId airfield, ObjectId aircraft,
    ObjectAirfieldSlotKind slotKind, size_t moduleIndex, size_t slotIndex,
    uint32_t authoredOrder, container::String moduleClass,
    uint32_t slotCount, uint32_t runwayCount,
    uint64_t confirmedTick = 0);
[[nodiscard]] std::optional<size_t> parkingRunwayForSlot(
    const game::ObjectParkingPlaceRule& rule, size_t slotIndex) noexcept;
[[nodiscard]] std::optional<size_t> flightDeckRunwayForSlot(
    const game::ObjectFlightDeckRule& rule, size_t slotIndex) noexcept;
void rememberAircraftParkingReservation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft, const ObjectAirfieldReservation& reservation,
    uint64_t confirmedTick);
void rememberAircraftRunwayReservation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft, const ObjectAirfieldReservation& reservation);
void clearAircraftParkingReservation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick);
void clearAircraftRunwayReservation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft);
[[nodiscard]] ObjectAirfieldEvent makeRuntimeEvent(
    ObjectAirfieldEventKind kind, ObjectId object, uint32_t authoredOrder,
    container::String moduleClass, size_t moduleIndex,
    uint64_t confirmedTick,
    ObjectAircraftRuntimeState state = ObjectAircraftRuntimeState::Idle,
    ObjectJetAirfieldPhase phase = ObjectJetAirfieldPhase::Parked);
[[nodiscard]] ObjectAirfieldEvent makeSlowDeathEvent(
    ObjectId object, uint32_t sourcePathfindLayer,
    const game::ObjectAircraftSlowDeathRule& rule,
    ObjectAircraftSlowDeathPhase phase, uint64_t dueTick,
    uint64_t confirmedTick, container::String fx = {},
    container::String ocl = {}, container::String audio = {},
    container::String payloadTemplate = {});

} // namespace airfield_detail
} // namespace engine
