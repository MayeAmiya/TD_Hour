#pragma once
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <iterator>
#include <optional>

namespace engine::object_lifecycle_detail {

[[nodiscard]] inline bool hasObjectKind(const ObjectKindOfComponent* kinds,
                                 game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] inline std::optional<PlayerScoredObjectKind>
builtScoreKind(const ObjectKindOfComponent* kinds) noexcept {
    if (!hasObjectKind(kinds, game::ObjectKindOf::Score) &&
        !hasObjectKind(kinds, game::ObjectKindOf::ScoreCreate))
        return std::nullopt;
    if (hasObjectKind(kinds, game::ObjectKindOf::Structure))
        return PlayerScoredObjectKind::Building;
    if (hasObjectKind(kinds, game::ObjectKindOf::Infantry) ||
        hasObjectKind(kinds, game::ObjectKindOf::Vehicle))
        return PlayerScoredObjectKind::Unit;
    return std::nullopt;
}

[[nodiscard]] inline PlayerAcademyProductionFacts academyProductionFacts(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    const ObjectContainmentRuntimeComponent* containment =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity);
    bool tunnelNetwork = false;
    if (containment && containment->plan) {
        tunnelNetwork = std::any_of(
            containment->plan->rules.begin(), containment->plan->rules.end(),
            [](const ObjectContainmentRule& rule) noexcept {
                return rule.kind == ObjectContainmentKind::Tunnel;
            });
    }
    return {
        .supplyCenter = hasObjectKind(kinds, game::ObjectKindOf::FsSupplyCenter),
        .dozer = hasObjectKind(kinds, game::ObjectKindOf::Dozer),
        .infantry = hasObjectKind(kinds, game::ObjectKindOf::Infantry),
        .vehicle = hasObjectKind(kinds, game::ObjectKindOf::Vehicle),
        .harvester = hasObjectKind(kinds, game::ObjectKindOf::Harvester),
        .hero = hasObjectKind(kinds, game::ObjectKindOf::Hero),
        .strategyCenter = hasObjectKind(kinds, game::ObjectKindOf::FsStrategyCenter),
        .tunnelNetwork = tunnelNetwork,
        .secondaryIncome = hasObjectKind(kinds, game::ObjectKindOf::MoneyHacker) ||
            hasObjectKind(kinds, game::ObjectKindOf::FsBlackMarket) ||
            hasObjectKind(kinds, game::ObjectKindOf::FsSupplyDropzone),
        .barracks = hasObjectKind(kinds, game::ObjectKindOf::FsBarracks),
        .warFactory = hasObjectKind(kinds, game::ObjectKindOf::FsWarfactory),
        .advancedTech = hasObjectKind(kinds, game::ObjectKindOf::FsAdvancedTech),
        .disguiser = hasObjectKind(kinds, game::ObjectKindOf::Disguiser),
    };
}

inline void recordAcademyProductionForObject(
    PlayerRegistry& players, const ecs::registry& registry,
    ecs::entity entity, PlayerId owner, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond) noexcept {
    static_cast<void>(players.recordAcademyProduction(
        owner, academyProductionFacts(registry, entity), confirmedTick,
        logicFramesPerSecond));
}

inline void recordCapturedObjectScore(
    PlayerRegistry& players, const ecs::registry& registry,
    ecs::entity entity, PlayerId newOwner) {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!hasObjectKind(kinds, game::ObjectKindOf::Structure) ||
        !type || type->name.empty()) return;
    static_cast<void>(players.recordObjectCaptured(
        newOwner, type->name,
        hasObjectKind(kinds, game::ObjectKindOf::Score)));
    static_cast<void>(players.recordAcademyEvent(
        newOwner, PlayerAcademyEvent::BuildingCaptured));
}

inline void recordRecoveredVehicleAcademy(
    PlayerRegistry& players, const ecs::registry& registry,
    ecs::entity entity, PlayerId previousOwner, PlayerId newOwner,
    uint64_t confirmedTick) noexcept {
    if (!previousOwner.isNeutral() || !newOwner || newOwner.isNeutral() ||
        !hasObjectKind(ecs::try_get<ObjectKindOfComponent>(registry, entity),
                       game::ObjectKindOf::Vehicle) ||
        !isObjectDisabledBy(registry, entity, ObjectDisabledReason::Unmanned,
                            confirmedTick)) return;
    static_cast<void>(players.recordAcademyEvent(
        newOwner, PlayerAcademyEvent::VehicleRecovered));
}

[[nodiscard]] inline bool hasFactionStructureKind(
    const ObjectKindOfComponent* kinds) noexcept {
    if (!kinds) return false;
    static const game::ObjectKindOfMask mask = [] {
        game::ObjectKindOfMask value;
        for (const game::ObjectKindOf kind : {
                 game::ObjectKindOf::FsFactory,
                 game::ObjectKindOf::FsBaseDefense,
                 game::ObjectKindOf::FsTechnology,
                 game::ObjectKindOf::FsSupplyDropzone,
                 game::ObjectKindOf::FsSuperweapon,
                 game::ObjectKindOf::FsBlackMarket,
                 game::ObjectKindOf::FsSupplyCenter,
                 game::ObjectKindOf::FsStrategyCenter,
                 game::ObjectKindOf::FsFake,
                 game::ObjectKindOf::FsInternetCenter,
                 game::ObjectKindOf::FsAdvancedTech,
                 game::ObjectKindOf::FsBarracks,
                 game::ObjectKindOf::FsWarfactory,
                 game::ObjectKindOf::FsAirfield}) {
            game::setObjectKind(value, kind);
        }
        return value;
    }();
    return kinds->mask.test_for_any(mask);
}

[[nodiscard]] inline bool isNavigationBlockingStructure(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    if (hasObjectKind(kinds, game::ObjectKindOf::Mine) ||
        hasObjectKind(kinds, game::ObjectKindOf::Projectile) ||
        hasObjectKind(kinds, game::ObjectKindOf::BridgeTower)) {
        return false;
    }
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (type && type->archetype) {
        if (type->archetype->templateData.fenceWidthFixed >
                math::q32_32{} &&
            !hasObjectKind(kinds, game::ObjectKindOf::DefensiveWall)) {
            return true;
        }
        const game::ObjectBodyKind body = type->archetype->templateData.body.kind;
        if (body == game::ObjectBodyKind::Structure ||
            body == game::ObjectBodyKind::HiveStructure) return true;
    }
    return hasObjectKind(kinds, game::ObjectKindOf::Structure);
}

[[nodiscard]] inline game::ObjectVeterancyLevel productionVeterancyLevel(
    const PlayerState& player, container::StringView templateName) noexcept {
    const auto found = std::find_if(
        player.productionModifiers.veterancy.begin(),
        player.productionModifiers.veterancy.end(),
        [templateName](const ProductionVeterancyModifier& modifier) {
            return container::asciiEqualIgnoreCase(
                modifier.thingTemplateName, templateName);
        });
    if (found == player.productionModifiers.veterancy.end())
        return game::ObjectVeterancyLevel::Regular;
    return ObjectExperienceSystem::parseLevel(found->veterancyName)
        .value_or(game::ObjectVeterancyLevel::Regular);
}

[[nodiscard]] inline math::q32_32 normalizeLegacyCreationOrientation(
    math::q32_32 angle) noexcept {
    // Match the legacy (-PI, +PI] convention without projecting an already
    // authoritative simulation angle through float. Keep the exact canonical
    // constants used by q32_32_trig so creation and later simulation agree at
    // the wrap boundary.
    constexpr int64_t kPiRaw = 13493037705LL;
    constexpr int64_t kTwoPiRaw = 26986075409LL;
    int64_t normalized = angle.raw() % kTwoPiRaw;
    if (normalized > kPiRaw) normalized -= kTwoPiRaw;
    if (normalized <= -kPiRaw) normalized += kTwoPiRaw;
    return math::q32_32::from_raw(normalized);
}

} // namespace engine::object_lifecycle_detail
