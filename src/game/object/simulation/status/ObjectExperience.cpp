#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/status/ObjectExperience.h"

#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace engine
{
namespace
{

constexpr size_t kVeterancyLevelCount = 4;
constexpr int64_t kFixedOneRaw = int64_t{1} << 32;
constexpr int64_t kMaximumScalarRaw = kMaximumExperienceScalarInteger * kFixedOneRaw;
constexpr int64_t kMinimumScalarRaw = -kMaximumScalarRaw;

[[nodiscard]] uint8_t levelIndex(game::ObjectVeterancyLevel level) noexcept
{
    return std::min<uint8_t>(static_cast<uint8_t>(level),
                             static_cast<uint8_t>(game::ObjectVeterancyLevel::Heroic));
}

using container::asciiEqualIgnoreCase;

[[nodiscard]] ObjectVeterancyComponent& ensureVeterancy(ecs::registry& registry,
                                                         ecs::entity entity)
{
    if (ObjectVeterancyComponent* existing =
            ecs::try_get<ObjectVeterancyComponent>(registry, entity))
    {
        return *existing;
    }
    return ecs::emplace<ObjectVeterancyComponent>(registry, entity);
}

[[nodiscard]] const game::ThingTemplate* objectTemplate(const ecs::registry& registry,
                                                        ecs::entity entity) noexcept
{
    const ThingTemplateComponent* component =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return component && component->archetype
               ? &component->archetype->templateData
               : nullptr;
}

[[nodiscard]] game::ObjectVeterancyLevel levelForPoints(
    int64_t points,
    const game::ThingTemplate& templateData) noexcept
{
    uint8_t index = 0;
    while (index + 1 < kVeterancyLevelCount &&
           points >= templateData.experienceRequired[index + 1])
    {
        ++index;
    }
    return static_cast<game::ObjectVeterancyLevel>(index);
}

[[nodiscard]] int64_t saturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

[[nodiscard]] int64_t saturatingDifference(int64_t value,
                                           int64_t baseline) noexcept
{
    if (baseline > 0 && value < std::numeric_limits<int64_t>::min() + baseline)
        return std::numeric_limits<int64_t>::min();
    if (baseline < 0 && value > std::numeric_limits<int64_t>::max() + baseline)
        return std::numeric_limits<int64_t>::max();
    return value - baseline;
}

[[nodiscard]] int64_t scalePoints(int32_t points, math::q32_32 scalar) noexcept
{
    // addScalar() bounds the integer portion to +/-32768. Multiply the Int
    // and Q32.32 raw value as an unsigned magnitude so the intermediate may
    // use all 96 required bits without ever invoking q32_32's wrapping
    // destination operator. The final division by 2^32 truncates toward zero,
    // matching RefCode's compound Int *= Real conversion.
    const int64_t scalarRaw = scalar.raw();
    const bool negative = (points < 0) != (scalarRaw < 0);
    const uint64_t pointMagnitude = points < 0
        ? static_cast<uint64_t>(-static_cast<int64_t>(points))
        : static_cast<uint64_t>(points);
    const uint64_t scalarMagnitude = scalarRaw < 0
        ? static_cast<uint64_t>(-scalarRaw)
        : static_cast<uint64_t>(scalarRaw);
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t high = 0;
    const uint64_t low = _umul128(pointMagnitude, scalarMagnitude, &high);
    const uint64_t magnitude = (high << 32) | (low >> 32);
#else
    const unsigned __int128 product =
        static_cast<unsigned __int128>(pointMagnitude) *
        static_cast<unsigned __int128>(scalarMagnitude);
    const uint64_t magnitude = static_cast<uint64_t>(product >> 32);
#endif
    return negative ? -static_cast<int64_t>(magnitude)
                    : static_cast<int64_t>(magnitude);
}

void bumpRevision(ecs::registry& registry, ecs::entity entity,
                  ObjectExperienceComponent& experience,
                  uint64_t confirmedTick) noexcept
{
    if (experience.revision != std::numeric_limits<uint64_t>::max())
        ++experience.revision;
    experience.lastChangedTick = confirmedTick;
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
}

void addModelConditions(RenderModelComponent& visual,
                        const game::ModelConditionMask& mask) noexcept
{
    for (size_t index = 0; index < visual.modelConditionFlags.words.size(); ++index)
        visual.modelConditionFlags.words[index] |= mask.words[index];
}

[[nodiscard]] game::WeaponSetConditionMask weaponConditionFor(
    game::ObjectVeterancyLevel level) noexcept
{
    switch (level)
    {
    case game::ObjectVeterancyLevel::Veteran:
        return game::weaponSetConditionBit(game::WeaponSetCondition::Veteran);
    case game::ObjectVeterancyLevel::Elite:
        return game::weaponSetConditionBit(game::WeaponSetCondition::Elite);
    case game::ObjectVeterancyLevel::Heroic:
        return game::weaponSetConditionBit(game::WeaponSetCondition::Hero);
    case game::ObjectVeterancyLevel::Regular:
        return 0;
    }
    return 0;
}

[[nodiscard]] game::ArmorSetConditionMask armorConditionFor(
    game::ObjectVeterancyLevel level) noexcept
{
    switch (level)
    {
    case game::ObjectVeterancyLevel::Veteran:
        return game::armorSetConditionBit(game::ArmorSetCondition::Veteran);
    case game::ObjectVeterancyLevel::Elite:
        return game::armorSetConditionBit(game::ArmorSetCondition::Elite);
    case game::ObjectVeterancyLevel::Heroic:
        return game::armorSetConditionBit(game::ArmorSetCondition::Hero);
    case game::ObjectVeterancyLevel::Regular:
        return 0;
    }
    return 0;
}

} // namespace

void ObjectExperienceSystem::initializeObject(ecs::registry& registry,
                                              ecs::entity entity,
                                              const game::ThingTemplate& templateData,
                                              uint64_t confirmedTick) const
{
    if (entity == ecs::null || !registry.valid(entity))
        return;
    ObjectExperienceComponent value{
        .trainable = templateData.isTrainable,
        .lastChangedTick = confirmedTick,
    };
    if (ObjectExperienceComponent* existing =
            ecs::try_get<ObjectExperienceComponent>(registry, entity))
    {
        *existing = value;
    }
    else
    {
        ecs::emplace<ObjectExperienceComponent>(registry, entity, value);
    }
    ObjectVeterancyComponent& veterancy = ensureVeterancy(registry, entity);
    veterancy.level = game::ObjectVeterancyLevel::Regular;
}

ObjectExperienceMutation ObjectExperienceSystem::addPoints(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    int32_t points,
    bool canScaleForBonus,
    uint64_t confirmedTick) const
{
    return addPointsImpl(registry, lifecycle, object, points,
                         canScaleForBonus, confirmedTick, false);
}

ObjectExperienceMutation
ObjectExperienceSystem::addPointsIncludingPendingDestroy(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    int32_t points,
    bool canScaleForBonus,
    uint64_t confirmedTick) const
{
    return addPointsImpl(registry, lifecycle, object, points,
                         canScaleForBonus, confirmedTick, true);
}

ObjectExperienceMutation ObjectExperienceSystem::addPointsImpl(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    int32_t points,
    bool canScaleForBonus,
    uint64_t confirmedTick,
    bool includePendingDestroy) const
{
    ObjectExperienceMutation result{.requestedObject = object, .receiver = object};
    if (!object)
        return result;

    const auto resolveEntity = [&lifecycle, includePendingDestroy](
                                   ObjectId candidate) {
        return includePendingDestroy
                   ? lifecycle.entityFromIdIncludingPending(candidate)
                   : lifecycle.entityFromId(candidate);
    };

    container::Array<ObjectId, 64> visited{};
    size_t visitedCount = 0;
    ObjectId receiver = object;
    int64_t routedPoints = points;
    while (receiver && visitedCount < visited.size())
    {
        if (std::find(visited.begin(), visited.begin() +
                      static_cast<std::ptrdiff_t>(visitedCount), receiver) !=
            visited.begin() + static_cast<std::ptrdiff_t>(visitedCount))
        {
            return result;
        }
        visited[visitedCount++] = receiver;
        const std::optional<ecs::entity> entity = resolveEntity(receiver);
        if (!entity)
            return result;
        ObjectExperienceComponent* experience =
            ecs::try_get<ObjectExperienceComponent>(registry, *entity);
        if (!experience)
            return result;
        if (!experience->sink)
            break;
        const std::optional<ecs::entity> sinkEntity =
            resolveEntity(experience->sink);
        if (!sinkEntity ||
            !ecs::try_get<ObjectExperienceComponent>(registry, *sinkEntity))
        {
            // RefCode falls back to the source tracker when its loose sink ID
            // no longer resolves.
            break;
        }
        // Forward the RAW amount along the sink chain.  RefCode's
        // ExperienceTracker::addExperiencePoints hands the unmodified value to
        // its sink and lets only the final receiver apply its own scalar, and
        // only when canScaleForBonus is set.  Scaling per hop here meant a
        // drone that sinks XP to its parent while holding an
        // ExperienceScalarUpgrade double-scaled the award — and scaled even
        // awards that are explicitly not scalable — so units reached veterancy
        // at the wrong kill counts.
        receiver = experience->sink;
        result.routedToSink = true;
    }
    if (visitedCount == visited.size())
        return result;

    const std::optional<ecs::entity> entity = resolveEntity(receiver);
    if (!entity)
        return result;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, *entity);
    ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, *entity);
    const game::ThingTemplate* templateData = objectTemplate(registry, *entity);
    if (!experience || !veterancy || !templateData || !experience->trainable)
        return result;

    result.receiver = receiver;
    result.previousPoints = experience->currentPoints;
    result.previousLevel = veterancy->level;
    int64_t applied = routedPoints;
    if (canScaleForBonus)
    {
        applied = scalePoints(
            static_cast<int32_t>(std::clamp<int64_t>(
                applied,
                std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::max())),
            experience->gainScalar);
    }
    experience->currentPoints = saturatingAdd(experience->currentPoints, applied);
    veterancy->level = levelForPoints(experience->currentPoints, *templateData);

    result.currentPoints = experience->currentPoints;
    result.currentLevel = veterancy->level;
    result.appliedPoints = saturatingDifference(
        experience->currentPoints, result.previousPoints);
    result.pointsChanged = result.currentPoints != result.previousPoints;
    result.levelChanged = result.currentLevel != result.previousLevel;
    result.accepted = true;
    if (result.pointsChanged || result.levelChanged)
        bumpRevision(registry, *entity, *experience, confirmedTick);
    return result;
}

ObjectExperienceMutation ObjectExperienceSystem::setLevel(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    game::ObjectVeterancyLevel level,
    uint64_t confirmedTick) const
{
    ObjectExperienceMutation result{.requestedObject = object, .receiver = object};
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return result;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, *entity);
    ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, *entity);
    const game::ThingTemplate* templateData = objectTemplate(registry, *entity);
    if (!experience || !veterancy || !templateData)
        return result;
    const uint8_t index = levelIndex(level);
    level = static_cast<game::ObjectVeterancyLevel>(index);
    result.previousPoints = experience->currentPoints;
    result.previousLevel = veterancy->level;
    if (veterancy->level == level)
    {
        result.currentPoints = result.previousPoints;
        result.currentLevel = result.previousLevel;
        result.accepted = true;
        return result;
    }

    experience->currentPoints = templateData->experienceRequired[index];
    veterancy->level = level;
    result.currentPoints = experience->currentPoints;
    result.currentLevel = level;
    result.appliedPoints = saturatingDifference(
        result.currentPoints, result.previousPoints);
    result.pointsChanged = result.currentPoints != result.previousPoints;
    result.levelChanged = true;
    result.accepted = true;
    bumpRevision(registry, *entity, *experience, confirmedTick);
    return result;
}

ObjectExperienceMutation ObjectExperienceSystem::setMinimumLevel(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    game::ObjectVeterancyLevel level,
    uint64_t confirmedTick) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    const ObjectVeterancyComponent* veterancy = entity
        ? ecs::try_get<ObjectVeterancyComponent>(registry, *entity)
        : nullptr;
    if (!veterancy || veterancy->level >= level)
    {
        ObjectExperienceMutation result{.requestedObject = object, .receiver = object};
        if (veterancy)
        {
            result.accepted = true;
            result.previousLevel = veterancy->level;
            result.currentLevel = veterancy->level;
            if (const ObjectExperienceComponent* experience =
                    ecs::try_get<ObjectExperienceComponent>(registry, *entity))
            {
                result.previousPoints = experience->currentPoints;
                result.currentPoints = experience->currentPoints;
            }
        }
        return result;
    }
    return setLevel(registry, lifecycle, object, level, confirmedTick);
}

bool ObjectExperienceSystem::resetPointsAndLevel(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    uint64_t confirmedTick) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, *entity);
    ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, *entity);
    if (!experience || !veterancy)
        return false;
    if (experience->currentPoints == 0 &&
        veterancy->level == game::ObjectVeterancyLevel::Regular)
        return true;
    experience->currentPoints = 0;
    veterancy->level = game::ObjectVeterancyLevel::Regular;
    bumpRevision(registry, *entity, *experience, confirmedTick);
    return true;
}

bool ObjectExperienceSystem::setTrainable(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    bool trainable,
    uint64_t confirmedTick) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, *entity);
    if (!experience)
        return false;
    if (experience->trainable == trainable)
        return true;
    experience->trainable = trainable;
    bumpRevision(registry, *entity, *experience, confirmedTick);
    return true;
}

bool ObjectExperienceSystem::setSink(ecs::registry& registry,
                                     const ObjectLifecycle& lifecycle,
                                     ObjectId object,
                                     ObjectId sink,
                                     uint64_t confirmedTick) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || sink == object)
        return false;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, *entity);
    if (!experience || experience->sink == sink)
        return false;
    experience->sink = sink;
    bumpRevision(registry, *entity, *experience, confirmedTick);
    return true;
}

bool ObjectExperienceSystem::addScalar(ecs::registry& registry,
                                       ecs::entity entity,
                                       math::q32_32 delta,
                                       uint64_t confirmedTick) const noexcept
{
    if (entity == ecs::null || !registry.valid(entity))
        return false;
    ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, entity);
    if (!experience || delta.raw() == 0)
        return false;
    const int64_t deltaRaw = std::clamp(
        delta.raw(), kMinimumScalarRaw, kMaximumScalarRaw);
    int64_t next = experience->gainScalar.raw();
    if (deltaRaw > 0 && next > kMaximumScalarRaw - deltaRaw)
        next = kMaximumScalarRaw;
    else if (deltaRaw < 0 && next < kMinimumScalarRaw - deltaRaw)
        next = kMinimumScalarRaw;
    else
        next += deltaRaw;
    next = std::clamp(next, kMinimumScalarRaw, kMaximumScalarRaw);
    if (next == experience->gainScalar.raw())
        return false;
    experience->gainScalar = math::q32_32::from_raw(next);
    bumpRevision(registry, entity, *experience, confirmedTick);
    return true;
}

int32_t ObjectExperienceSystem::experienceValue(const ecs::registry& registry,
                                                ecs::entity entity) const noexcept
{
    const game::ThingTemplate* templateData = objectTemplate(registry, entity);
    const ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, entity);
    if (!templateData || !veterancy)
        return 0;
    return templateData->experienceValue[levelIndex(veterancy->level)];
}

void ObjectExperienceSystem::projectVeterancy(
    ecs::registry& registry,
    ecs::entity entity,
    game::ObjectVeterancyLevel oldLevel,
    game::ObjectVeterancyLevel newLevel,
    const ObjectSimulationRules& rules,
    const GameContentSnapshot* content,
    SimulationRandom* random,
    uint64_t confirmedTick) const
{
    if (entity == ecs::null || !registry.valid(entity) || oldLevel == newLevel)
        return;

    if (ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity))
    {
        RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity);
        static_cast<void>(scaleObjectMaximumHealthPreserveRatio(
            *health,
            rules.veterancy.healthBonusForLevelIndex(levelIndex(newLevel)),
            rules.veterancy.healthBonusForLevelIndex(levelIndex(oldLevel)),
            rules));
        if (visual) {
            projectObjectBodyDamageVisual(
                registry, entity, health->damageState, *visual);
        }
    }

    constexpr game::ArmorSetConditionMask kVeterancyArmor =
        game::armorSetConditionBit(game::ArmorSetCondition::Veteran) |
        game::armorSetConditionBit(game::ArmorSetCondition::Elite) |
        game::armorSetConditionBit(game::ArmorSetCondition::Hero);
    constexpr game::WeaponSetConditionMask kVeterancyWeapons =
        game::weaponSetConditionBit(game::WeaponSetCondition::Veteran) |
        game::weaponSetConditionBit(game::WeaponSetCondition::Elite) |
        game::weaponSetConditionBit(game::WeaponSetCondition::Hero);
    if (ObjectCombatProfileComponent* combat =
            ecs::try_get<ObjectCombatProfileComponent>(registry, entity))
    {
        combat->armorConditions =
            (combat->armorConditions & ~kVeterancyArmor) | armorConditionFor(newLevel);
        combat->weaponConditions =
            (combat->weaponConditions & ~kVeterancyWeapons) | weaponConditionFor(newLevel);
        if (ObjectArmorComponent* armor =
                ecs::try_get<ObjectArmorComponent>(registry, entity))
        {
            refreshResolvedObjectArmor(*combat, *armor);
        }
    }
    if (content)
        static_cast<void>(refreshObjectWeaponSet(
            registry, entity, *content, rules.logicFramesPerSecond,
            confirmedTick));

    static const game::ModelConditionMask veteranModel =
        game::modelConditionMaskOf(game::ModelConditionFlag::WeaponsetVeteran);
    static const game::ModelConditionMask eliteModel =
        game::modelConditionMaskOf(game::ModelConditionFlag::WeaponsetElite);
    static const game::ModelConditionMask heroModel =
        game::modelConditionMaskOf(game::ModelConditionFlag::WeaponsetHero);
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity))
    {
        game::ModelConditionMask all = veteranModel;
        for (size_t index = 0; index < all.words.size(); ++index)
            all.words[index] |= eliteModel.words[index] | heroModel.words[index];
        visual->modelConditionFlags.clear(all);
        switch (newLevel)
        {
        case game::ObjectVeterancyLevel::Veteran:
            addModelConditions(*visual, veteranModel);
            break;
        case game::ObjectVeterancyLevel::Elite:
            addModelConditions(*visual, eliteModel);
            break;
        case game::ObjectVeterancyLevel::Heroic:
            addModelConditions(*visual, heroModel);
            break;
        case game::ObjectVeterancyLevel::Regular:
            break;
        }
    }

    // Preserve the source call order. Veteran->Elite and Elite->Heroic each
    // perform two real mask transitions in RefCode, and each transition may
    // refresh a running weapon timer through the existing deterministic API.
    switch (newLevel)
    {
    case game::ObjectVeterancyLevel::Regular:
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Veteran, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Elite, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Hero, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        break;
    case game::ObjectVeterancyLevel::Veteran:
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Veteran, true,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Elite, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Hero, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        break;
    case game::ObjectVeterancyLevel::Elite:
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Veteran, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Elite, true,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Hero, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        break;
    case game::ObjectVeterancyLevel::Heroic:
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Veteran, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Elite, false,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::Hero, true,
            content, random, rules.logicFramesPerSecond, confirmedTick));
        break;
    }
}

std::optional<game::ObjectVeterancyLevel>
ObjectExperienceSystem::parseLevel(container::StringView value) noexcept
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    if (asciiEqualIgnoreCase(value, "REGULAR"))
        return game::ObjectVeterancyLevel::Regular;
    if (asciiEqualIgnoreCase(value, "VETERAN"))
        return game::ObjectVeterancyLevel::Veteran;
    if (asciiEqualIgnoreCase(value, "ELITE"))
        return game::ObjectVeterancyLevel::Elite;
    if (asciiEqualIgnoreCase(value, "HEROIC") || asciiEqualIgnoreCase(value, "HERO"))
        return game::ObjectVeterancyLevel::Heroic;
    return std::nullopt;
}

} // namespace engine
