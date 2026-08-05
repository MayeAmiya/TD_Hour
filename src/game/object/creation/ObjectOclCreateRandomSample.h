#pragma once

#include "core/container/container_types.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>

namespace engine {

class SimulationRandom;

// Immutable capabilities resolved from the active content snapshot before an
// OCL attempts to materialize a selected model. They keep RNG admission free
// of live ECS queries and make failed/unsupported candidates explicit.
struct ObjectOclCreateCandidateTraits final {
    bool available = false;
    bool hasPhysics = false;
    bool hasLifetimeUpdate = false;
};

struct ObjectOclCreateFixedVector final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

// One frozen random transaction for one GenericObjectCreationNugget result.
// GameSession consumes this value after spawn without touching the session RNG
// again, so creation callbacks and containment cannot perturb the nugget's
// remaining legacy-authored samples.
struct ObjectOclCreateRandomSample final {
    size_t modelIndex = 0;
    size_t animationSetIndex = 0;
    math::q32_32 spreadMinimumRadius{};
    math::q32_32 spreadStartAngleRadians{};
    math::q32_32 healthFraction{int32_t{1}};
    math::q32_32 onGroundOrientationRadians{};
    math::q32_32 sendOutOrientationRadians{};
    ObjectOclCreateFixedVector sendOutForce;
    math::q32_32 flightYawRate{};
    math::q32_32 flightRollRate{};
    math::q32_32 flightPitchRate{};
    ObjectOclCreateFixedVector flightForce;
    math::q32_32 whirlingYawRate{};
    math::q32_32 whirlingRollRate{};
    math::q32_32 whirlingPitchRate{};
    uint32_t lifetimeFrames = 0;
    bool candidateAvailable = false;
    bool hasPhysics = false;
    bool hasLifetimeUpdate = false;
    bool hasSpread = false;
    bool hasAnimationSet = false;
    bool hasOnGroundOrientation = false;
    bool hasSendOutOrientation = false;
    bool hasSendOutForce = false;
    bool hasFlightForce = false;
    bool hasWhirlingRates = false;
};

struct ObjectOclCreateRandomSampleRequest final {
    const game::ObjectCreationGenericFields* common = nullptr;
    container::Span<const ObjectOclCreateCandidateTraits> candidates;
    size_t animationSetCount = 0;
    uint32_t lifetimeOverrideFrames = 0;
    uint32_t logicFramesPerSecond = 30;
    // Wrappers run common doStuffToObj but are not selected from ObjectNames
    // and do not enter the payload-only SpreadFormation branch.
    bool chooseModel = true;
    bool allowSpread = true;
    bool allowDebrisAnimation = false;
};

[[nodiscard]] ObjectOclCreateRandomSample sampleObjectOclCreateRandom(
    const ObjectOclCreateRandomSampleRequest& request,
    SimulationRandom& random) noexcept;

} // namespace engine
