#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectExperienceLimits.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>
namespace game
{
struct ThingTemplate;
}

namespace engine
{

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;
struct ObjectSimulationRules;

// Mutable counterpart to RefCode ExperienceTracker. Veterancy remains a
// separate, compact projection because DieMux already consumes it directly;
// ObjectExperienceSystem is the sole normal writer of both components.
struct ObjectExperienceComponent final
{
    int64_t currentPoints = 0;
    math::q32_32 gainScalar{int32_t{1}};
    ObjectId sink = INVALID_OBJECT_ID;
    bool trainable = false;
    uint64_t revision = 0;
    uint64_t lastChangedTick = 0;
};

struct ObjectExperienceMutation final
{
    ObjectId requestedObject = INVALID_OBJECT_ID;
    ObjectId receiver = INVALID_OBJECT_ID;
    int64_t previousPoints = 0;
    int64_t currentPoints = 0;
    int64_t appliedPoints = 0;
    game::ObjectVeterancyLevel previousLevel = game::ObjectVeterancyLevel::Regular;
    game::ObjectVeterancyLevel currentLevel = game::ObjectVeterancyLevel::Regular;
    bool accepted = false;
    bool pointsChanged = false;
    bool levelChanged = false;
    bool routedToSink = false;
};

enum class ObjectExperienceEventKind : uint8_t
{
    PointsChanged,
    VeterancyChanged,
};

struct ObjectExperienceEvent final
{
    ObjectExperienceEventKind kind = ObjectExperienceEventKind::PointsChanged;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    int64_t previousPoints = 0;
    int64_t currentPoints = 0;
    int64_t appliedPoints = 0;
    game::ObjectVeterancyLevel previousLevel = game::ObjectVeterancyLevel::Regular;
    game::ObjectVeterancyLevel currentLevel = game::ObjectVeterancyLevel::Regular;
    uint64_t confirmedTick = 0;
    // VeterancyGainCreate deliberately suppresses level-up feedback while
    // still applying every authoritative health/armor/weapon projection.
    bool provideFeedback = true;
};

class ObjectExperienceSystem final
{
public:
    void initializeObject(ecs::registry& registry,
                          ecs::entity entity,
                          const game::ThingTemplate& templateData,
                          uint64_t confirmedTick) const;

    [[nodiscard]] ObjectExperienceMutation addPoints(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        int32_t points,
        bool canScaleForBonus,
        uint64_t confirmedTick) const;

    // RefCode keeps a destroyed Object addressable until GameLogic performs
    // its deferred-delete flush. This variant is reserved for a lethal-damage
    // callback already admitted to the same deterministic batch; normal XP
    // producers must use addPoints() and therefore reject PendingDestroy.
    [[nodiscard]] ObjectExperienceMutation addPointsIncludingPendingDestroy(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        int32_t points,
        bool canScaleForBonus,
        uint64_t confirmedTick) const;

    [[nodiscard]] ObjectExperienceMutation setLevel(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        game::ObjectVeterancyLevel level,
        uint64_t confirmedTick) const;

    [[nodiscard]] ObjectExperienceMutation setMinimumLevel(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        game::ObjectVeterancyLevel level,
        uint64_t confirmedTick) const;

    // RiderChangeContain transfers the rider's rank to the vehicle and then
    // performs RefCode's setExperienceAndLevel(0).  This is intentionally a
    // distinct operation from setLevel(Regular): an already-Regular object
    // may still hold partial experience which must be cleared.
    [[nodiscard]] bool resetPointsAndLevel(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool setTrainable(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        bool trainable,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool setSink(ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ObjectId object,
                               ObjectId sink,
                               uint64_t confirmedTick) const;

    [[nodiscard]] bool addScalar(ecs::registry& registry,
                                 ecs::entity entity,
                                 math::q32_32 delta,
                                 uint64_t confirmedTick) const noexcept;

    [[nodiscard]] int32_t experienceValue(const ecs::registry& registry,
                                          ecs::entity entity) const noexcept;

    // Applies the Object-side part of onVeterancyLevelChanged after the
    // synthetic Object upgrade has been granted by ObjectSimulation.
    void projectVeterancy(ecs::registry& registry,
                          ecs::entity entity,
                          game::ObjectVeterancyLevel oldLevel,
                          game::ObjectVeterancyLevel newLevel,
                          const ObjectSimulationRules& rules,
                          const GameContentSnapshot* content,
                          SimulationRandom* random,
                          uint64_t confirmedTick) const;

    [[nodiscard]] static std::optional<game::ObjectVeterancyLevel>
    parseLevel(container::StringView value) noexcept;

private:
    [[nodiscard]] ObjectExperienceMutation addPointsImpl(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        ObjectId object,
        int32_t points,
        bool canScaleForBonus,
        uint64_t confirmedTick,
        bool includePendingDestroy) const;
};

} // namespace engine
