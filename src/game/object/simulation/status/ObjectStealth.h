#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "core/ecs/registry.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

#include "game/object/plan/status/ObjectStealthPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;
struct ObjectSimulationRules;

struct ObjectStealthComponent final {
    container::SharedPtr<const game::ObjectStealthPlan> plan;
    uint64_t stealthAllowedTick = 0;
    uint64_t detectionExpiresTick = 0;
    uint64_t nextBlackMarketCheckTick = 0;
    uint64_t temporaryGrantExpiresTick = 0;
    uint64_t temporaryGrantObservedExternalOrderRevision = 0;
    // Drawable-local presentation state retained on the confirmed object.
    // Detector scans reset the second material pass to one; the normal
    // 30 Hz update decays it by the retail 0.8 scalar unless FRENZY freezes
    // the pass.  Keeping the value per object avoids the old global
    // frame-modulo pulse that synchronized every detected unit.
    math::q32_32 heatVisionOpacity{};
    // Retail seeds each StealthUpdate pulse independently and advances it by
    // 0.2 radians per active update.  This field is presentation-only, but it
    // lives beside the confirmed stealth state so pause/replay do not consult
    // render wall time.
    math::q32_32 friendlyPulsePhaseRadians{};
    bool enabled = true;
    bool blackMarketAvailable = false;
};

// Disguise is intentionally separate from ordinary stealth.  The requested
// identity changes immediately, while the apparent identity changes only at
// the authored transition midpoint.  Render extraction may consume the
// apparent values without replacing the object's real ThingTemplateComponent
// or consulting the mutable target object.
enum class ObjectDisguisePhase : uint8_t {
    None,
    Disguising,
    Disguised,
    Revealing,
};

struct ObjectDisguiseComponent final {
    ObjectId sourceTarget = INVALID_OBJECT_ID;
    PlayerId requestedPlayer = INVALID_PLAYER_ID;
    container::String requestedTemplateName;
    PlayerId apparentPlayer = INVALID_PLAYER_ID;
    container::String apparentTemplateName;
    uint64_t transitionStartedTick = 0;
    uint64_t transitionMidpointTick = 0;
    uint64_t transitionCompleteTick = 0;
    ObjectDisguisePhase phase = ObjectDisguisePhase::None;
    bool transitionMidpointApplied = false;
};

// RefCode PartitionFilterStealthedAndUndetected acquisition semantics. This
// is shared by idle/guard AI and CommandButtonHunt so disguised units and
// stealth-garrisoned neutral containers cannot produce contradictory target
// visibility in separate AI paths.
[[nodiscard]] bool objectHiddenFromObserverForAcquisition(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity observer,
    ecs::entity target) noexcept;

// Produced by markDetected when the authored module asks idle visible enemies
// to wake and acquire the revealed object.  The AI/visibility boundary owns
// relationship and line-of-sight filtering; Stealth owns only this stable,
// replayable request and never writes another object's order queue directly.
struct ObjectStealthRevealOrderRequestComponent final {
    uint64_t requestedTick = 0;
    uint64_t revision = 0;
};

struct ObjectStealthDetectorComponent final {
    container::SharedPtr<const game::ObjectStealthDetectorPlan> plan;
    uint64_t nextScanTick = 0;
    bool enabled = true;
};

struct ObjectGrantStealthComponent final {
    container::SharedPtr<const game::ObjectGrantStealthPlan> plan;
    math::q32_32 currentRadius{};
    bool presentationStarted = false;
};

// One detector pulse is kept as one value event so presentation strings are
// copied once, not once per revealed target. Target IDs are ObjectId-sorted.
// Simulation already committed DETECTED before this crosses the boundary;
// clients may independently suppress sounds/particles hidden by local shroud.
struct ObjectStealthDetectorPulseEvent final {
    ObjectId detector = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    container::Vector<ObjectId> detectedTargets;
    container::Vector<ObjectId> gridTargets;
    container::Vector<ObjectId> newlyDetectedTargets;
    container::String pingSound;
    container::String particleSystem;
    container::String beaconParticleSystem;
    container::String gridParticleSystem;
    container::String particleBone;
    uint64_t confirmedTick = 0;
};

struct ObjectGrantStealthPulseEvent final {
    ObjectId grantor = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    container::Vector<ObjectId> grantedTargets;
    container::String radiusParticleSystem;
    uint32_t particleLifetimeFrames = 0;
    math::q32_32 currentRadius{};
    bool finalScan = false;
    uint64_t confirmedTick = 0;
};

enum class ObjectDisguisePresentationEventKind : uint8_t {
    DisguiseStarted,
    DisguiseRevealedSuccess,
    DisguiseRevealedFailure,
};

struct ObjectDisguisePresentationEvent final {
    ObjectDisguisePresentationEventKind kind =
        ObjectDisguisePresentationEventKind::DisguiseStarted;
    ObjectId object = INVALID_OBJECT_ID;
    container::String fxList;
    uint64_t confirmedTick = 0;
};

class ObjectStealthSystem final {
public:
    void initializeObject(
        ecs::registry& registry, ecs::entity entity,
        const ObjectSimulationRules& rules, uint64_t sessionSeed,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool markDetected(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint32_t frames, const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool receiveGrant(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, bool active, uint32_t frames,
        uint64_t confirmedTick) const;

    // A valid target copies its current requested disguise when it has one,
    // otherwise its immutable real template and controlling player. Passing
    // INVALID_OBJECT_ID begins the authored reveal transition. Target death
    // after this call cannot invalidate the copied value identity.
    [[nodiscard]] bool disguiseAsObject(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId target, const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool setDetectorEnabled(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, bool enabled, uint64_t confirmedTick) const;

    void updateDetectors(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const ObjectSpatialIndex& spatialIndex,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectStealthDetectorPulseEvent>& events) const;

    void updateGrantors(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const ObjectSpatialIndex& spatialIndex,
        uint64_t confirmedTick,
        container::Vector<ObjectGrantStealthPulseEvent>& events) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, uint64_t confirmedTick) const;

    [[nodiscard]] container::Vector<ObjectDisguisePresentationEvent>
    takeDisguisePresentationEvents();
    void resetPresentationEvents() noexcept;

private:
    mutable container::Vector<ObjectDisguisePresentationEvent>
        m_disguisePresentationEvents;
};

} // namespace engine
