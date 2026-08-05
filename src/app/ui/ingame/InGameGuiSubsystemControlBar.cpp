#include "core/container/container_types.h"
#include "InGameGuiSubsystem.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

#include "ControlBarSchemeRuntime.h"
#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "Font.h"
#include "FontRegistry.h"
#include "Renderer.h"
#include "StringTable.h"
#include "TextureManager.h"
#include "VFS.h"
#include "Widget.h"
#include "system/AudioSubsystem.h"
#include "debug/debug.h"
#include "core/constants/Paths.h"
#include "core/constants/Colors.h"
#include "presentation/render/PresentationDefaults.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <utility>
#include "InGameGuiSubsystemDetail.h"

using namespace ingame_gui_detail;

namespace {

char commandLabelHotkey(container::StringView text) noexcept {
    for (size_t index = 0; index + 1u < text.size(); ++index) {
        if (text[index] != '&') continue;
        if (text[index + 1u] == '&') {
            ++index;
            continue;
        }
        const unsigned char value = static_cast<unsigned char>(
            text[index + 1u]);
        if (value >= 'A' && value <= 'Z') {
            return static_cast<char>(value - 'A' + 'a');
        }
        if (value >= 'a' && value <= 'z') {
            return static_cast<char>(value);
        }
    }
    return '\0';
}

} // namespace

void InGameGuiSubsystem::synchronizePlayerMoney() {
    const engine::session_query::PlayerMoneyProjection& money =
        m_gameProjection.money;
    if (m_observedPlayerMoneyRevision == money.revision) return;
    m_observedPlayerMoneyRevision = money.revision;

    // `GUI:$$$` is merely the authored WND placeholder.  The original
    // ControlBar replaces it from the local player's money model; preserving
    // that replacement here keeps WND data presentation-only and does not
    // expose PlayerRegistry or ECS to the UI thread.
    if (gui::Widget* display = m_layer.find("MoneyDisplay")) {
        display->setText("$" + std::to_string(std::max<int64_t>(0, money.cash)));
    }
}

bool InGameGuiSubsystem::activateLocalizedCommandHotkey(
    uint32_t scancode, uint32_t modifiers) {
    constexpr uint32_t blockingModifiers =
        SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI;
    const bool shiftHeld = (modifiers & SDL_KMOD_SHIFT) != 0;
    if ((modifiers & blockingModifiers) == 0 &&
        scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        const char key = static_cast<char>(
            'a' + (scancode - SDL_SCANCODE_A));
        engine::StringTable& strings = engine::StringTable::instance();
        const auto matches = [&strings, key](container::StringView label) {
            container::String localized = label.empty()
                ? container::String{} : strings.fetch(container::String{label});
            if (localized.empty()) localized.assign(label);
            return commandLabelHotkey(localized) == key;
        };
        const auto& shortcuts = m_gameProjection.specialPowerShortcuts;
        for (size_t index = 0; index < shortcuts.buttons.size(); ++index) {
            const auto& source = shortcuts.buttons[index];
            if (!source.availability.enabled || !matches(source.textLabel)) {
                continue;
            }
            gui::Widget* button = m_layer.findGenPowersShortcut(
                "ButtonCommand" + std::to_string(index + 1u));
            if (!button || !button->isEffectivelyVisible() ||
                !button->isEnabled() || !button->hasClickHandler()) {
                continue;
            }
            button->invokeClick({.modifiers = modifiers});
            return true;
        }
        const gui::Widget* scienceContext = m_layer.context(
            gui::GameWndContext::PurchaseScience);
        if (scienceContext && scienceContext->isEffectivelyVisible()) {
            const auto activateScience =
                [this, &matches](container::StringView rank,
                                 const auto& buttons) {
                    for (size_t index = 0; index < buttons.size(); ++index) {
                        const auto& source = buttons[index];
                        if (!source.visible || !source.enabled ||
                            !matches(source.textLabel)) {
                            continue;
                        }
                        gui::Widget* button = m_layer.find(
                            "ButtonRank" + container::String{rank} +
                            "Number" + std::to_string(index));
                        if (!button || !button->isEffectivelyVisible() ||
                            !button->isEnabled() || !button->onClick) {
                            continue;
                        }
                        button->onClick(*button);
                        return true;
                    }
                    return false;
                };
            const auto& science = m_gameProjection.sciencePurchase;
            if (activateScience("1", science.rank1) ||
                activateScience("3", science.rank3) ||
                activateScience("8", science.rank8)) {
                return true;
            }
        }

        // Command-button hotkeys remain available in waypoint mode. The
        // pending target later samples CommandModeInputState::queueOrders(),
        // so Guard, AttackMove and targeted abilities retain their authored
        // command family while appending a node. Stop is the sole immediate
        // replacement shortcut and is deliberately consumed without firing.
        const auto& commandUi = m_gameProjection.commandUi;
        for (size_t index = 0; index < commandUi.slots.size(); ++index) {
            const auto& source = commandUi.slots[index];
            const auto& token = commandUi.actionTokens[index];
            if (!source.visible || !token.isValid() ||
                !matches(source.textLabel)) {
                continue;
            }
            if (shiftHeld &&
                token.descriptor.kind == game::CommandButtonKind::Stop) {
                return true;
            }
            container::String widgetName{"ButtonCommand"};
            const size_t oneBased = index + 1u;
            if (oneBased < 10u) widgetName.push_back('0');
            widgetName += std::to_string(oneBased);
            gui::Widget* button = m_layer.find(widgetName);
            if (!button || !button->isEffectivelyVisible() ||
                !button->isEnabled() || !button->hasClickHandler()) {
                continue;
            }
            button->invokeClick({.modifiers = modifiers});
            return true;
        }
    }
    return m_layer.activateLocalizedCommandHotkey(
        scancode, modifiers);
}

void InGameGuiSubsystem::synchronizeScriptCommandBar() {
    const engine::session_query::InGameCommandProjection& command =
        m_gameProjection.commandUi;
    const uint64_t localRouteSelection =
        m_gameProjection.selectedLocalConstructionRouteNode
        ? m_gameProjection.selectedLocalConstructionRouteNode->previewIdentity
        : 0;
    if (m_observedConstructionProgressPermille !=
            command.constructionProgressPermille ||
        m_observedSelectedUnderConstruction !=
            command.selectedUnderConstruction ||
        m_observedLocalConstructionRouteSelection != localRouteSelection) {
        m_observedConstructionProgressPermille =
            command.constructionProgressPermille;
        m_observedSelectedUnderConstruction =
            command.selectedUnderConstruction;
        if (gui::Widget* description =
                m_layer.find("UnderConstructionDesc")) {
            const double percent = static_cast<double>(
                command.constructionProgressPermille) / 10.0;
            description->setText(command.selectedUnderConstruction &&
                    localRouteSelection == 0
                ? engine::StringTable::instance().fetchFormat(
                      "CONTROLBAR:UnderConstructionDesc", percent)
                : container::String{});
        }
    }
    if (m_observedCommandBarRevision == command.commandBarRevision &&
        m_observedLocalConstructionRouteSelection == localRouteSelection) {
        return;
    }
    m_observedCommandBarRevision = command.commandBarRevision;
    m_observedLocalConstructionRouteSelection = localRouteSelection;
    if (m_observedCommandBarSelectionRevision != command.selectionRevision) {
        m_observedCommandBarSelectionRevision = command.selectionRevision;
        m_layer.clearControlBarSelectionPresentation();
    }

    gui::Widget* selected = m_layer.find("WinUnitSelected");
    gui::Widget* portrait = m_layer.find("CameoWindow");
    // RefCode keeps CommandWindow visible while ProductionQueueWindow
    // replaces only the portrait area on the right.  The queue is an
    // auxiliary sibling, not a mutually exclusive ControlBar context.
    const bool showSelection = command.hasSelection &&
        localRouteSelection == 0 &&
        !command.multiSelection && !command.selectedPortraitImage.empty() &&
        !command.productionQueue.visible();
    if (selected) selected->setVisible(showSelection);
    if (portrait) {
        portrait->clearDrawImage(0);
        portrait->setVisible(showSelection);
        if (showSelection) {
            portrait->setDrawImage(
                0, command.selectedPortraitImage,
                command.selectedPortraitImage,
                command.selectedPortraitImage);
        }
    }
    for (size_t index = 0; index < command.upgradeCameos.size(); ++index) {
        gui::Widget* widget = m_layer.find(
            "UnitUpgrade" + std::to_string(index + 1u));
        if (!widget) continue;
        const auto& cameo = command.upgradeCameos[index];
        const bool visible = showSelection &&
            !command.selectedUnderConstruction && cameo.visible &&
            !cameo.buttonImage.empty();
        widget->setUseOverlayStates(true);
        widget->clearDrawImage(0);
        widget->setVisible(visible);
        widget->setEnabled(visible && cameo.complete);
        // An acquired UpgradeCameo is enabled, not selected/pushed. ACTIVE is
        // reserved for actual toggle state (weapon mode, overcharge, etc.).
        widget->setActive(false);
        if (visible) {
            widget->setDrawImage(
                0, cameo.buttonImage, cameo.buttonImage,
                cameo.buttonImage);
        }
    }

    auto displayedSlots = command.slots;
    const container::String& commandMarkerImage =
        gui::ingame::ControlBarSchemeRuntime::instance()
            .commandMarkerImage();
    for (size_t slot = 0; slot < displayedSlots.size(); ++slot) {
        if (!command.inventorySlots[slot] ||
            command.inventoryPassengers[slot]) {
            continue;
        }
        // ControlBar::updateSlotExitImage applies the faction scheme's
        // CommandMarkerImage to every empty EXIT_CONTAINER control.  Loaded
        // passengers replace it with their own ButtonImage; once they leave,
        // the disabled empty-frame image must be restored immediately.
        if (!commandMarkerImage.empty()) {
            displayedSlots[slot].buttonImage = commandMarkerImage;
        }
    }
    m_layer.applyScriptCommandBarSlots(displayedSlots);
    m_layer.applyCommandBarAvailability(command.availability);
    configureCommandBarSlotButtons();
    if (gui::Widget* cancel = m_layer.find("ButtonCancelConstruction")) {
        const bool showCancel = localRouteSelection != 0 ||
            command.selectedUnderConstruction || command.selectedOrderWaypoint;
        cancel->setUseOverlayStates(true);
        cancel->setVisible(showCancel);
        cancel->setEnabled(showCancel &&
            (localRouteSelection != 0 ||
             command.actionTokens[0].isValid()));
        cancel->setActive(false);
        cancel->setText({});
        cancel->setTooltip(showCancel
            ? command.slots[0].descriptionLabel : container::String{});
        cancel->clearDrawImage(0);
        cancel->onClick = {};
        if (showCancel) {
            if (!command.slots[0].buttonImage.empty()) {
                cancel->setDrawImage(
                    0, command.slots[0].buttonImage,
                    command.slots[0].buttonImage,
                    command.slots[0].buttonImage);
            }
            if (localRouteSelection != 0) {
                cancel->onClick = [this](gui::Widget&) {
                    static_cast<void>(m_logicIntents.post(
                        app::runtime::
                            CancelSelectedLocalConstructionRouteNodeIntent{},
                        m_gameProjection.sessionRevision));
                };
            } else if (command.actionTokens[0].isValid()) {
                cancel->onClick = [this](gui::Widget&) {
                    activateCommandBarSlot(0);
                };
            }
        }
    }
    synchronizePrimaryControlBarContext();
}
void InGameGuiSubsystem::synchronizeProductionQueue() {
    const engine::session_query::InGameProductionQueueProjection& projection =
        m_gameProjection.commandUi.productionQueue;
    if (m_hasObservedProductionQueueProjection &&
        m_observedProductionQueueProjection == projection &&
        m_observedProductionQueueRevision == projection.revision &&
        m_observedProductionQueueSelectionRevision ==
            m_gameProjection.commandUi.selectionRevision &&
        m_observedProductionQueueProducer == projection.producer) {
        return;
    }
    m_observedProductionQueueRevision = projection.revision;
    m_observedProductionQueueSelectionRevision =
        m_gameProjection.commandUi.selectionRevision;
    m_observedProductionQueueProducer = projection.producer;
    m_observedProductionQueueProjection = projection;
    m_hasObservedProductionQueueProjection = true;

    for (size_t index = 0;
         index < engine::session_query::InGameProductionQueueProjection::
             kMaximumVisibleItems;
         ++index) {
        container::String widgetName{"ButtonQueue"};
        const size_t oneBased = index + 1u;
        if (oneBased < 10u) widgetName.push_back('0');
        widgetName += std::to_string(oneBased);
        gui::Widget* widget = m_layer.find(widgetName);
        if (!widget) continue;
        widget->hide();
        widget->setEnabled(false);
        widget->setText({});
        widget->setCooldownClockProgress(1.0f);
        widget->setUseOverlayStates(true);
        widget->setCommandButtonChrome(false);
        widget->clearCommandBorder();
        widget->setTooltip({});
        widget->clearDrawImage(0);
        widget->onClick = {};
        widget->onPointerClick = {};
        if (index >= projection.visibleItemCount) continue;

        const engine::session_query::InGameProductionQueueItemProjection& item =
            projection.items[index];
        if (item.buttonImage.empty()) continue;
        widget->show();
        widget->setCommandButtonChrome(true);
        widget->setEnabled(item.action.isValid());
        widget->setCooldownClockProgress(
            static_cast<float>(item.progressPermille) / 1000.0f);
        widget->setText(item.queuedCount > 1u
            ? std::to_string(item.queuedCount)
            : container::String{});
        widget->setTooltip(item.textLabel);
        widget->setDrawImage(
            0, item.buttonImage, item.buttonImage, item.buttonImage);
        if (!item.action.isValid()) continue;
        widget->onPointerClick = [this, token = item.action](
            gui::Widget&, const gui::WidgetClickContext& click) {
            if (click.button != gui::WidgetPointerButton::None &&
                click.button != gui::WidgetPointerButton::Left &&
                click.button != gui::WidgetPointerButton::Right) {
                return;
            }
            const uint8_t repeatCount =
                (click.modifiers & SDL_KMOD_SHIFT) != 0 ? 5u : 1u;
            trackCommandOutcomeRequest(
                m_logicIntents.postTracked(
                    app::runtime::CancelProductionQueueItemIntent{
                        .token = token,
                        .repeatCount = repeatCount,
                    },
                    m_gameProjection.sessionRevision),
                CommandOutcomeSurface::ProductionQueue);
        };
    }
    synchronizePrimaryControlBarContext();
}

void InGameGuiSubsystem::synchronizePrimaryControlBarContext() {
    const gui::Widget* purchase = m_layer.context(
        gui::GameWndContext::PurchaseScience);
    const gui::Widget* beacon = m_layer.context(gui::GameWndContext::Beacon);
    if ((purchase && purchase->isVisible()) ||
        (beacon && beacon->isVisible())) {
        return;
    }
    if (m_gameProjection.selectedLocalConstructionRouteNode ||
        m_gameProjection.commandUi.selectedUnderConstruction) {
        const gui::Widget* underConstruction =
            m_layer.context(gui::GameWndContext::UnderConstruction);
        if (!underConstruction || !underConstruction->isVisible()) {
            m_layer.showContext(gui::GameWndContext::UnderConstruction);
        }
        m_scriptCommandBarCommandContextVisible = false;
        return;
    }
    const bool showQueue =
        m_gameProjection.commandUi.productionQueue.visible();
    const bool showCommands = m_gameProjection.commandUi.hasCommandSet;
    if (showCommands) {
        const gui::Widget* commands =
            m_layer.context(gui::GameWndContext::Command);
        if (!commands || !commands->isVisible()) {
            m_layer.showContext(gui::GameWndContext::Command);
        }
        if (gui::Widget* queue =
                m_layer.context(gui::GameWndContext::BuildQueue)) {
            if (showQueue) {
                queue->show();
            } else if (queue->isVisible()) {
                m_layer.hideContext(gui::GameWndContext::BuildQueue);
            }
        }
        m_scriptCommandBarCommandContextVisible = true;
        return;
    }
    if (showQueue) {
        const gui::Widget* queue =
            m_layer.context(gui::GameWndContext::BuildQueue);
        if (!queue || !queue->isVisible()) {
            m_layer.showContext(gui::GameWndContext::BuildQueue);
        }
        m_scriptCommandBarCommandContextVisible = false;
        return;
    }
    if (const gui::Widget* commands =
            m_layer.context(gui::GameWndContext::Command);
        commands && commands->isVisible()) {
        m_layer.hideContext(gui::GameWndContext::Command);
    }
    if (const gui::Widget* queue =
            m_layer.context(gui::GameWndContext::BuildQueue);
        queue && queue->isVisible()) {
        m_layer.hideContext(gui::GameWndContext::BuildQueue);
    }
    m_scriptCommandBarCommandContextVisible = false;
}

void InGameGuiSubsystem::synchronizeSciencePurchase() {
    const engine::session_query::SciencePurchaseProjection& projection =
        m_gameProjection.sciencePurchase;
    if (m_observedSciencePurchaseRevision == projection.revision) return;
    m_observedSciencePurchaseRevision = projection.revision;
    if (gui::Widget* general = m_layer.find("ButtonGeneral")) {
        general->setEnabled(projection.available);
    }
    if (!projection.available) {
        if (gui::Widget* purchase = m_layer.context(
                gui::GameWndContext::PurchaseScience);
            purchase && purchase->isVisible()) {
            m_layer.hideContext(gui::GameWndContext::PurchaseScience);
            synchronizePrimaryControlBarContext();
        }
    }

    const auto apply = [this](container::StringView rank,
                              const auto& buttons) {
        for (size_t index = 0; index < buttons.size(); ++index) {
            const container::String widgetName =
                "ButtonRank" + container::String{rank} + "Number" +
                std::to_string(index);
            gui::Widget* widget = m_layer.find(widgetName);
            if (!widget) continue;
            const engine::session_query::SciencePurchaseButtonProjection& button =
                buttons[index];
            const bool visible = button.visible && !button.buttonImage.empty();
            widget->setVisible(visible);
            widget->setEnabled(visible && button.enabled);
            widget->setActive(visible && button.acquired);
            widget->setText(visible && !button.acquired &&
                    button.cost > 0
                ? std::to_string(button.cost)
                : container::String{});
            // PURCHASE_SCIENCE CommandButtons author neither TextLabel nor
            // DescriptLabel in shipped CommandButton.ini, so the Science
            // block's DisplayName/Description (routed through
            // PlayerUiProjection) is the only text there is. RefCode's
            // ControlBarPopupDescription shows the science name and its
            // description on separate lines; this path owns a single tooltip
            // string, so compose the two resolved lines here rather than
            // dropping the name. Both are string-table labels, and
            // fetchOrFallback passes an unresolved value through unchanged.
            if (!visible) {
                widget->setTooltip(container::String{});
            } else {
                engine::StringTable& strings =
                    engine::StringTable::instance();
                container::String tooltip;
                if (!button.textLabel.empty()) {
                    tooltip = strings.fetchOrFallback(
                        button.textLabel, button.textLabel);
                }
                if (!button.descriptionLabel.empty()) {
                    const container::String description =
                        strings.fetchOrFallback(
                            button.descriptionLabel,
                            button.descriptionLabel);
                    if (!description.empty()) {
                        if (!tooltip.empty()) tooltip += "\n\n";
                        tooltip += description;
                    }
                }
                widget->setTooltip(std::move(tooltip));
            }
            widget->clearDrawImage(0);
            if (visible) {
                widget->setDrawImage(
                    0, button.buttonImage, button.buttonImage,
                    button.buttonImage);
            }
            if (!visible || !button.enabled ||
                button.science.empty() || button.buttonStableId == 0) {
                widget->onClick = {};
                continue;
            }
            widget->onClick = [this,
                               commandButtonName = button.commandButtonName,
                               science = button.science,
                               stableId = button.buttonStableId](gui::Widget&) {
                trackCommandOutcomeRequest(
                    m_logicIntents.postTracked(
                        app::runtime::PurchaseScienceIntent{
                            .commandButtonName = commandButtonName,
                            .science = science,
                            .buttonStableId = stableId,
                        },
                        m_gameProjection.sessionRevision),
                    CommandOutcomeSurface::SciencePurchase);
            };
        }
    };
    apply("1", projection.rank1);
    apply("3", projection.rank3);
    apply("8", projection.rank8);
    if (gui::Widget* points =
            m_layer.find("StaticTextRankPointsAvailable")) {
        points->setText(std::to_string(projection.purchasePoints));
    }
    if (gui::Widget* progress = m_layer.find("ProgressBarExperience")) {
        progress->setSliderValue(std::clamp(
            projection.rankProgress, 0.0f, 1.0f));
    }
    if (gui::Widget* title = m_layer.find("StaticTextTitle")) {
        const container::String key =
            "SCIENCE:Rank" + std::to_string(projection.rankLevel);
        engine::StringTable& strings = engine::StringTable::instance();
        title->setText(strings.exists(key) ? strings.fetch(key)
                                           : container::String{});
    }
}

void InGameGuiSubsystem::consumeCommandOutcomes() {
    const engine::CommandOutcomeProjection& projection =
        m_gameProjection.commandOutcomes;
    if (projection.revision == 0 ||
        projection.revision <= m_observedCommandOutcomeRevision) {
        return;
    }

    for (const engine::CommandOutcome& outcome : projection.records) {
        if (outcome.revision <= m_observedCommandOutcomeRevision) {
            continue;
        }
        const auto tracked = std::find_if(
            m_trackedCommandOutcomeRequests.begin(),
            m_trackedCommandOutcomeRequests.end(),
            [&outcome](const TrackedCommandOutcomeRequest& request) {
                return request.requestSequence == outcome.requestSequence;
            });
        if (tracked != m_trackedCommandOutcomeRequests.end() &&
            outcome.terminal()) {
            refreshCommandOutcomeSurface(tracked->surface);
            m_trackedCommandOutcomeRequests.erase(tracked);
        } else if (outcome.terminal() && outcome.buttonStableId != 0) {
            // CommandMap hotkeys enter the same intent mailbox without going
            // through this WND callback.  They still carry the immutable
            // button identity in their receipt, so refresh the command bar
            // when the currently projected control owns that identity.
            const auto matchingControl = std::find_if(
                m_gameProjection.commandUi.actionTokens.begin(),
                m_gameProjection.commandUi.actionTokens.end(),
                [&outcome](const engine::session_query::
                    InGameCommandActionToken& token) {
                    return token.isValid() &&
                        token.descriptor.stableId == outcome.buttonStableId;
                });
            if (matchingControl !=
                m_gameProjection.commandUi.actionTokens.end()) {
                refreshCommandOutcomeSurface(
                    CommandOutcomeSurface::CommandBar);
            }
        }
        if (outcome.state != engine::CommandOutcomeState::Rejected) continue;
        container::String feedback;
        switch (outcome.reason) {
        case engine::CommandOutcomeReason::CancelledByUser:
        case engine::CommandOutcomeReason::CancelledBySelectionChange:
        case engine::CommandOutcomeReason::SupersededByLocalMode:
            // Deliberate local cancellation is already visible through the
            // cursor/mode transition and should not produce an error banner.
            continue;
        case engine::CommandOutcomeReason::NetworkNotReady:
        case engine::CommandOutcomeReason::ExpiredBeforeConfirmation:
        case engine::CommandOutcomeReason::BackendTimedOut:
            feedback = "Command confirmation unavailable";
            break;
        case engine::CommandOutcomeReason::StaleSession:
        case engine::CommandOutcomeReason::GameNotRunning:
        case engine::CommandOutcomeReason::SourceBecameUnavailable:
            feedback = "Game state changed";
            break;
        case engine::CommandOutcomeReason::StaleSelection:
        case engine::CommandOutcomeReason::SelectionMismatch:
        case engine::CommandOutcomeReason::RouterInvalidSelection:
            feedback = "Selection changed";
            break;
        case engine::CommandOutcomeReason::QueueChanged:
            feedback = "Queue updated; try again";
            break;
        case engine::CommandOutcomeReason::DescriptorChanged:
            feedback = "Command layout changed";
            break;
        case engine::CommandOutcomeReason::AvailabilityChanged:
        case engine::CommandOutcomeReason::SlotUnavailable:
        case engine::CommandOutcomeReason::RouterUnavailable:
            feedback = "Command conditions changed";
            break;
        case engine::CommandOutcomeReason::SingleUseConsumed:
            feedback = "Command already used";
            break;
        case engine::CommandOutcomeReason::ScienceUnavailable:
            feedback = "Science is unavailable";
            break;
        case engine::CommandOutcomeReason::LocalPresentationRejected:
        case engine::CommandOutcomeReason::RouterCompositionRejected:
            feedback = "Target or placement is invalid";
            break;
        default:
            feedback = "Command unavailable";
            break;
        }
        if (!feedback.empty()) {
            enqueueCommandOutcomeFeedback(
                outcome.requestSequence, std::move(feedback));
        }
    }
    m_observedCommandOutcomeRevision = projection.revision;
    if (m_gameProjection.sessionRevision != 0) {
        static_cast<void>(m_logicIntents.post(
            app::runtime::AcknowledgeCommandOutcomesIntent{
                .revision = projection.revision},
            m_gameProjection.sessionRevision));
    }
}

void InGameGuiSubsystem::trackCommandOutcomeRequest(
    std::optional<uint64_t> requestSequence,
    CommandOutcomeSurface surface) {
    if (!requestSequence || *requestSequence == 0) return;
    const auto duplicate = std::find_if(
        m_trackedCommandOutcomeRequests.begin(),
        m_trackedCommandOutcomeRequests.end(),
        [sequence = *requestSequence](const TrackedCommandOutcomeRequest& request) {
            return request.requestSequence == sequence;
        });
    if (duplicate != m_trackedCommandOutcomeRequests.end()) return;
    m_trackedCommandOutcomeRequests.push_back({
        .requestSequence = *requestSequence,
        .surface = surface,
    });
}

void InGameGuiSubsystem::refreshCommandOutcomeSurface(
    CommandOutcomeSurface surface) noexcept {
    switch (surface) {
    case CommandOutcomeSurface::CommandBar:
        m_observedCommandBarRevision = 0;
        m_observedCommandBarSelectionRevision = UINT64_MAX;
        m_observedLocalConstructionRouteSelection = UINT64_MAX;
        break;
    case CommandOutcomeSurface::ProductionQueue:
        m_observedProductionQueueRevision = UINT64_MAX;
        m_observedProductionQueueSelectionRevision = UINT64_MAX;
        break;
    case CommandOutcomeSurface::SciencePurchase:
        m_observedSciencePurchaseRevision = UINT64_MAX;
        break;
    case CommandOutcomeSurface::SpecialPowerShortcut:
        m_observedSpecialPowerShortcutRevision = UINT64_MAX;
        break;
    }
}

void InGameGuiSubsystem::enqueueCommandOutcomeFeedback(
    uint64_t requestSequence, container::String text) {
    if (requestSequence == 0 || text.empty()) return;
    if (m_commandOutcomeFeedbackRequest == requestSequence) return;
    const auto duplicate = std::find_if(
        m_commandOutcomeFeedbackQueue.begin(),
        m_commandOutcomeFeedbackQueue.end(),
        [requestSequence](const CommandOutcomeFeedback& value) {
            return value.requestSequence == requestSequence;
        });
    if (duplicate != m_commandOutcomeFeedbackQueue.end()) return;
    m_commandOutcomeFeedbackQueue.push_back({
        .requestSequence = requestSequence,
        .text = std::move(text),
    });
    if (m_commandOutcomeFeedbackRequest != 0) return;
    const CommandOutcomeFeedback& next =
        m_commandOutcomeFeedbackQueue.front();
    m_commandOutcomeFeedbackRequest = next.requestSequence;
    m_commandOutcomeFeedback = next.text;
    m_commandOutcomeFeedbackStartedAt = std::chrono::steady_clock::now();
}

void InGameGuiSubsystem::advanceCommandOutcomeFeedback() {
    if (m_commandOutcomeFeedbackRequest == 0) return;
    constexpr auto kFeedbackLifetime = std::chrono::milliseconds{2200};
    const auto elapsed = std::chrono::steady_clock::now() -
        m_commandOutcomeFeedbackStartedAt;
    if (elapsed < kFeedbackLifetime) return;
    if (!m_commandOutcomeFeedbackQueue.empty()) {
        m_commandOutcomeFeedbackQueue.pop_front();
    }
    if (m_commandOutcomeFeedbackQueue.empty()) {
        m_commandOutcomeFeedbackRequest = 0;
        m_commandOutcomeFeedback.clear();
        m_commandOutcomeFeedbackStartedAt = {};
        return;
    }
    const CommandOutcomeFeedback& next =
        m_commandOutcomeFeedbackQueue.front();
    m_commandOutcomeFeedbackRequest = next.requestSequence;
    m_commandOutcomeFeedback = next.text;
    m_commandOutcomeFeedbackStartedAt = std::chrono::steady_clock::now();
}

void InGameGuiSubsystem::synchronizeSpecialPowerShortcuts() {
    const engine::session_query::SpecialPowerShortcutProjection& projection =
        m_gameProjection.specialPowerShortcuts;
    if (m_loadedSpecialPowerShortcutWindow != projection.windowName) {
        m_loadedSpecialPowerShortcutWindow = projection.windowName;
        m_observedSpecialPowerShortcutRevision = UINT64_MAX;
        if (projection.available) {
            static_cast<void>(m_layer.loadGenPowersShortcut(
                projection.windowName));
        } else {
            m_layer.clearGenPowersShortcut();
        }
    }
    if (m_observedSpecialPowerShortcutRevision == projection.revision) return;
    m_observedSpecialPowerShortcutRevision = projection.revision;

    constexpr size_t kMaximumShortcutButtons = 16u;
    for (size_t index = 0; index < kMaximumShortcutButtons; ++index) {
        const container::String suffix = std::to_string(index + 1u);
        if (gui::Widget* parent = m_layer.findGenPowersShortcut(
                "ButtonParent" + suffix)) {
            parent->setVisible(false);
        }
        if (gui::Widget* button = m_layer.findGenPowersShortcut(
                "ButtonCommand" + suffix)) {
            button->setVisible(false);
            button->setEnabled(false);
            button->setText({});
            button->setTooltip({});
            button->setCooldownClockProgress(1.0f);
            button->clearDrawImage(0);
            button->setPushButtonPressSound({});
            button->onClick = {};
        }
    }
    gui::Widget* shortcutParent = m_layer.findGenPowersShortcut(
        "GenPowersShortcutBarParent");
    if (!projection.available) {
        if (shortcutParent) shortcutParent->hide();
        return;
    }
    if (shortcutParent) shortcutParent->show();
    const size_t count = std::min(
        projection.buttons.size(), kMaximumShortcutButtons);
    for (size_t index = 0; index < count; ++index) {
        const engine::session_query::SpecialPowerShortcutButtonProjection& source =
            projection.buttons[index];
        const container::String suffix = std::to_string(index + 1u);
        if (gui::Widget* parent = m_layer.findGenPowersShortcut(
                "ButtonParent" + suffix)) {
            parent->setVisible(true);
        }
        gui::Widget* button = m_layer.findGenPowersShortcut(
            "ButtonCommand" + suffix);
        if (!button) continue;
        if (source.buttonImage.empty()) continue;
        button->setVisible(true);
        button->setEnabled(source.availability.enabled);
        button->setCooldownClockProgress(
            static_cast<float>(source.availability.cooldown.readyPermille) /
            1000.0f);
        // A shortcut is one ability button even when several selected/source
        // objects can provide it. Provider multiplicity is routing state, not
        // a production-queue count, and must never leak as a number over the
        // ability cameo.
        button->setText({});
        button->setTooltip(source.descriptionLabel.empty()
            ? source.textLabel : source.descriptionLabel);
        button->setDrawImage(
            0, source.buttonImage, source.buttonImage,
            source.buttonImage);
        // ControlBar::setControlCommand() first assigns the ordinary command
        // cameo click; the Generals shortcut materializer then replaces it.
        button->setPushButtonPressSound("GUIGenShortcutClick");
        if (!source.availability.enabled ||
            source.commandButtonName.empty() ||
            source.buttonStableId == 0u) {
            continue;
        }
        button->onPointerClick = [
            this, commandButtonName = source.commandButtonName,
            stableId = source.buttonStableId](
                gui::Widget&, const gui::WidgetClickContext& click) {
            trackCommandOutcomeRequest(
                m_logicIntents.postTracked(
                    app::runtime::ActivateSpecialPowerShortcutIntent{
                        .commandButtonName = commandButtonName,
                        .buttonStableId = stableId,
                        .queued =
                            (click.modifiers & SDL_KMOD_SHIFT) != 0,
                    },
                    m_gameProjection.sessionRevision),
                CommandOutcomeSurface::SpecialPowerShortcut);
        };
    }
}

void InGameGuiSubsystem::submitBeaconEditorText(container::StringView text) {
    static_cast<void>(m_logicIntents.post(
        app::runtime::SubmitBeaconTextIntent{
            .text = container::String{text}},
        m_gameProjection.sessionRevision));
}

void InGameGuiSubsystem::synchronizeBeaconEditor() {
    const engine::session_query::InGameBeaconProjection& beacon =
        m_gameProjection.commandUi.beacon;

    // ZH exposes Beacon controls only in an active multiplayer session.
    // Single-player/challenge/replay must consume the meta command as a
    // no-op, not open an empty Beacon context from the ControlBar button.
    // Network Beacon creation has no authoritative command/lifecycle owner
    // yet. Do not advertise an enabled control whose click is a no-op.
    constexpr bool beaconControlsAvailable = false;
    if (gui::Widget* place = m_layer.find("ButtonPlaceBeacon")) {
        place->setVisible(beaconControlsAvailable);
        place->setEnabled(beaconControlsAvailable);
    }
    if (!beaconControlsAvailable) {
        m_layer.hideContext(gui::GameWndContext::Beacon);
    }

    gui::Widget* edit = m_layer.find("EditBeaconText");
    gui::Widget* label = m_layer.find("StaticTextBeaconLabel");
    gui::Widget* clear = m_layer.find("ButtonClearBeaconText");
    if (!beacon.isBeacon) {
        if (m_beaconEditorContextVisible) {
            m_layer.hideContext(gui::GameWndContext::Beacon);
            m_layer.clearControlBarInteractionState();
            if (m_gameProjection.commandUi.hasCommandSet)
                m_layer.showContext(gui::GameWndContext::Command);
        }
        m_beaconEditorContextVisible = false;
        m_beaconEditorObject = engine::INVALID_OBJECT_ID;
        m_beaconEditorCaptionRevision = 0;
        return;
    }

    const bool enteringBeaconContext = !m_beaconEditorContextVisible;
    if (enteringBeaconContext) {
        m_layer.showContext(gui::GameWndContext::Beacon);
        m_beaconEditorContextVisible = true;
    }
    const bool editorWasVisible = edit && edit->isVisible();
    if (edit) edit->setVisible(beacon.locallyControlled);
    if (label) label->setVisible(beacon.locallyControlled);
    if (clear) clear->setVisible(beacon.locallyControlled);
    if (!beacon.locallyControlled) {
        // A hidden entry must not retain keyboard focus after selection moves
        // from a local Beacon to an observed enemy/neutral Beacon.
        if (editorWasVisible) m_layer.clearControlBarInteractionState();
        return;
    }

    const uint64_t revision = beacon.revision;
    const bool objectChanged =
        m_beaconEditorObject != beacon.object;
    const bool captionChanged = m_beaconEditorCaptionRevision != revision;
    if (edit && (objectChanged || captionChanged)) {
        // A different selected Beacon must never inherit the previous
        // Beacon's pending entry text.  For the same Beacon, retain active
        // typing across the confirmed command round-trip, but deliberately
        // leave the old revision pending so the authoritative value is
        // synchronized as soon as the entry loses focus.
        if (objectChanged || !edit->isFocused()) {
            edit->setEditText(beacon.caption);
            edit->setCursorPosition(
                static_cast<int>(edit->getEditText().size()));
            m_beaconEditorCaptionRevision = revision;
        }
    }
    m_beaconEditorObject = beacon.object;
    if (enteringBeaconContext && edit) {
        static_cast<void>(m_layer.focusControlBarWidget("EditBeaconText"));
    }
}

void InGameGuiSubsystem::configureCommandBarSlotButtons() {
    for (size_t slot = 0;
         slot < engine::script::ScriptCommandBarPresentationConsumer::kSlotCount;
         ++slot) {
        container::String widgetName{"ButtonCommand"};
        const size_t oneBased = slot + 1u;
        if (oneBased < 10u) widgetName.push_back('0');
        widgetName += std::to_string(oneBased);
        if (gui::Widget* widget = m_layer.find(widgetName)) {
            widget->onClick = {};
            widget->onPointerClick = [this, slot](
                gui::Widget&, const gui::WidgetClickContext& click) {
                if (click.button == gui::WidgetPointerButton::Right ||
                    click.button == gui::WidgetPointerButton::Middle) {
                    return;
                }
                activateCommandBarSlot(
                    slot,
                    (click.modifiers & SDL_KMOD_SHIFT) != 0 ? 5u : 1u,
                    (click.modifiers & SDL_KMOD_SHIFT) != 0);
            };
        }
    }
}

void InGameGuiSubsystem::activateCommandBarSlot(
    size_t slot, uint8_t repeatCount, bool queued) {
    const auto& tokens = m_gameProjection.commandUi.actionTokens;
    if (slot >= tokens.size() || !tokens[slot].isValid()) return;
    trackCommandOutcomeRequest(
        m_logicIntents.postTracked(
            app::runtime::ActivateCommandBarSlotIntent{
                .token = tokens[slot],
                .repeatCount = std::clamp<uint8_t>(repeatCount, 1u, 5u),
                .queued = queued},
            m_gameProjection.sessionRevision),
        CommandOutcomeSurface::CommandBar);
}

void InGameGuiSubsystem::releaseScriptPopupPause() {
    if (m_scriptPopupPauseApplied) {
        static_cast<void>(m_logicIntents.post(
            app::runtime::SetScriptPresentationPausedIntent{.paused = false},
            m_gameProjection.sessionRevision));
    }
    m_scriptPopupPauseApplied = false;
}

void InGameGuiSubsystem::requestInGameMenuPause() {
    if (m_inGameMenuPauseApplied) return;
    m_inGameMenuPauseApplied = m_logicIntents.post(
        app::runtime::SetLocalPauseSourceIntent{
            .source = engine::LocalPauseSource::InGameMenu,
            .paused = true,
        },
        m_gameProjection.sessionRevision);
}

void InGameGuiSubsystem::releaseInGameMenuPause() {
    if (!m_inGameMenuPauseApplied) return;
    static_cast<void>(m_logicIntents.post(
        app::runtime::SetLocalPauseSourceIntent{
            .source = engine::LocalPauseSource::InGameMenu,
            .paused = false,
        },
        m_gameProjection.sessionRevision));
    m_inGameMenuPauseApplied = false;
}

void InGameGuiSubsystem::configureControlBarButtons() {
    configureCommandBarSlotButtons();
    if (auto* options = m_layer.find("ButtonOptions")) {
        options->onClick = [this](gui::Widget&) { openQuitMenu(); };
    }
    if (auto* general = m_layer.find("ButtonGeneral")) {
        general->onClick = [this](gui::Widget&) {
            synchronizeSciencePurchase();
            gui::Widget* purchase = m_layer.context(
                gui::GameWndContext::PurchaseScience);
            if (purchase && purchase->isVisible()) {
                m_layer.hideContext(gui::GameWndContext::PurchaseScience);
                synchronizePrimaryControlBarContext();
                return;
            }
            if (m_gameProjection.sciencePurchase.available)
                m_layer.showContext(gui::GameWndContext::PurchaseScience);
        };
    }
    if (auto* exit = m_layer.find("ButtonExit")) {
        exit->onClick = [this](gui::Widget&) {
            m_layer.hideAllContexts();
        };
    }
    if (auto* large = m_layer.find("ButtonLarge")) {
        large->onClick = [this](gui::Widget&) {
            m_layer.toggleControlBarCompact();
        };
    }
    if (auto* beacon = m_layer.find("ButtonPlaceBeacon")) {
        beacon->onClick = [this](gui::Widget&) {
            if (!m_gameProjection.startInfo ||
                !m_gameProjection.startInfo->network.enabled) {
                return;
            }
            // The actual network Beacon create command is deliberately
            // outside the current product boundary (Z-005). Consume the
            // button without opening an editor for a Beacon that does not yet
            // exist; an existing selected Beacon is synchronized separately.
        };
    }
    if (auto* edit = m_layer.find("EditBeaconText")) {
        edit->onClick = [this](gui::Widget& widget) {
            submitBeaconEditorText(widget.getEditText());
        };
    }
    if (auto* clear = m_layer.find("ButtonClearBeaconText")) {
        clear->onClick = [this](gui::Widget&) {
            if (gui::Widget* edit = m_layer.find("EditBeaconText")) {
                edit->setEditText({});
                edit->setCursorPosition(0);
            }
            // RefCode only clears GadgetTextEntry here.  The empty caption
            // enters MSG_SET_BEACON_TEXT if/when GEM_EDIT_DONE is raised by
            // Return, exactly like any other edited value.
        };
    }
    if (auto* communicator = m_layer.find("PopupCommunicator")) {
        communicator->onClick = [this](gui::Widget&) {
            // ControlBarSystem routes this button to ToggleDiplomacy(FALSE),
            // not to the separate WOL buddy PopupCommunicator layout.
            if (m_layer.hasOverlay()) {
                m_layer.closeOverlay();
            } else {
                openOverlay(gui::GameWndOverlay::Diplomacy);
            }
        };
    }
    if (auto* idleWorker = m_layer.find("ButtonIdleWorker")) {
        idleWorker->onClick = [this](gui::Widget&) {
            static_cast<void>(m_logicIntents.post(
                app::runtime::ApplyLocalSelectionShortcutIntent{
                    .shortcut = engine::selection::LocalSelectionShortcut::NextIdleWorker},
                m_gameProjection.sessionRevision));
        };
    }
    if (auto* underAttack = m_layer.find("WinUAttack")) {
        underAttack->onClick = [this](gui::Widget&) {
            if (m_underAttackHandler) m_underAttackHandler();
        };
    }
}
