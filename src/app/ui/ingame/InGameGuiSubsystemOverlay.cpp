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

bool InGameGuiSubsystem::openOverlay(gui::GameWndOverlay overlay) {
    // RefCode's ENABLE_SCORING / DISABLE_SCORING gate ScoreKeeper mutations
    // (kills, construction, income, and so on), not the ScoreScreen itself.
    // The score WND must remain reachable even when a cinematic map has
    // disabled accumulation; it may still present the match result and the
    // roster after the future score-event consumers are in place.
    if (m_layer.openOverlay(overlay)) {
        const bool pausesLocalSession =
            overlay == gui::GameWndOverlay::QuitMenu ||
            overlay == gui::GameWndOverlay::QuitNoSave ||
            overlay == gui::GameWndOverlay::Options;
        if (pausesLocalSession) {
            requestInGameMenuPause();
        } else {
            releaseInGameMenuPause();
        }
        if (overlay == gui::GameWndOverlay::ScoreScreen) {
            startScoreScreenMusic();
        } else {
            stopScoreScreenMusic();
        }
        m_activeOverlay = overlay;
        configureOverlayButtons();
        return true;
    }
    // A VFS revision change may invalidate the active cache before the new
    // WND fails to load. In that case no modal remains to own pause/music.
    if (!m_layer.hasOverlay()) {
        stopScoreScreenMusic();
        releaseInGameMenuPause();
    }
    return false;
}

void InGameGuiSubsystem::closeOverlay() {
    m_layer.closeOverlay();
    stopScoreScreenMusic();
    releaseInGameMenuPause();
}

void InGameGuiSubsystem::openQuitMenu() {
    openOverlay(gui::GameWndOverlay::QuitMenu);
}

void InGameGuiSubsystem::queueResultAction(
    engine::GameResultAction action) {
    stopScoreScreenMusic();
    if (!m_logicIntents.post(
            app::runtime::QueueResultActionIntent{.action = action},
            m_gameProjection.sessionRevision)) {
        // The result screen remains active when the typed action could not be
        // admitted, so preserve its presentation rather than silently muting.
        startScoreScreenMusic();
    }
}

void InGameGuiSubsystem::startScoreScreenMusic() {
    if (m_scoreScreenMusicRequested || !m_scoreScreenViewModel ||
        m_scoreScreenViewModel->music.empty()) {
        return;
    }
    m_scoreScreenMusicRequested = m_audio.requestScoreScreenMusic(
        m_scoreScreenViewModel->music);
}

void InGameGuiSubsystem::stopScoreScreenMusic() {
    if (!m_scoreScreenMusicRequested) return;
    if (m_audio.stopScoreScreenMusic()) {
        m_scoreScreenMusicRequested = false;
    }
}

void InGameGuiSubsystem::configureOverlayButtons() {
    const auto bindAll = [this](std::initializer_list<container::StringView> names,
                                const std::function<void(gui::Widget&)>& callback) {
        for (const container::StringView name : names) {
            if (gui::Widget* button = m_layer.findOverlay(name)) {
                button->onClick = callback;
            }
        }
    };

    if (m_activeOverlay == gui::GameWndOverlay::Victorious ||
        m_activeOverlay == gui::GameWndOverlay::Defeat) {
        bindAll({"ButtonContinue", "ButtonOK", "ButtonOk", "ButtonScoreScreen", "ButtonNext"},
                [this](gui::Widget&) {
                    static_cast<void>(openOverlay(gui::GameWndOverlay::ScoreScreen));
                });
        bindAll({"ButtonExit", "ButtonQuit"}, [this](gui::Widget&) {
            queueResultAction(engine::GameResultAction::Exit);
        });
        return;
    }

    if (m_activeOverlay == gui::GameWndOverlay::ScoreScreen) {
        populateScoreScreen();
        const bool hasNext = m_gameProjection.canResultNext;
        const bool canRetry = m_gameProjection.canResultRetry;
        for (const container::StringView name :
             {container::StringView{"ButtonNext"}, container::StringView{"ButtonContinue"},
              container::StringView{"ButtonNextMission"}}) {
            if (gui::Widget* button = m_layer.findOverlay(name)) {
                button->setVisible(hasNext);
                button->setText(localizedText(
                    {"GUI:Continue", "GUI:NextMission"}, "Next"));
                button->onClick = [this](gui::Widget&) {
                    queueResultAction(engine::GameResultAction::Next);
                };
            }
        }
        bindAll({"ButtonRetry", "ButtonRestart", "ButtonReplay", "ButtonEmote"}, [this](gui::Widget&) {
            queueResultAction(engine::GameResultAction::Retry);
        });
        for (const container::StringView name :
             {container::StringView{"ButtonRetry"}, container::StringView{"ButtonRestart"},
              container::StringView{"ButtonReplay"}, container::StringView{"ButtonEmote"}}) {
            if (gui::Widget* retry = m_layer.findOverlay(name)) {
                retry->setVisible(canRetry);
            }
        }
        if (gui::Widget* retry = m_layer.findOverlay("ButtonEmote")) {
            retry->setText(localizedText({"GUI:Retry"}, "Retry"));
        }
        bindAll({"ButtonExit", "ButtonQuit", "ButtonReturn", "ButtonOk", "ButtonOK"}, [this](gui::Widget&) {
            queueResultAction(engine::GameResultAction::Exit);
        });
        if (gui::Widget* exit = m_layer.findOverlay("ButtonOk")) {
            exit->setText(localizedText({"GUI:Exit"}, "Exit"));
        }
        // These stock ScoreScreen controls are network/social actions.  The
        // offline in-game flow does not expose inert buttons as fake actions.
        if (gui::Widget* buddy = m_layer.findOverlay("ButtonBuddy")) buddy->hide();
        if (gui::Widget* saveReplay = m_layer.findOverlay("ButtonSaveReplay")) saveReplay->hide();
        return;
    }

    auto closeCurrentOverlay = [this](gui::Widget&) { closeOverlay(); };
    auto backToPauseMenu = [this, closeCurrentOverlay](gui::Widget& widget) {
        if (m_activeOverlay == gui::GameWndOverlay::QuitMenu) {
            closeCurrentOverlay(widget);
        } else {
            openQuitMenu();
        }
    };

    if (auto* button = m_layer.findOverlay("ButtonReturn")) {
        button->onClick = closeCurrentOverlay;
    }
    if (auto* button = m_layer.findOverlay("ButtonBack")) {
        button->onClick = backToPauseMenu;
    }
    if (auto* button = m_layer.findOverlay("ButtonCancel")) {
        button->onClick = backToPauseMenu;
    }
    if (auto* button = m_layer.findOverlay("ButtonClose")) {
        button->onClick = closeCurrentOverlay;
    }
    if (auto* button = m_layer.findOverlay("ButtonSaveLoad")) {
        // U-006: presentation-only transactions are not complete game
        // checkpoints.  Do not advertise an action that cannot restore the
        // simulation, scripts, ECS, and presentation as one atomic state.
        button->onClick = {};
        button->hide();
    }
    if (auto* button = m_layer.findOverlay("ButtonOptions")) {
        button->setEnabled(m_layer.isGameplayInputEnabled());
        button->onClick = [this](gui::Widget&) {
            if (!m_layer.isGameplayInputEnabled()) return;
            openOverlay(gui::GameWndOverlay::Options);
        };
    }
}

void InGameGuiSubsystem::synchronizeMatchResult() {
    if (m_gameProjection.gameState != engine::GameState::Result) {
        if (m_presentedMatchResultRevision != 0) {
            closeOverlay();
            m_presentedMatchResultRevision = 0;
            m_scoreScreenViewModel.reset();
            m_resultIntroStartedAt = {};
        }
        return;
    }

    m_layer.setGameplayHudSuppressed(true);
    m_layer.setGameplayInputEnabled(false);
    const auto& snapshot = m_gameProjection.matchResult;
    if (!snapshot) return;
    if (m_presentedMatchResultRevision ==
        m_gameProjection.matchResultRevision) {
        if (m_presentedResultTransitionError !=
            m_gameProjection.resultTransitionError) {
            m_presentedResultTransitionError =
                m_gameProjection.resultTransitionError;
            populateScoreScreen();
            if (m_activeOverlay == gui::GameWndOverlay::ScoreScreen) {
                // A rejected staged continuation leaves the result UI active.
                startScoreScreenMusic();
            }
        }
        if ((m_activeOverlay == gui::GameWndOverlay::Victorious ||
             m_activeOverlay == gui::GameWndOverlay::Defeat) &&
            m_resultIntroStartedAt != std::chrono::steady_clock::time_point{}) {
            const auto introDuration = snapshot->outcome.mode == engine::scenario::MissionEndMode::Quick
                ? std::chrono::milliseconds{500}
                : std::chrono::milliseconds{2000};
            if (std::chrono::steady_clock::now() - m_resultIntroStartedAt >= introDuration) {
                static_cast<void>(openOverlay(gui::GameWndOverlay::ScoreScreen));
            }
        }
        return;
    }

    m_scoreScreenViewModel =
        gui::ingame::ScoreScreenViewModel::fromSnapshot(*snapshot);
    m_presentedMatchResultRevision = m_gameProjection.matchResultRevision;
    m_presentedResultTransitionError =
        m_gameProjection.resultTransitionError;
    m_resultIntroStartedAt = std::chrono::steady_clock::now();
    const gui::GameWndOverlay intro = snapshot->localVictory()
        ? gui::GameWndOverlay::Victorious
        : gui::GameWndOverlay::Defeat;
    if (!openOverlay(intro)) {
        // Mods may omit the short intro WND while retaining ScoreScreen.
        static_cast<void>(openOverlay(gui::GameWndOverlay::ScoreScreen));
    }
}

void InGameGuiSubsystem::populateScoreScreen() {
    if (!m_scoreScreenViewModel) return;
    const gui::ingame::ScoreScreenViewModel& model = *m_scoreScreenViewModel;
    const auto findFirst = [this](std::initializer_list<container::StringView> names)
        -> gui::Widget* {
        for (const container::StringView name : names) {
            if (gui::Widget* widget = m_layer.findOverlay(name)) return widget;
        }
        return nullptr;
    };

    if (gui::Widget* title = findFirst(
            {"ChallengeWinLossText", "StaticTextResult", "StaticTextTitle", "TextResult", "GameResult"})) {
        title->setText(model.resultTitle);
    }
    gui::Widget* summary = findFirst(
        {"GeneralRemarks", "StaticTextMission", "StaticTextMap",
         "StaticTextSummary", "TextMission"});
    if (summary) {
        summary->setText(model.missionSummary);
    }
    gui::Widget* transitionError = findFirst(
        {"StaticTextTransitionError", "StaticTextError", "TransitionError",
         "TextError"});
    if (transitionError) {
        transitionError->setVisible(
            !m_gameProjection.resultTransitionError.empty());
        transitionError->setText(
            m_gameProjection.resultTransitionError.empty()
                ? container::String{}
                : localizedText({"GUI:Error"}, "Could not continue:") +
                    " " + m_gameProjection.resultTransitionError);
    } else if (summary &&
               !m_gameProjection.resultTransitionError.empty()) {
        summary->setText(
            model.missionSummary + "\n" +
            localizedText({"GUI:Error"}, "Could not continue:") + " " +
            m_gameProjection.resultTransitionError);
    }
    if (gui::Widget* roster = findFirst(
            {"ListboxPlayers", "ListboxScore", "ListboxScores", "ListboxResults",
             "PlayerList", "ScoreList"})) {
        roster->setItems(model.playerRows);
        roster->setSelectedIndex(-1);
        roster->setScrollOffset(0);
    }
    if (!model.backgroundImage.empty()) {
        if (gui::Widget* background = findFirst(
                {"MainBackdrop", "ScoreScreenImage", "WinScoreScreenImage", "Background", "WindowBackground"})) {
            background->setDrawImage(0, model.backgroundImage);
        }
    }

    const auto setIndexed = [this](container::StringView prefix, size_t index,
                                   const container::String& value, bool visible) {
        container::String name{prefix};
        name += std::to_string(index);
        if (gui::Widget* widget = m_layer.findOverlay(name)) {
            widget->setVisible(visible);
            if (visible) widget->setText(value);
        }
    };
    constexpr size_t kStockScoreRows = 8;
    for (size_t index = 0; index < kStockScoreRows; ++index) {
        const bool visible = index < model.players.size();
        const gui::ingame::ScoreScreenPlayerViewModel empty;
        const gui::ingame::ScoreScreenPlayerViewModel& player =
            visible ? model.players[index] : empty;
        setIndexed("StaticTextPlayer", index, player.name, visible);
        setIndexed("StaticTextUnitsBuilt", index, player.unitsBuilt, visible);
        setIndexed("StaticTextUnitsLost", index, player.unitsLost, visible);
        setIndexed("StaticTextUnitsDestroyed", index, player.unitsDestroyed, visible);
        setIndexed("StaticTextBuildingsBuilt", index, player.buildingsBuilt, visible);
        setIndexed("StaticTextBuildingsLost", index, player.buildingsLost, visible);
        setIndexed("StaticTextBuildingsDestroyed", index, player.buildingsDestroyed, visible);
        setIndexed("StaticTextResources", index, player.resourcesCollected, visible);
        setIndexed("StaticTextObserver", index, {}, false);
    }
}
