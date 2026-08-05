#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace game {

struct ThingTemplate;

struct ObjectBridgeTimedResource final {
    container::String resource;
    uint32_t delayMilliseconds = 0;
    container::String bone;
    uint32_t authoredOrder = 0;
};

struct ObjectBridgeBehaviorRule final {
    math::q32_32 lateralScaffoldSpeed{int32_t{1}};
    math::q32_32 verticalScaffoldSpeed{int32_t{1}};
    container::Vector<ObjectBridgeTimedResource> dieFx;
    container::Vector<ObjectBridgeTimedResource> dieOcl;
    uint32_t authoredOrder = 0;
};

struct ObjectBridgeScaffoldRule final {
    // The authored BridgeScaffoldBehavior has no fields. Motion endpoints and
    // bridge-derived speeds belong to the immutable creation request produced
    // from TerrainRoad/BridgeInfo, not to this template rule.
    uint32_t authoredOrder = 0;
};

struct ObjectBridgeTowerRule final {
    uint32_t authoredOrder = 0;
};

struct ObjectRailroadRule final {
    container::String pathPrefixName;
    container::String crashFxTemplateName;
    container::Vector<container::String> carriageTemplateNames;
    container::String bigMetalBounceSound;
    container::String smallMetalBounceSound;
    container::String meatyBounceSound;
    container::String runningSound;
    container::String clicketyClackSound;
    container::String whistleSound;
    math::q32_32 runningGarrisonSpeedMax{int32_t{1}};
    math::q32_32 killSpeedMin{int32_t{1}};
    math::q32_32 speedMax{int32_t{4}};
    math::q32_32 acceleration = math::q32_32::from_fraction(101, 100);
    math::q32_32 braking = math::q32_32::from_fraction(99, 100);
    math::q32_32 friction = math::q32_32::from_fraction(97, 100);
    uint32_t waitAtStationMilliseconds = 5000;
    bool isLocomotive = false;
    uint32_t authoredOrder = 0;
};

struct ObjectRailedTransportContainRule final {
    uint32_t authoredOrder = 0;
};

struct ObjectRailedTransportDockRule final {
    uint32_t pullInsideDurationMilliseconds = 0;
    uint32_t pushOutsideDurationMilliseconds = 0;
    math::q32_32 toleranceDistance{int32_t{50}};
    uint32_t authoredOrder = 0;
};

struct ObjectRailedTransportAiRule final {
    container::String pathPrefixName;
    uint32_t authoredOrder = 0;
};

struct ObjectBridgeRailPlan final {
    container::Vector<ObjectBridgeBehaviorRule> bridges;
    container::Vector<ObjectBridgeScaffoldRule> scaffolds;
    container::Vector<ObjectBridgeTowerRule> towers;
    container::Vector<ObjectRailroadRule> railroads;
    container::Vector<ObjectRailedTransportContainRule> railedTransportContains;
    container::Vector<ObjectRailedTransportDockRule> railedTransportDocks;
    container::Vector<ObjectRailedTransportAiRule> railedTransportAi;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectBridgeRailPlan>
compileObjectBridgeRailPlan(const ThingTemplate& templateData);

} // namespace game
