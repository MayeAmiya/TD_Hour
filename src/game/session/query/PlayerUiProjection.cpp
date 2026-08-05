#include "game/session/query/PlayerUiProjection.h"

#include "game/command/CommandButtonStore.h"
#include "game/command/CommandSetStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/RankInfoCatalog.h"
#include "game/player/FactionTemplate.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>

namespace engine::session_query {
namespace {

[[nodiscard]] bool containsScience(
    const container::Vector<container::String>& values,
    container::StringView science) {
    return std::binary_search(values.begin(), values.end(), science);
}

[[nodiscard]] const game::CommandButtonTemplate*
shortcutPresentationButton(
    const engine::GameContentSnapshot& content,
    const engine::PlayerTemplatePresentationData& factionPresentation,
    const engine::PlayerState& player,
    const game::CommandButtonTemplate& shortcut) {
    if (shortcut.sciences.empty()) return &shortcut;

    const container::String* bestScience = nullptr;
    for (const container::String& science : shortcut.sciences) {
        if (!containsScience(player.sciences.known, science)) break;
        bestScience = &science;
    }
    if (!bestScience) return &shortcut;

    for (const container::String& commandSetName :
         factionPresentation.purchaseScienceCommandSets) {
        const game::CommandSetTemplate* commandSet =
            commandSetName.empty() ? nullptr
                : content.findCommandSet(commandSetName);
        if (!commandSet) continue;
        for (const container::String& commandName : commandSet->commands) {
            const game::CommandButtonTemplate* purchase = commandName.empty()
                ? nullptr
                : content.findCommandButton(commandName);
            if (!purchase || purchase->descriptor.kind !=
                    game::CommandButtonKind::PurchaseScience ||
                purchase->sciences.empty()) {
                continue;
            }
            if (purchase->sciences.front() == *bestScience) return purchase;
        }
    }
    return &shortcut;
}

} // namespace

SessionUiContentProjection PlayerUiQueryPort::content() const noexcept {
    const GameContentSnapshot& content =
        m_content->m_contentSnapshot;
    return {
        .mappedImageLayers = content.mappedImageContentLayerSnapshot(),
        .mapStrings = content.mapStringContentLayerSnapshot(),
    };
}

PlayerPowerProjection PlayerUiQueryPort::power() const noexcept {
    const engine::PlayerState* player = m_content->m_players.localPlayer();
    if (!player) return {};
    return {
        .revision = player->revisions.energy,
        .production = std::max(0, player->energy.effectiveProduction(
            m_confirmedTick)),
        .consumption = std::max(0, player->energy.consumption),
        .sufficient = player->energy.hasSufficientPower(m_confirmedTick),
    };
}

PlayerMoneyProjection PlayerUiQueryPort::money() const noexcept {
    const engine::PlayerState* player = m_content->m_players.localPlayer();
    if (!player) return {};
    return {
        .revision = player->revisions.economy,
        .cash = player->cash,
    };
}

SciencePurchaseProjection PlayerUiQueryPort::sciencePurchase(
    uint64_t uiRevision) const {
    SciencePurchaseProjection projection;
    const GameSessionContentStartState& sessionContent =
        *m_content;
    const GameContentSnapshot& content = sessionContent.m_contentSnapshot;
    const engine::PlayerState* player = sessionContent.m_players.localPlayer();
    const std::optional<engine::PlayerTemplatePresentationData> faction = player
        ? m_ruleset.factionPresentation(player->faction)
        : std::nullopt;
    const engine::ScienceCatalog* sciences =
        content.scienceCatalog();
    if (!player || !faction || !sciences || !sciences->isLoaded()) {
        return projection;
    }

    projection.purchasePoints = player->sciences.purchasePoints;
    projection.rankLevel = player->progress.rankLevel;
    if (const engine::RankInfoCatalog* ranks =
            content.rankInfoCatalog()) {
        const engine::RankInfoDefinition* current = ranks->find(
            static_cast<uint32_t>(std::max(1, player->progress.rankLevel)));
        const engine::RankInfoDefinition* next = ranks->find(
            static_cast<uint32_t>(std::max(1, player->progress.rankLevel) + 1));
        if (current && next &&
            next->skillPointsNeeded > current->skillPointsNeeded) {
            const int64_t earned = std::clamp<int64_t>(
                player->progress.skillPoints - current->skillPointsNeeded,
                0, static_cast<int64_t>(next->skillPointsNeeded) -
                    current->skillPointsNeeded);
            projection.rankProgress = static_cast<float>(earned) /
                static_cast<float>(next->skillPointsNeeded -
                                   current->skillPointsNeeded);
        } else if (current && !next) {
            projection.rankProgress = 1.0f;
        }
    }
    projection.revision = uiRevision;

    container::Array<const game::CommandSetTemplate*, 3> commandSets{};
    for (size_t index = 0; index < commandSets.size(); ++index) {
        const container::String& name =
            faction->purchaseScienceCommandSets[index];
        commandSets[index] = name.empty()
            ? nullptr : content.findCommandSet(name);
    }
    if (std::any_of(commandSets.begin(), commandSets.end(),
                    [](const game::CommandSetTemplate* value) {
                        return value == nullptr;
                    })) {
        return projection;
    }
    projection.available = true;

    const auto fill = [&](const game::CommandSetTemplate& commandSet,
                          auto& output) {
        const size_t count = std::min(output.size(),
                                      commandSet.commands.size());
        for (size_t index = 0; index < count; ++index) {
            const container::String& commandName =
                commandSet.commands[index];
            const game::CommandButtonTemplate* button = commandName.empty()
                ? nullptr
                : content.findCommandButton(commandName);
            if (!button ||
                button->descriptor.kind !=
                    game::CommandButtonKind::PurchaseScience ||
                !button->descriptor.userActivatable()) {
                continue;
            }
            container::Vector<container::StringView> authoredSciences;
            if (!button->sciences.empty()) {
                authoredSciences.reserve(button->sciences.size());
                for (const container::String& science : button->sciences) {
                    if (!science.empty()) authoredSciences.push_back(science);
                }
            } else if (!button->science.empty()) {
                authoredSciences.push_back(button->science);
            }
            const engine::ScienceDefinition* rootDefinition =
                !authoredSciences.empty()
                ? sciences->find(authoredSciences.front()) : nullptr;
            if (!rootDefinition || !rootDefinition->grantable) continue;

            SciencePurchaseButtonProjection& target = output[index];
            target.buttonStableId = button->descriptor.stableId;
            target.commandButtonName = button->name;
            target.science.clear();
            target.buttonImage = button->buttonImage;
            target.textLabel = button->textLabel;
            target.descriptionLabel = button->descriptionLabel;
            target.acquired = !authoredSciences.empty();
            const engine::ScienceDefinition* nextScience = nullptr;
            for (const container::StringView scienceName : authoredSciences) {
                const engine::ScienceDefinition* candidate =
                    sciences->find(scienceName);
                if (!candidate || !candidate->grantable) continue;
                if (containsScience(player->sciences.known,
                                    candidate->name)) {
                    continue;
                }
                target.acquired = false;
                nextScience = candidate;
                break;
            }
            const bool hidden = containsScience(
                player->sciences.hidden, rootDefinition->name);
            const bool hasRootSciences = std::all_of(
                rootDefinition->rootSciences.begin(),
                rootDefinition->rootSciences.end(),
                [player](const container::String& root) {
                    return containsScience(player->sciences.known, root);
                });
            target.visible = !hidden && hasRootSciences;
            if (target.visible && nextScience) {
                target.science = nextScience->name;
                target.cost = nextScience->purchasePointCost;
                target.enabled = sessionContent.m_players.canPurchaseScience(
                    player->id, *nextScience);
            }

            // RefCode ControlBarPopupDescription selects the first authored
            // science the player does not yet own (falling back to the last
            // authored entry once all are owned) and then lets
            // ScienceStore::getNameAndDescription overwrite the CommandButton
            // TextLabel/DescriptLabel for a PURCHASE_SCIENCE button. That is
            // the only source of promotion name/description text, so without
            // it the picker shows nothing at all.
            //
            // Deviation, deliberately in the player's favour: RefCode
            // overwrites unconditionally and therefore blanks the tooltip for
            // the 12 of 96 Science blocks that author neither key. Here an
            // empty science label keeps the CommandButton label instead of
            // erasing it.
            const engine::ScienceDefinition* describedScience = nextScience;
            if (!describedScience) {
                for (auto it = authoredSciences.rbegin();
                     it != authoredSciences.rend(); ++it) {
                    if (const engine::ScienceDefinition* candidate =
                            sciences->find(*it)) {
                        describedScience = candidate;
                        break;
                    }
                }
            }
            if (describedScience) {
                if (!describedScience->displayNameLabel.empty()) {
                    target.textLabel = describedScience->displayNameLabel;
                }
                if (!describedScience->descriptionLabel.empty()) {
                    target.descriptionLabel =
                        describedScience->descriptionLabel;
                }
            }
        }
    };
    fill(*commandSets[0], projection.rank1);
    fill(*commandSets[1], projection.rank3);
    fill(*commandSets[2], projection.rank8);
    return projection;
}

SpecialPowerShortcutProjection
PlayerUiQueryPort::specialPowerShortcuts(
    uint64_t uiRevision) const {
    SpecialPowerShortcutProjection projection;
    const GameSessionContentStartState& sessionContent =
        *m_content;
    const GameContentSnapshot& content = sessionContent.m_contentSnapshot;
    const engine::PlayerState* player = sessionContent.m_players.localPlayer();
    const std::optional<engine::PlayerTemplatePresentationData> faction = player
        ? m_ruleset.factionPresentation(player->faction)
        : std::nullopt;
    if (!player || !faction ||
        faction->specialPowerShortcutCommandSet.empty() ||
        faction->specialPowerShortcutWindow.empty() ||
        faction->specialPowerShortcutButtonCount <= 0) {
        return projection;
    }
    const game::CommandSetTemplate* commandSet =
        content.findCommandSet(
            faction->specialPowerShortcutCommandSet);
    if (!commandSet) return projection;

    projection.windowName =
        faction->specialPowerShortcutWindow;
    projection.revision = uiRevision;
    const size_t authoredCount = static_cast<size_t>(std::clamp(
        faction->specialPowerShortcutButtonCount, 0, 16));
    const size_t count = std::min(authoredCount, commandSet->commands.size());
    for (size_t index = 0; index < count; ++index) {
        const container::String& commandName = commandSet->commands[index];
        const game::CommandButtonTemplate* button = commandName.empty()
            ? nullptr : content.findCommandButton(commandName);
        if (!button || (button->descriptor.kind !=
                game::CommandButtonKind::SpecialPowerFromShortcut &&
            button->descriptor.kind !=
                game::CommandButtonKind::SpecialPowerConstructFromShortcut &&
            button->descriptor.kind !=
                game::CommandButtonKind::SelectAllUnitsOfType) ||
            !button->descriptor.userActivatable()) {
            continue;
        }

        if (button->descriptor.kind ==
                game::CommandButtonKind::SelectAllUnitsOfType) {
            InGameCommandSlotAvailability availability;
            availability.visible = true;
            availability.enabled = !button->object.empty();
            availability.reason = availability.enabled
                ? InGameCommandAvailabilityReason::None
                : InGameCommandAvailabilityReason::MissingContentReference;
            availability.revision = uiRevision;
            projection.buttons.push_back({
                .buttonStableId = button->descriptor.stableId,
                .commandButtonName = button->name,
                .buttonImage = button->buttonImage,
                .textLabel = button->textLabel,
                .descriptionLabel = button->descriptionLabel,
                .availability = availability,
            });
            continue;
        }

        const engine::session_query::InGameCommandAggregateAvailability provider =
            engine::session_query::evaluateInGameShortcutAvailability(
                {
                    .source = m_source,
                    .commands = m_commands,
                    .economy = m_economy,
                    .confirmedTick = m_confirmedTick,
                    .logicFramesPerSecond = m_logicFramesPerSecond,
                },
                player->id, *button);
        if (!provider.actor) continue;
        const game::CommandButtonTemplate* presentationButton =
            shortcutPresentationButton(
                content, *faction, *player, *button);
        projection.buttons.push_back({
            .buttonStableId = button->descriptor.stableId,
            .commandButtonName = button->name,
            .buttonImage = presentationButton->buttonImage,
            .textLabel = presentationButton->textLabel,
            .descriptionLabel = presentationButton->descriptionLabel,
            .sourceObject = provider.actor,
            .availability = provider.availability,
            .availableSourceCount = provider.availableActorCount,
        });
    }
    projection.available = !projection.buttons.empty();
    return projection;
}


} // namespace engine::session_query
