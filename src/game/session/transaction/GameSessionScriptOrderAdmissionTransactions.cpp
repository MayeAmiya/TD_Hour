#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"
#include "game/session/transaction/GameSessionObjectSaleTransactions.h"

#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/command/OrderExecutor.h"
#include "game/session/command/OrderContracts.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

constexpr auto equalsAsciiIgnoreCase = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* commandButtonField(
    const game::CommandButtonTemplate& button,
    container::StringView key) noexcept {
    for (auto it = button.fields.rbegin(); it != button.fields.rend(); ++it) {
        if (equalsAsciiIgnoreCase(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] bool commandButtonHasOption(
    const game::CommandButtonTemplate& button,
    container::StringView option) noexcept {
    size_t cursor = 0;
    while (cursor < button.options.size()) {
        while (cursor < button.options.size() &&
               (std::isspace(static_cast<unsigned char>(button.options[cursor])) ||
                button.options[cursor] == ',' || button.options[cursor] == '|')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < button.options.size() &&
               !std::isspace(static_cast<unsigned char>(button.options[cursor])) &&
               button.options[cursor] != ',' && button.options[cursor] != '|') {
            ++cursor;
        }
        if (begin != cursor && equalsAsciiIgnoreCase(
                container::StringView{button.options}.substr(begin, cursor - begin),
                option)) return true;
    }
    return false;
}

// RefCode's COMMAND_OPTION_NEED_OBJECT_TARGET is a C++ bit mask, never an
// authored spelling: "NEED_OBJECT_TARGET" appears in no shipped INI and in no
// CommandButtonStore option name, so testing for it always answered false and
// silently rejected every object-target FIRE_WEAPON click. The mask is exactly
// the union of the three relationship-scoped object-target options.
[[nodiscard]] bool commandButtonNeedsObjectTarget(
    const game::CommandButtonTemplate& button) noexcept {
    return commandButtonHasOption(button, "NEED_TARGET_ENEMY_OBJECT") ||
        commandButtonHasOption(button, "NEED_TARGET_NEUTRAL_OBJECT") ||
        commandButtonHasOption(button, "NEED_TARGET_ALLY_OBJECT");
}

[[nodiscard]] std::optional<uint32_t> commandButtonUnsignedField(
    const game::CommandButtonTemplate& button,
    container::StringView key) noexcept {
    const container::String* value = commandButtonField(button, key);
    if (!value || value->empty()) return std::nullopt;
    uint32_t parsed = 0;
    const char* begin = value->data();
    const char* end = begin + value->size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    return error == std::errc{} && next == end
        ? std::optional<uint32_t>{parsed}
        : std::nullopt;
}

} // namespace

GameSessionScriptOrderAdmissionTransactions::
GameSessionScriptOrderAdmissionTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionOrderAdmissionPolicyPort policy) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_policy(policy) {}

OrderExecutionResult GameSessionScriptOrderAdmissionTransactions::executeScriptOrder(
    const ScriptOrderIntent& order) {
    return executeScriptOrderInternal(order, false, false);
}

container::Vector<ObjectId>
GameSessionScriptOrderAdmissionTransactions::selectScriptMoveOrderActors(
    container::Span<const ObjectId> actors,
    ScriptOrderAuthority authority) const {
    if (!m_policy) return {};
    container::Vector<ObjectId> canonical{actors.begin(), actors.end()};
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()),
                    canonical.end());

    canonical.erase(
        std::remove_if(
            canonical.begin(), canonical.end(),
            [this, authority](ObjectId actor) {
                const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
                const ObjectHealthComponent* health = entity
                    ? ecs::try_get<ObjectHealthComponent>(
                          m_world.m_registry, *entity)
                    : nullptr;
                return !actor || !entity ||
                    (health && health->effectivelyDead) ||
                    (authority == ScriptOrderAuthority::ScenarioTeam &&
                     isObjectDisabledBy(
                         m_world.m_registry, *entity,
                         ObjectDisabledReason::Held,
                         m_presentation.m_confirmedTick)) ||
                    m_policy.rejectsOrdersWhileSleeping(actor) ||
                    !m_ai.m_objectAI.hasOrderCapability(
                        actor, ai::ObjectAIOrderCapability::MoveStop);
            }),
        canonical.end());
    return canonical;
}

std::optional<ScriptCommandButtonSelectionResult>
GameSessionScriptOrderAdmissionTransactions::selectScriptCommandButtonExecution(
    container::Span<const ObjectId> teamActors,
    container::StringView buttonName,
    script::ScriptCommandButtonActorPolicy actorPolicy,
    math::q32_32 actorPercentage,
    script::ScriptCommandButtonTargetKind targetKind,
    ObjectId explicitTarget,
    container::StringView targetFilter,
    container::Span<const container::String> targetObjectTypes,
    std::optional<math::q32_32> maximumRange) const {
    if (!m_policy) return std::nullopt;
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || teamActors.empty() ||
        buttonName.empty() || actorPercentage < math::q32_32{} ||
        (maximumRange && *maximumRange < math::q32_32{})) {
        return std::nullopt;
    }
    const game::CommandButtonTemplate* button =
        m_content.m_contentSnapshot.findCommandButton(buttonName);
    if (!button) return std::nullopt;

    const auto specialPowerDefinition = [&]()
        -> const SpecialPowerDefinition* {
        return button->specialPower.empty()
            ? nullptr
            : m_content.m_contentSnapshot.findSpecialPower(button->specialPower);
    };
    const auto actorHasSpecialPower = [&](ecs::entity entity) {
        const SpecialPowerDefinition* definition = specialPowerDefinition();
        const ObjectSpecialPowerComponent* powers =
            ecs::try_get<ObjectSpecialPowerComponent>(m_world.m_registry, entity);
        return definition && powers && std::any_of(
            powers->instances.begin(), powers->instances.end(),
            [definition](const ObjectSpecialPowerRuntime& runtime) {
                return runtime.content == definition->id;
            });
    };
    const auto actorHasCommandType = [&](ObjectId actor) {
        for (size_t slot = 0; slot < game::COMMAND_SET_SLOT_COUNT; ++slot) {
            const container::StringView candidateName =
                m_policy.effectiveCommandBarButton(actor, slot);
            const game::CommandButtonTemplate* candidate =
                m_content.m_contentSnapshot.findCommandButton(candidateName);
            if (candidate &&
                candidate->descriptor.kind == button->descriptor.kind) {
                return true;
            }
        }
        return false;
    };
    const auto canUse = [&](ObjectId actor, ObjectId target) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
        if (!entity || m_world.m_objects.isPendingDestroy(actor) ||
            isObjectDisabled(m_world.m_registry, *entity, m_presentation.m_confirmedTick)) {
            return false;
        }
        const bool needsObject = commandButtonNeedsObjectTarget(*button);
        const bool needsPosition =
            commandButtonHasOption(*button, "NEED_TARGET_POS");
        if ((needsObject && !target) || (needsPosition && !target) ||
            (target && !needsObject && !needsPosition)) {
            return false;
        }
        if (target) {
            const std::optional<ecs::entity> targetEntity =
                m_world.m_objects.entityFromId(target);
            if (!targetEntity || m_world.m_objects.isPendingDestroy(target))
                return false;
            // RefCode checks the relationship mask only for a true object-
            // target button. NEED_TARGET_POS may receive an Object solely as
            // a convenient source of its current position.
            if (needsObject) {
                const PlayerRelationship relationship =
                    relationshipBetweenObjects(
                        m_world.m_registry, m_content.m_players, *entity, *targetEntity);
                const bool allowed =
                    (relationship == PlayerRelationship::Enemies &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_ENEMY_OBJECT")) ||
                    (relationship == PlayerRelationship::Allies &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_ALLY_OBJECT")) ||
                    (relationship == PlayerRelationship::Neutral &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_NEUTRAL_OBJECT"));
                if (!allowed) return false;
            }
        }

        const game::CommandButtonKind commandKind = button->descriptor.kind;
        if (commandKind == game::CommandButtonKind::SpecialPower) {
            const SpecialPowerDefinition* definition = specialPowerDefinition();
            const ObjectSpecialPowerComponent* powers =
                ecs::try_get<ObjectSpecialPowerComponent>(m_world.m_registry, *entity);
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
            if (!definition || !powers || !owner ||
                (!definition->requiredScience.empty() &&
                 !m_content.m_players.hasScience(owner->player,
                                       definition->requiredScience))) {
                return false;
            }
            return std::any_of(
                powers->instances.begin(), powers->instances.end(),
                [this, definition](const ObjectSpecialPowerRuntime& runtime) {
                    return runtime.content == definition->id &&
                        runtime.readyTick <= m_presentation.m_confirmedTick &&
                        runtime.pausedCount == 0;
                });
        }
        if (commandKind == game::CommandButtonKind::ObjectUpgrade ||
            commandKind == game::CommandButtonKind::PlayerUpgrade ||
            commandKind == game::CommandButtonKind::UnitBuild) {
            const ObjectProductionComponent* production =
                ecs::try_get<ObjectProductionComponent>(m_world.m_registry, *entity);
            if (!production) return false;
            if (commandKind == game::CommandButtonKind::UnitBuild) {
                return true;
            }
            // RefCode's CommandButton::isValidToUseOn rejects a producer
            // while any upgrade job is already in its ProductionUpdate FIFO,
            // then asks the relevant UpgradeMux/Player authority whether the
            // authored upgrade is still applicable.
            if (std::any_of(
                    production->jobs.begin(), production->jobs.end(),
                    [](const ObjectProductionJob& job) {
                        return job.kind == ObjectProductionJobKind::PlayerUpgrade ||
                               job.kind == ObjectProductionJobKind::ObjectUpgrade;
                    }) || button->upgrade.empty()) {
                return false;
            }
            const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
            const UpgradeDefinition* definition = catalog
                ? catalog->find(button->upgrade) : nullptr;
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
            if (!definition || !owner) return false;
            if (definition->type == UpgradeDefinitionType::Player) {
                return !m_content.m_players.hasUpgradeComplete(
                           owner->player, definition->id) &&
                       !m_content.m_players.hasUpgradeInProgress(
                           owner->player, definition->id);
            }
            return m_policy.canReceiveUpgrade(
                actor, definition->name);
        }
        if (commandKind == game::CommandButtonKind::DozerConstruct) {
            return target
                ? ecs::try_get<ObjectBuilderComponent>(m_world.m_registry, *entity) !=
                      nullptr
                : ecs::try_get<ObjectProductionComponent>(m_world.m_registry, *entity) !=
                      nullptr;
        }
        if (commandKind == game::CommandButtonKind::SwitchWeapon ||
            commandKind == game::CommandButtonKind::FireWeapon) {
            return ecs::try_get<ObjectWeaponComponent>(m_world.m_registry, *entity) !=
                nullptr;
        }
        if (commandKind == game::CommandButtonKind::HackInternet) {
            const ObjectEconomyComponent* economy =
                ecs::try_get<ObjectEconomyComponent>(m_world.m_registry, *entity);
            return economy && economy->plan &&
                !economy->plan->hackInternet.empty() &&
                !economy->hackInternet.empty();
        }
        if (commandKind == game::CommandButtonKind::AttackMove) {
            return ecs::try_get<ObjectLocomotionComponent>(
                       m_world.m_registry, *entity) != nullptr;
        }
        if (commandKind == game::CommandButtonKind::CombatDrop) {
            const ObjectAirfieldComponent* airfield =
                ecs::try_get<ObjectAirfieldComponent>(m_world.m_registry, *entity);
            return target && airfield && airfield->plan &&
                !airfield->chinookAi.empty() &&
                !airfield->plan->chinookAi.empty();
        }
        return commandKind == game::CommandButtonKind::Stop ||
            commandKind == game::CommandButtonKind::Sell;    };

    ScriptCommandButtonSelectionResult result;
    if (actorPolicy ==
        script::ScriptCommandButtonActorPolicy::PartialUsable) {
        if (targetKind != script::ScriptCommandButtonTargetKind::None)
            return std::nullopt;
        container::Vector<ObjectId> usable;
        for (const ObjectId actor : teamActors) {
            if (canUse(actor, INVALID_OBJECT_ID)) usable.push_back(actor);
        }
        if (usable.size() > static_cast<size_t>(
                std::numeric_limits<int32_t>::max())) {
            return std::nullopt;
        }
        const math::q32_32 scaled = actorPercentage *
            math::q32_32{static_cast<int32_t>(usable.size())} /
            math::q32_32{int32_t{100}};
        if (scaled <= math::q32_32{}) return result;
        const size_t count = std::min(
            usable.size(), static_cast<size_t>(std::max(
                int32_t{}, scaled.to_int())));
        result.actors.assign(usable.begin(), usable.begin() + count);
        return result;
    }
    if (actorPolicy != script::ScriptCommandButtonActorPolicy::All)
        return std::nullopt;

    ObjectId source = INVALID_OBJECT_ID;
    for (const ObjectId actor : teamActors) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
        if (!entity) continue;
        const bool capable =
            button->descriptor.kind == game::CommandButtonKind::SpecialPower
            ? actorHasSpecialPower(*entity)
            : actorHasCommandType(actor);
        if (capable) {
            source = actor;
            break;
        }
    }
    if (!source) return std::nullopt;
    result.sourceActor = source;

    if (targetKind == script::ScriptCommandButtonTargetKind::NamedObject) {
        if (!explicitTarget || !canUse(source, explicitTarget))
            return std::nullopt;
        result.actors.assign(teamActors.begin(), teamActors.end());
        result.targetObject = explicitTarget;
        return result;
    }
    if (targetKind == script::ScriptCommandButtonTargetKind::None ||
        targetKind == script::ScriptCommandButtonTargetKind::Waypoint) {
        return std::nullopt;
    }

    math::q32_32 centerX{};
    math::q32_32 centerY{};
    size_t centerCount = 0;
    const auto accumulateCenter = [&](bool requireAI) {
        for (const ObjectId actor : teamActors) {
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
            const TransformComponent* transform = entity
                ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity)
                : nullptr;
            if (!transform ||
                isObjectDisabledBy(m_world.m_registry, *entity,
                                   ObjectDisabledReason::Held,
                                   m_presentation.m_confirmedTick) ||
                (requireAI && !m_ai.m_objectAI.find(actor))) {
                continue;
            }
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, *entity,
                *transform);
            centerX += position.x;
            centerY += position.y;
            ++centerCount;
        }
    };
    // AIGroup::getCenter() first uses ordinary AIUpdate members and falls
    // back to the remaining group only when none are usable. Held objects do
    // not contribute in either pass.
    accumulateCenter(true);
    if (centerCount == 0) accumulateCenter(false);
    if (centerCount == 0) return std::nullopt;
    if (centerCount > static_cast<size_t>(
            std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    const math::q32_32 fixedCenterCount{
        static_cast<int32_t>(centerCount)};
    centerX /= fixedCenterCount;
    centerY /= fixedCenterCount;

    const std::optional<ecs::entity> sourceEntity = m_world.m_objects.entityFromId(source);
    const OwnerComponent* sourceOwner = sourceEntity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *sourceEntity)
        : nullptr;
    const ObjectMapStatusComponent* sourceMap = sourceEntity
        ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *sourceEntity)
        : nullptr;
    if (!sourceOwner) return std::nullopt;
    const bool sourceOffMap = sourceMap && sourceMap->offMap;
    const bool directObjectType = targetKind ==
            script::ScriptCommandButtonTargetKind::NearestObjectType &&
        static_cast<bool>(m_content.m_contentSnapshot.findObjectArchetype(targetFilter));
    const bool mostValuable = targetKind ==
        script::ScriptCommandButtonTargetKind::MostValuableEnemy;
    const std::optional<game::ObjectKindOf> targetKindOf =
        game::parseObjectKindOf(targetFilter);
    const std::optional<math::q32_32> maximumRangeSquared = maximumRange
        ? std::optional<math::q32_32>{*maximumRange * *maximumRange}
        : std::nullopt;

    math::q32_32 bestDistance{};
    int64_t bestValue = std::numeric_limits<int64_t>::min();
    ObjectId best = INVALID_OBJECT_ID;
    const auto candidates = ecs::view<
        const ObjectIdentityComponent, const ObjectLifecycleComponent,
        const OwnerComponent, const TransformComponent,
        const ThingTemplateComponent>(m_world.m_registry);
    for (const ecs::entity candidate : candidates) {
        const ObjectIdentityComponent& identity = candidates.template get<
            const ObjectIdentityComponent>(candidate);
        const ObjectLifecycleComponent& lifecycle = candidates.template get<
            const ObjectLifecycleComponent>(candidate);
        const TransformComponent& transform = candidates.template get<
            const TransformComponent>(candidate);
        const ThingTemplateComponent& type = candidates.template get<
            const ThingTemplateComponent>(candidate);
        if (!identity.id || lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !type.archetype || m_world.m_objects.isPendingDestroy(identity.id)) {
            continue;
        }
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, candidate);
        if ((map && map->offMap) != sourceOffMap) continue;
        const PlayerRelationship affiliation =
            relationshipBetweenPlayerAndObject(
                m_world.m_registry, m_content.m_players, sourceOwner->player, candidate);
        const bool objectTypeTarget = targetKind ==
            script::ScriptCommandButtonTargetKind::NearestObjectType;
        if (affiliation != PlayerRelationship::Enemies &&
            !(objectTypeTarget && affiliation == PlayerRelationship::Neutral)) {
            continue;
        }

        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, candidate);
        const bool structure = game::objectHasKind(
            type.archetype->kindOfMask, game::ObjectKindOf::Structure);
        if (targetKind ==
                script::ScriptCommandButtonTargetKind::NearestGarrisonedBuilding) {
            const ObjectContainmentRuntimeComponent* containment =
                ecs::try_get<ObjectContainmentRuntimeComponent>(
                    m_world.m_registry, candidate);
            const bool garrisonable = containment && containment->plan &&
                std::any_of(containment->plan->rules.begin(),
                            containment->plan->rules.end(),
                    [](const ObjectContainmentRule& rule) {
                        return rule.kind == ObjectContainmentKind::Garrison;
                    });
            if (!structure || !garrisonable) continue;
        } else if (targetKind ==
                   script::ScriptCommandButtonTargetKind::NearestKindOf) {
            if (!kinds || !targetKindOf ||
                !game::objectHasKind(kinds->mask, *targetKindOf)) {
                continue;
            }
        } else if (targetKind ==
                   script::ScriptCommandButtonTargetKind::NearestEnemyBuilding) {
            if (!structure) continue;
        } else if (targetKind ==
                   script::ScriptCommandButtonTargetKind::NearestEnemyBuildingClass) {
            if (!structure || !kinds || !targetKindOf ||
                !game::objectHasKind(kinds->mask, *targetKindOf)) {
                continue;
            }
        } else if (objectTypeTarget) {
            const container::StringView candidateType =
                type.archetype->templateData.name;
            const bool matches = directObjectType
                ? candidateType == targetFilter
                : std::find(targetObjectTypes.begin(),
                            targetObjectTypes.end(), candidateType) !=
                      targetObjectTypes.end();
            if (!matches) continue;
        }
        if (!canUse(source, identity.id)) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, candidate, transform);
        const math::q32_32 dx = position.x - centerX;
        const math::q32_32 dy = position.y - centerY;
        const math::q32_32 distance = dx * dx + dy * dy;
        if (maximumRangeSquared && distance > *maximumRangeSquared)
            continue;
        int64_t candidateValue = 0;
        if (mostValuable) {
            const OwnerComponent& candidateOwner =
                candidates.template get<const OwnerComponent>(candidate);
            const PlayerState* candidatePlayer =
                m_content.m_players.get(candidateOwner.player);
            if (candidatePlayer) {
                candidateValue = std::max<int64_t>(
                    0, calculateObjectBuildCost(
                           *type.archetype, *candidatePlayer,
                           m_world.m_registry, m_world.m_objects));
            }
        }
        if ((mostValuable &&
             (candidateValue > bestValue ||
              (candidateValue == bestValue &&
                (!best || distance < bestDistance ||
                 (distance == bestDistance &&
                  (!best || identity.id < best)))))) ||
            (!mostValuable &&
             (!best || distance < bestDistance ||
              (distance == bestDistance &&
               (!best || identity.id < best))))) {
            bestValue = candidateValue;
            bestDistance = distance;
            best = identity.id;
        }
    }
    if (!best) return std::nullopt;
    result.actors.assign(teamActors.begin(), teamActors.end());
    result.targetObject = best;
    return result;
}

OrderExecutionResult GameSessionScriptOrderAdmissionTransactions::executeScriptCommandButton(
    const ScriptOrderIntent& envelope,
    bool requireButtonInCommandSet) {
    if (!m_policy) {
        return {.accepted = false,
                .rejection = OrderRejectionReason::InvalidPlayer,
                .message = "order admission has no Session barrier"};
    }
    const auto reject = [](OrderRejectionReason reason,
                           container::String message) {
        return OrderExecutionResult{
            .accepted = false,
            .rejection = reason,
            .message = std::move(message),
        };
    };
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        envelope.confirmedTick != m_presentation.m_confirmedTick) {
        return reject(OrderRejectionReason::MalformedOrder,
                      "command-button effect is outside the active confirmed tick");
    }
    if (envelope.kind != ObjectOrderKind::CommandButton ||
        envelope.contentName.empty()) {
        return reject(OrderRejectionReason::MalformedOrder,
                      "command-button effect has no authored button identity");
    }
    const game::CommandButtonTemplate* button =
        m_content.m_contentSnapshot.findCommandButton(envelope.contentName);
    if (!button) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      "command-button is absent from the frozen session content");
    }
    if (envelope.targetObject && envelope.targetPosition.valid) {
        return reject(OrderRejectionReason::MalformedOrder,
                      "command-button effect has more than one target shape");
    }

    ScriptOrderIntent routed = envelope;
    routed.actors.clear();
    routed.actors.reserve(envelope.actors.size());
    const auto actorHasButton = [this, button](ObjectId actor) {
        for (size_t slot = 0; slot < game::COMMAND_SET_SLOT_COUNT; ++slot) {
            if (equalsAsciiIgnoreCase(
                    m_policy.effectiveCommandBarButton(actor, slot),
                    button->name))
                return true;
        }
        return false;
    };
    for (const ObjectId actor : envelope.actors) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
        if (!entity || m_world.m_objects.isPendingDestroy(actor) ||
            (isObjectDisabled(
                 m_world.m_registry, *entity,
                 m_presentation.m_confirmedTick) &&
             !game::commandButtonWorksWhileDisabled(
                 button->descriptor.kind)) ||
            (requireButtonInCommandSet && !actorHasButton(actor))) {
            continue;
        }
        routed.actors.push_back(actor);
    }
    if (routed.actors.empty()) {
        // Missing/disabled named objects, empty Teams and a named object's
        // current CommandSet not containing the button are legacy no-ops.
        return {.accepted = true, .actorCount = 0};
    }

    const bool hasObjectTarget = static_cast<bool>(routed.targetObject);
    const bool hasPositionTarget = routed.targetPosition.valid;
    const bool noTarget = !hasObjectTarget && !hasPositionTarget;
    const auto noTargetOnly = [&](container::StringView command) {
        return reject(OrderRejectionReason::UnsupportedCommand,
                      container::String(command) +
                          " CommandButton does not accept this target shape");
    };
    const auto field = [button](container::StringView key)
        -> container::StringView {
        const container::String* value = commandButtonField(*button, key);
        return value ? container::StringView{*value} : container::StringView{};
    };
    const auto actorOwner = [this](ObjectId actor)
        -> std::optional<PlayerId> {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
            : nullptr;
        return owner ? std::optional<PlayerId>{owner->player} : std::nullopt;
    };

    const game::CommandButtonKind commandKind = button->descriptor.kind;
    if (commandKind == game::CommandButtonKind::SpecialPower) {
        const SpecialPowerDefinition* definition = button->specialPower.empty()
            ? nullptr
            : m_content.m_contentSnapshot.findSpecialPower(button->specialPower);
        if (!definition) {
            return reject(OrderRejectionReason::MalformedOrder,
                          "special-power CommandButton has no resolved SpecialPower");
        }
        routed.actors.erase(std::remove_if(
            routed.actors.begin(), routed.actors.end(),
            [this, definition](ObjectId actor) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(actor);
                const ObjectSpecialPowerComponent* powers = entity
                    ? ecs::try_get<ObjectSpecialPowerComponent>(
                          m_world.m_registry, *entity)
                    : nullptr;
                return !powers || std::none_of(
                    powers->instances.begin(), powers->instances.end(),
                    [definition](const ObjectSpecialPowerRuntime& runtime) {
                        return runtime.content == definition->id;
                    });
            }), routed.actors.end());
        if (routed.actors.empty()) {
            // AIGroup forwards the button to every member, but Object's
            // doSpecialPowerAtObject() silently does nothing when that member
            // has no module for the requested SpecialPowerTemplate.
            return {.accepted = true, .actorCount = 0};
        }
        routed.kind = ObjectOrderKind::SpecialPower;
        routed.contentName = button->name;
        return executeScriptOrderInternal(routed, false, false);
    }
    if (commandKind == game::CommandButtonKind::Stop) {
        routed.kind = ObjectOrderKind::Stop;
        routed.contentName.clear();
        routed.targetObject = INVALID_OBJECT_ID;
        routed.targetPosition = {};
        return executeScriptOrderInternal(routed, false, false);
    }
    if (commandKind == game::CommandButtonKind::SwitchWeapon) {
        if (!noTarget) return noTargetOnly("SWITCH_WEAPON");
        // CommandButtonStore compiles the authored WeaponSlot with RefCode's
        // INI::parseLookupList semantics (first token wins, PRIMARY default).
        // Reparsing the raw field here rejected Command_ChinaNeutronWarhead,
        // whose shipped value is "SECONDARY TERTIARY".
        const game::WeaponSlot slot =
            static_cast<game::WeaponSlot>(button->descriptor.weaponSlot);
        size_t changed = 0;
        for (const ObjectId actor : routed.actors) {
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
            if (entity && setObjectWeaponLock(
                    m_world.m_registry, *entity, slot,
                    ObjectWeaponLockType::Permanent)) {
                ++changed;
            }
        }
        return {.accepted = true, .actorCount = changed};
    }
    if (commandKind == game::CommandButtonKind::FireWeapon) {
        const bool needsObject = commandButtonNeedsObjectTarget(*button);
        const bool needsPosition =
            commandButtonHasOption(*button, "NEED_TARGET_POS");
        const bool attackObjectsPosition =
            commandButtonHasOption(*button, "ATTACK_OBJECTS_POSITION");
        const bool usesMineClearingWeaponSet =
            commandButtonHasOption(*button,
                                   "USES_MINE_CLEARING_WEAPONSET");
        if ((noTarget && (needsObject || needsPosition)) ||
            (hasPositionTarget && (!needsPosition || needsObject)) ||
            (hasObjectTarget && !needsObject && !needsPosition)) {
            return reject(OrderRejectionReason::UnsupportedCommand,
                          "FIRE_WEAPON target shape does not match its CommandButton options");
        }
        // Same compiled WeaponSlot as SWITCH_WEAPON above: descriptor.weaponSlot
        // already applied RefCode's first-token lookup and PRIMARY default.
        const game::WeaponSlot slot =
            static_cast<game::WeaponSlot>(button->descriptor.weaponSlot);

        ObjectId attackObject = hasObjectTarget
            ? routed.targetObject : INVALID_OBJECT_ID;
        CommandPosition sharedAttackPosition = routed.targetPosition;
        std::optional<ecs::entity> targetEntity;
        if (hasObjectTarget) {
            targetEntity = m_world.m_objects.entityFromId(routed.targetObject);
            const ObjectFixedTransformComponent* targetTransform = targetEntity
                ? ecs::try_get<ObjectFixedTransformComponent>(
                      m_world.m_registry, *targetEntity)
                : nullptr;
            if (!targetEntity || m_world.m_objects.isPendingDestroy(routed.targetObject) ||
                ((attackObjectsPosition || (!needsObject && needsPosition)) &&
                 (!targetTransform || !targetTransform->authoritative))) {
                return reject(OrderRejectionReason::InvalidTarget,
                              "FIRE_WEAPON target is unavailable");
            }
            if (attackObjectsPosition || (!needsObject && needsPosition)) {
                attackObject = INVALID_OBJECT_ID;
                sharedAttackPosition = {
                    .x = targetTransform->position.x,
                    .y = targetTransform->position.y,
                    .z = targetTransform->position.z,
                    .valid = true,
                };
            }
        }

        container::Vector<ObjectId> eligible;
        eligible.reserve(routed.actors.size());
        for (const ObjectId actor : routed.actors) {
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
            if (entity && usesMineClearingWeaponSet) {
                ObjectCombatProfileComponent* combat =
                    ecs::try_get<ObjectCombatProfileComponent>(
                        m_world.m_registry, *entity);
                const game::WeaponSetConditionMask mineClearing =
                    game::weaponSetConditionBit(
                        game::WeaponSetCondition::MineClearingDetail);
                if (combat && (combat->weaponConditions & mineClearing) == 0) {
                    combat->weaponConditions |= mineClearing;
                    static_cast<void>(refreshObjectWeaponSet(
                        m_world.m_registry, *entity,
                        m_content.m_contentSnapshot,
                        m_content.m_objectSimulationRules.logicFramesPerSecond,
                        envelope.confirmedTick));
                }
            }
            const ObjectWeaponComponent* weapons = entity
                ? ecs::try_get<ObjectWeaponComponent>(m_world.m_registry, *entity)
                : nullptr;
            const size_t slotIndex = static_cast<size_t>(slot);
            if (!weapons || !weapons->activeWeaponSetIndex ||
                *weapons->activeWeaponSetIndex >= weapons->sets.size() ||
                slotIndex >= game::kWeaponSlotCount ||
                !weapons->sets[*weapons->activeWeaponSetIndex]
                     .slots[slotIndex].content ||
                m_policy.rejectsOrdersWhileSleeping(actor)) {
                continue;
            }
            if (hasObjectTarget && needsObject) {
                const PlayerRelationship relationship =
                    relationshipBetweenObjects(
                        m_world.m_registry, m_content.m_players, *entity, *targetEntity);
                const bool relationshipAllowed =
                    (relationship == PlayerRelationship::Enemies &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_ENEMY_OBJECT")) ||
                    (relationship == PlayerRelationship::Allies &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_ALLY_OBJECT")) ||
                    (relationship == PlayerRelationship::Neutral &&
                     commandButtonHasOption(*button,
                                            "NEED_TARGET_NEUTRAL_OBJECT"));
                if (!relationshipAllowed) continue;
            }
            eligible.push_back(actor);
        }
        if (eligible.empty()) return {.accepted = true, .actorCount = 0};

        // RefCode's AIGroup forwards a CommandButton to each member. Submit
        // one typed Attack per actor as well: no-target FIRE_WEAPON uses that
        // actor's own position, and a rejected member must not leave a weapon
        // lock on otherwise valid members.
        size_t queued = 0;
        for (const ObjectId actor : eligible) {
            CommandPosition attackPosition = sharedAttackPosition;
            if (noTarget) {
                const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
                if (!entity) continue;
                const ObjectFixedTransformComponent* fixedTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        m_world.m_registry, *entity);
                if (!fixedTransform || !fixedTransform->authoritative)
                    continue;
                const LogicFixedVec3 attackPositionFixed =
                    fixedTransform->position;
                attackPosition = {
                    .x = attackPositionFixed.x,
                    .y = attackPositionFixed.y,
                    .z = attackPositionFixed.z,
                    .valid = true,
                };
            }

            ScriptOrderIntent single = routed;
            single.kind = ObjectOrderKind::Attack;
            single.systemPurpose =
                ObjectOrderSystemPurpose::CommandButtonFireWeapon;
            single.actors = {actor};
            single.targetObject = attackObject;
            single.targetPosition = attackPosition;
            single.contentName.clear();
            single.maximumShots = commandButtonUnsignedField(
                *button, "MaxShotsToFire");
            const OrderExecutionResult admitted =
                executeScriptOrderInternal(single, false, false);
            if (!admitted.accepted || admitted.actorCount == 0) continue;
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
            if (entity) {
                static_cast<void>(setObjectWeaponLock(
                    m_world.m_registry, *entity, slot,
                    ObjectWeaponLockType::Temporary));
            }
            queued += admitted.actorCount;
        }
        return {.accepted = true, .actorCount = queued};
    }
    if (commandKind == game::CommandButtonKind::ObjectUpgrade ||
        commandKind == game::CommandButtonKind::PlayerUpgrade) {
        if (!noTarget) return noTargetOnly(button->command);
        if (button->upgrade.empty()) {
            return reject(OrderRejectionReason::MalformedOrder,
                          "upgrade CommandButton has no Upgrade field");
        }
        size_t queued = 0;
        for (const ObjectId actor : routed.actors) {
            const std::optional<PlayerId> owner = actorOwner(actor);
            if (owner && m_policy.queuePlayerUpgrade(
                    actor, *owner, button->upgrade,
                    envelope.sourceEffectOrdinal,
                    envelope.confirmedTick).accepted) {
                ++queued;
            }
        }
        return {.accepted = true, .actorCount = queued};
    }
    if (commandKind == game::CommandButtonKind::UnitBuild ||
        commandKind == game::CommandButtonKind::DozerConstruct) {
        const container::StringView objectType = field("Object");
        if (objectType.empty()) {
            return reject(OrderRejectionReason::MalformedOrder,
                          "build CommandButton has no Object field");
        }
        if (hasPositionTarget &&
            commandKind == game::CommandButtonKind::DozerConstruct) {
            routed.kind = ObjectOrderKind::Build;
            routed.contentName = container::String{objectType};
            return executeScriptOrderInternal(routed, false, false);
        }
        if (!noTarget) return noTargetOnly(button->command);
        size_t queued = 0;
        for (const ObjectId actor : routed.actors) {
            const std::optional<PlayerId> owner = actorOwner(actor);
            if (owner && m_policy.queueProduction(
                    actor, *owner, objectType,
                    envelope.sourceEffectOrdinal,
                    envelope.confirmedTick).accepted) {
                ++queued;
            }
        }
        return {.accepted = true, .actorCount = queued};
    }
    if (commandKind == game::CommandButtonKind::HackInternet) {
        if (!noTarget) return noTargetOnly("HACK_INTERNET");
        routed.actors.erase(std::remove_if(
            routed.actors.begin(), routed.actors.end(),
            [this](ObjectId actor) {
                const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
                const ObjectEconomyComponent* economy = entity
                    ? ecs::try_get<ObjectEconomyComponent>(m_world.m_registry, *entity)
                    : nullptr;
                return !economy || !economy->plan ||
                    economy->plan->hackInternet.empty() ||
                    economy->hackInternet.empty();
            }), routed.actors.end());
        if (routed.actors.empty())
            return {.accepted = true, .actorCount = 0};
        routed.kind = ObjectOrderKind::CommandButton;
        routed.contentName = button->name;
        return executeScriptOrderInternal(routed, true, false);
    }
    if (commandKind == game::CommandButtonKind::Sell) {
        if (!noTarget) return noTargetOnly("SELL");
        size_t sold = 0;
        GameSessionObjectSaleTransactions sales{
            m_content, m_world, m_ai, m_presentation,
            m_policy.lifecycle};
        for (const ObjectId actor : routed.actors) {
            const std::optional<PlayerId> owner = actorOwner(actor);
            if (owner && sales.beginObjectSale(
                    actor, *owner, envelope.confirmedTick)) {
                ++sold;
            }
        }
        return {.accepted = true, .actorCount = sold};
    }
    if (commandKind == game::CommandButtonKind::AttackMove) {
        if (!hasPositionTarget) return noTargetOnly("ATTACK_MOVE");
        routed.kind = ObjectOrderKind::Move;
        routed.attackMove = true;
        routed.maximumShots = commandButtonUnsignedField(
            *button, "MaxShotsToFire");
        routed.contentName.clear();
        return executeScriptOrderInternal(routed, false, false);
    }
    if (commandKind == game::CommandButtonKind::CombatDrop) {
        if (!hasObjectTarget) return noTargetOnly("COMBATDROP");
        const std::optional<ecs::entity> target =
            m_world.m_objects.entityFromId(routed.targetObject);
        const ObjectFixedTransformComponent* targetTransform = target
            ? ecs::try_get<ObjectFixedTransformComponent>(
                  m_world.m_registry, *target)
            : nullptr;
        if (!targetTransform || !targetTransform->authoritative) {
            return reject(OrderRejectionReason::InvalidTarget,
                          "COMBATDROP target has no authoritative transform");
        }
        const LogicFixedVec3 targetPositionFixed =
            targetTransform->position;
        routed.kind = ObjectOrderKind::CommandButton;
        routed.contentName = button->name;
        routed.actors.erase(std::remove_if(
            routed.actors.begin(), routed.actors.end(),
            [this](ObjectId actor) {
                const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(actor);
                const ObjectAirfieldComponent* airfield = entity
                    ? ecs::try_get<ObjectAirfieldComponent>(m_world.m_registry, *entity)
                    : nullptr;
                return !airfield || !airfield->plan ||
                    airfield->chinookAi.empty() ||
                    airfield->plan->chinookAi.empty();
            }), routed.actors.end());
        if (routed.actors.empty())
            return {.accepted = true, .actorCount = 0};
        routed.targetPosition = {
            .x = targetPositionFixed.x,
            .y = targetPositionFixed.y,
            .z = targetPositionFixed.z,
            .valid = true,
        };
        return executeScriptOrderInternal(routed, false, true);
    }

    return reject(OrderRejectionReason::UnsupportedCommand,
                  "CommandButton command has no completed modern owner: " +
                      button->command);
}

OrderExecutionResult GameSessionScriptOrderAdmissionTransactions::executeScriptOrderInternal(
    const ScriptOrderIntent& order,
    bool allowHackInternetCommand,
    bool allowCombatDropCommand) {
    if (!m_policy) {
        return {.accepted = false,
                .rejection = OrderRejectionReason::InvalidPlayer,
                .message = "order admission has no Session barrier"};
    }
    if (!m_content.m_active) {
        return {.accepted = false, .rejection = OrderRejectionReason::InvalidPlayer,
                .message = "cannot execute a script order outside an active session"};
    }
    OrderExecutionResult result;
    if (!m_presentation.m_hasConfirmedFrame || order.confirmedTick != m_presentation.m_confirmedTick) {
        result = {.accepted = false,
                  .rejection = OrderRejectionReason::MalformedOrder,
                  .message = "script order does not belong to the active confirmed tick"};
    } else if (isObjectWaypointRouteSubtype(order.moveRouteSubtype) &&
               (order.kind != ObjectOrderKind::Move ||
                order.waypointGraphRevision !=
                    m_content.m_terrain.waypointGraphRevision() ||
                !m_content.m_terrain.waypointById(order.waypointStartId))) {
        result = {.accepted = false,
                  .rejection = OrderRejectionReason::InvalidTarget,
                  .message =
                      "script waypoint route references a missing or stale graph node"};
    } else if (order.kind == ObjectOrderKind::TacticalAttack &&
               std::any_of(
                   order.actors.begin(), order.actors.end(),
                   [this, guard = order.tacticalAttackSubtype ==
                        ObjectTacticalAttackSubtype::Guard ||
                        order.tacticalAttackSubtype ==
                        ObjectTacticalAttackSubtype::GuardTunnelNetwork](ObjectId actor) {
                       if (m_policy.rejectsOrdersWhileSleeping(actor))
                           return false;
                       return !m_ai.m_objectAI.hasOrderCapability(
                                  actor, ai::ObjectAIOrderCapability::Attack) ||
                           (guard && !m_ai.m_objectAI.hasOrderCapability(
                               actor, ai::ObjectAIOrderCapability::MoveStop));
                   })) {
        // TacticalAttack has no fallback queue consumer. Hunt/Squad/Area
        // require Attack ownership; Guard additionally composes a Move child
        // and therefore requires MoveStop ownership. Reject the full order
        // before queue mutation instead of leaving an unowned head forever.
        result = {.accepted = false,
                  .rejection = OrderRejectionReason::UnsupportedCommand,
                  .message =
                        order.tacticalAttackSubtype ==
                                  ObjectTacticalAttackSubtype::Guard ||
                              order.tacticalAttackSubtype ==
                                  ObjectTacticalAttackSubtype::GuardTunnelNetwork
                          ? "script Guard actor lacks ObjectAI Attack/MoveStop capability"
                          : "script tactical actor has no ObjectAI Attack capability"};
    } else {
        ScriptOrderIntent admittedOrder = order;
        if (order.kind == ObjectOrderKind::Move) {
            // RefCode filters Held members from AIGroup/Team movement, while
            // doNamedMove() submits directly to the named object's AIUpdate.
            // Preserve that distinction while retaining the shared dead,
            // sleep and consumer checks.
            admittedOrder.actors = selectScriptMoveOrderActors(
                container::Span<const ObjectId>{order.actors},
                order.authority);
        } else {
            admittedOrder.actors.erase(
                std::remove_if(
                    admittedOrder.actors.begin(), admittedOrder.actors.end(),
                    [this, kind = order.kind](ObjectId actor) {
                        // ScriptActions::doNamedFireSpecialPowerAt* calls the
                        // object's SpecialPowerModuleInterface directly. It
                        // never routes through AIUpdateInterface, so an AI
                        // player's ATTITUDE_SLEEP must not consume a named
                        // cinematic launch as a successful zero-actor order.
                        return kind != ObjectOrderKind::SpecialPower &&
                            m_policy.rejectsOrdersWhileSleeping(actor);
                    }),
                admittedOrder.actors.end());
        }
        const bool ordinaryWaypoint =
            order.moveRouteSubtype ==
                ObjectMoveRouteSubtype::WaypointPathIndividuals ||
            order.moveRouteSubtype ==
                ObjectMoveRouteSubtype::WaypointPathTeam;
        if (ordinaryWaypoint) {
            admittedOrder.attackMove = !admittedOrder.actors.empty() &&
                std::all_of(
                    admittedOrder.actors.begin(),
                    admittedOrder.actors.end(),
                    [this](ObjectId actor) {
                        return m_ai.m_objectAI.hasOrderCapability(
                            actor, ai::ObjectAIOrderCapability::Attack);
                    });
        }
        ScriptOrderIntent promotedOrder = admittedOrder;
        promotedOrder.actors.clear();
        if (admittedOrder.kind == ObjectOrderKind::Move &&
            admittedOrder.moveRouteSubtype ==
                ObjectMoveRouteSubtype::Direct &&
            !admittedOrder.attackMove) {
            for (const ObjectId actor : admittedOrder.actors) {
                if (m_policy.attitudePromotesMove(actor) &&
                    m_ai.m_objectAI.hasOrderCapability(
                        actor, ai::ObjectAIOrderCapability::Attack)) {
                    promotedOrder.actors.push_back(actor);
                }
            }
            admittedOrder.actors.erase(
                std::remove_if(
                    admittedOrder.actors.begin(),
                    admittedOrder.actors.end(),
                    [&promotedOrder](ObjectId actor) {
                        return std::find(
                            promotedOrder.actors.begin(),
                            promotedOrder.actors.end(), actor) !=
                            promotedOrder.actors.end();
                    }),
                admittedOrder.actors.end());
            promotedOrder.attackMove =
                !promotedOrder.actors.empty();
        }
        if (admittedOrder.actors.empty() &&
            promotedOrder.actors.empty()) {
            // AIUpdateInterface silently ignores ordinary commands while an
            // AI-controlled actor is in ATTITUDE_SLEEP. SpecialPower never
            // reaches this branch solely because of sleep: named script
            // powers call their module directly in ZH.
            result = {.accepted = true, .actorCount = 0};
        } else {
            result = {.accepted = true, .actorCount = 0};
            m_ai.m_objectAI.captureOrderCapabilitySnapshot(
                m_ai.m_scriptOrderCapabilitySnapshot);
            const ai::ObjectAIOrderCapabilitySnapshot& capabilities =
                m_ai.m_scriptOrderCapabilitySnapshot;
            const auto executeBatch = [this, &result,
                                       allowHackInternetCommand,
                                       allowCombatDropCommand,
                                       &capabilities](
                const ScriptOrderIntent& batch) {
                if (batch.actors.empty()) return;
                const OrderExecutionResult batchResult =
                    OrderExecutor::executeScript(
                        m_world.m_registry, m_content.m_players, m_world.m_objects,
                        &m_world.m_objectTeams, batch,
                        allowHackInternetCommand,
                        allowCombatDropCommand, capabilities);
                result.actorCount += batchResult.actorCount;
                if (!batchResult.accepted && result.accepted) {
                    result.accepted = false;
                    result.rejection = batchResult.rejection;
                    result.message = batchResult.message;
                }
            };
            executeBatch(admittedOrder);
            executeBatch(promotedOrder);
        }
    }
    m_presentation.m_scriptOrderExecutionRecords.push_back({
        .confirmedTick = order.confirmedTick,
        .sourceScriptId = order.sourceScriptId,
        .sourceEffectOrdinal = order.sourceEffectOrdinal,
        .accepted = result.accepted,
        .rejection = result.rejection,
        .actorCount = result.actorCount,
    });
    return result;
}

} // namespace engine
