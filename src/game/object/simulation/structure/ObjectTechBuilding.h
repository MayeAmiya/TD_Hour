#pragma once

#include <cstdint>

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include "game/object/plan/structure/ObjectTechBuildingPlanTypes.h"
namespace engine
{

class ObjectLifecycle;
class PlayerRegistry;
struct ObjectSimulationRules;
struct ObjectOwnershipChangeRequest;

enum class ObjectTechBuildingEventKind : uint8_t
{
    CapturedStateChanged,
    PulseFx,
};

struct ObjectTechBuildingEvent final
{
    ObjectTechBuildingEventKind kind = ObjectTechBuildingEventKind::CapturedStateChanged;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    container::String fxList;
    bool captured = false;
    uint64_t confirmedTick = 0;
};

enum class ObjectBeaconClientEventKind : uint8_t
{
    ShowSmoke,
    Hide,
    RadarPulse,
};

struct ObjectBeaconClientEvent final
{
    ObjectBeaconClientEventKind kind = ObjectBeaconClientEventKind::ShowSmoke;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    uint32_t indicatorColorRgb = 0xffffffu;
    uint32_t radarPulseDurationMilliseconds = 0;
    LogicFixedVec3 position{};
    uint64_t confirmedTick = 0;
};

struct ObjectTechBuildingRuntime final
{
    bool captured = false;
    bool deathReleased = false;
    uint64_t nextPulseTick = 0;
};

struct ObjectBeaconClientRuntime final
{
    bool smokeStarted = false;
    uint64_t nextRadarPulseTick = 0;
};

struct ObjectTechBuildingComponent final
{
    container::SharedPtr<const game::ObjectTechBuildingPlan> plan;
    container::Vector<ObjectTechBuildingRuntime> techBuildings;
    container::Vector<ObjectBeaconClientRuntime> beacons;
};

class ObjectTechBuildingSystem final
{
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity, const ObjectSimulationRules& rules) const;

    void onObjectOwnerChanged(ecs::registry& registry,
                              const ObjectLifecycle& lifecycle,
                              const PlayerRegistry& players,
                              ObjectId object,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick,
                              container::Vector<ObjectTechBuildingEvent>& outTechEvents,
                              container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const;

    void onObjectReclaim(ecs::registry& registry,
                         const ObjectLifecycle& lifecycle,
                         ObjectId object,
                         uint64_t confirmedTick,
                         container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const;
    [[nodiscard]] bool onDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal,
        container::Vector<ObjectOwnershipChangeRequest>& ownership) const;

    void update(ecs::registry& registry,
                const ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectTechBuildingEvent>& outTechEvents,
                container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const;
};

} // namespace engine
