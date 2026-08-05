#include "InGameGuiSubsystem.h"
#include "InGameGuiSubsystemDetail.h"
#include "app/runtime/GameLogicIntent.h"
#include "app/runtime/GameUiProjection.h"

#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "Widget.h"
#include "debug/debug.h"

#include <algorithm>
#include <chrono>

using namespace ingame_gui_detail;

void InGameGuiSubsystem::showLoadingScreen() {
    m_loadingAttempted = true;
    const engine::GameStartInfo defaultInfo;
    const auto& info = m_gameProjection.startInfo
        ? *m_gameProjection.startInfo : defaultInfo;
    m_loadingVisualRevision = m_gameProjection.loadingRevision;
    m_loadingNotificationRevision = m_loadingVisualRevision;
    m_loadingPresentedNotificationPosted = false;
    m_loadingDismissedNotificationPosted = false;
    m_loadingDisplayedProgress = 0.0f;
    m_loadingFadeT = 0.0f;
    m_loadingAnimationStartedAt = std::chrono::steady_clock::now();
    m_loadingLastUpdateAt = m_loadingAnimationStartedAt;
    m_loadingVisualPhase = LoadingVisualPhase::FadingIn;
    if (!loadRuntime(m_loadingScreen, loadingScreenPaths(info))) {
        clearLoadingWidgetHandles();
        TD_LOG_WARN("[InGameGUI] Loading screen WND not available for mode={}", static_cast<int>(info.mode));
        // A missing/modded WND must not deadlock GameLogic's presentation
        // handshake. Continue headlessly and dismiss as soon as loading is ready.
        m_loadingVisualPhase = LoadingVisualPhase::Active;
        static_cast<void>(postLoadingScreenPresentedNotification());
        return;
    }

    m_loadingVisible = true;
    cacheLoadingWidgetHandles();
    configureLoadingScreen();
    TD_LOG_INFO("[InGameGUI] Loading screen shown");
}

void InGameGuiSubsystem::hideLoadingScreen() {
    if (!m_loadingVisible && !m_loadingScreen.isLoaded()) {
        m_loadingAttempted = false;
        m_loadingFailureVisible = false;
        m_loadingVisualPhase = LoadingVisualPhase::Hidden;
        m_loadingVisualRevision = 0;
        m_loadingDisplayedProgress = 0.0f;
        m_loadingFadeT = 0.0f;
        m_loadingAnimationStartedAt = {};
        m_loadingLastUpdateAt = {};
        m_loadingObjectiveLines.clear();
        clearLoadingWidgetHandles();
        return;
    }
    if (m_loadingVisualPhase == LoadingVisualPhase::Dismissed) {
        m_postLoadingFadeActive = true;
        m_postLoadingFadeStartedAt = std::chrono::steady_clock::now();
    }
    clearLoadingWidgetHandles();
    m_loadingScreen.shutdown();
    m_loadingVisible = false;
    m_loadingAttempted = false;
    m_loadingFailureVisible = false;
    m_loadingVisualPhase = LoadingVisualPhase::Hidden;
    m_loadingVisualRevision = 0;
    m_loadingDisplayedProgress = 0.0f;
    m_loadingFadeT = 0.0f;
    m_loadingAnimationStartedAt = {};
    m_loadingLastUpdateAt = {};
    m_loadingObjectiveLines.clear();
    TD_LOG_INFO("[InGameGUI] Loading screen hidden");
}

void InGameGuiSubsystem::cacheLoadingWidgetHandles() {
    clearLoadingWidgetHandles();
    if (!m_loadingScreen.isLoaded()) return;
    m_loadingWidgets.runtimeGeneration = m_loadingScreen.generation();
    const auto find = [this](container::StringView name) {
        return m_loadingScreen.findByName(name);
    };
    m_loadingWidgets.progress = find("ProgressLoad");
    m_loadingWidgets.percent = find("Percent");
    for (container::StringView name :
         {container::StringView{"StaticTextLoading"},
          container::StringView{"StaticTextStatus"},
          container::StringView{"LoadingStatus"}}) {
        if ((m_loadingWidgets.status = find(name))) break;
    }
    m_loadingWidgets.location = find("StaticTextCameoText3");
    m_loadingWidgets.objectives = find("ObjectivesWin");
    for (size_t index = 0; index < m_loadingWidgets.objectiveLines.size(); ++index) {
        m_loadingWidgets.objectiveLines[index] =
            find("StaticTextLine" + std::to_string(index));
    }
    for (size_t index = 0; index < m_loadingWidgets.cameoTexts.size(); ++index) {
        m_loadingWidgets.cameoTexts[index] =
            find("StaticTextCameoText" + std::to_string(index));
    }
    const auto collect = [&find](container::Vector<gui::Widget*>& output,
                                 std::initializer_list<container::StringView> names) {
        output.reserve(names.size());
        for (container::StringView name : names) {
            if (gui::Widget* widget = find(name)) output.push_back(widget);
        }
    };
    collect(m_loadingWidgets.challengeEarly,
            {"BioNameLeft", "BioBirthplaceLeft", "BioStrategyLeft",
             "BioNameRight", "BioBirthplaceRight", "BioStrategyRight"});
    collect(m_loadingWidgets.challengeLeft,
            {"BigNameEntryLeft", "BioNameEntryLeft", "BioBirthplaceEntryLeft",
             "BioStrategyEntryLeft", "PortraitLeft"});
    collect(m_loadingWidgets.challengeRight,
            {"BigNameEntryRight", "BioNameEntryRight", "BioBirthplaceEntryRight",
             "BioStrategyEntryRight", "PortraitRight"});
    collect(m_loadingWidgets.challengeCircles,
            {"CircleAlphaOuter", "CircleAlphaInner"});
    collect(m_loadingWidgets.challengeVersus,
            {"VersusBackdrop", "OverlayVs"});
}

void InGameGuiSubsystem::clearLoadingWidgetHandles() noexcept {
    m_loadingWidgets = {};
}

bool InGameGuiSubsystem::postLoadingScreenPresentedNotification() {
    if (m_loadingNotificationRevision != m_loadingVisualRevision) return false;
    if (m_loadingPresentedNotificationPosted) return true;
    // This only marks the next recorded UI draw list. The render owner posts
    // NotifyLoadingScreenPresentedIntent after endFrame/Present succeeds;
    // elapsed fade time alone is not proof that the WND reached the screen.
    m_loadingPresentedNotificationPosted = true;
    return true;
}

bool InGameGuiSubsystem::postLoadingScreenDismissedNotification() {
    if (m_loadingNotificationRevision != m_loadingVisualRevision ||
        !m_loadingPresentedNotificationPosted) {
        return false;
    }
    if (m_loadingDismissedNotificationPosted) return true;
    if (!m_logicIntents.post(
            app::runtime::NotifyLoadingScreenDismissedIntent{
                .loadingRevision = m_loadingNotificationRevision},
            m_gameProjection.sessionRevision)) {
        return false;
    }
    m_loadingDismissedNotificationPosted = true;
    return true;
}

void InGameGuiSubsystem::configureLoadingScreen() {
    gui::Widget* root = m_loadingScreen.root();
    if (!root) return;
    const engine::GameStartInfo defaultInfo;
    const engine::GameStartInfo& info = m_gameProjection.startInfo
        ? *m_gameProjection.startInfo : defaultInfo;
    const game::Mission* mission = loadingMission(info);
    const auto find = [this](container::StringView name) {
        return m_loadingScreen.findByName(name);
    };
    const auto setText = [&find](container::StringView name,
                                 container::String text) {
        if (gui::Widget* widget = find(name)) widget->setText(std::move(text));
    };
    const auto setVisible = [&find](container::StringView name, bool visible) {
        if (gui::Widget* widget = find(name)) widget->setVisible(visible);
    };
    const auto setImage = [&find](container::StringView name,
                                  const container::String& image) {
        if (!image.empty()) {
            if (gui::Widget* widget = find(name)) widget->setDrawImage(0, image);
        }
    };

    gui::Widget* progress = find("ProgressLoad");
    if (progress) progress->setSliderValue(0.0f);
    setText("Percent", "0%");
    setVisible("Percent", false);

    m_loadingObjectiveLines.clear();
    if (info.mode == engine::GameMode::SinglePlayer) {
        container::String background = "MissionLoad_USA";
        container::String progressCenter = "LoadingBar_ProgressCenter2";
        if (asciiEqualIgnoreCase(info.sequence.campaignName, "GLA")) {
            background = "MissionLoad_GLA";
            progressCenter = "LoadingBar_ProgressCenter3";
        } else if (asciiEqualIgnoreCase(info.sequence.campaignName, "China")) {
            background = "MissionLoad_China";
            progressCenter = "LoadingBar_ProgressCenter1";
        }
        setImage("ParentSinglePlayerLoadScreen", background);
        if (progress) progress->setDrawImage(6, progressCenter);
        TD_LOG_INFO(
            "[InGameGUI] Campaign loading visuals: campaign='{}' background='{}' progress='{}'",
            info.sequence.campaignName.empty() ? "USA" : info.sequence.campaignName,
            background, progressCenter);

        // SinglePlayerLoadScreen.wnd authors ObjectivesWin as visible by
        // default. Clear every mission-optional field before population so a
        // missing mission or empty localized label cannot expose an empty
        // panel left behind by the WND defaults.
        setText("StaticTextCameoText3", {});
        setVisible("StaticTextCameoText3", false);
        setVisible("ObjectivesWin", false);
        for (size_t index = 0; index < 5; ++index) {
            const container::String name =
                "StaticTextLine" + std::to_string(index);
            setText(name, {});
            setVisible(name, false);
        }
        for (size_t index = 0; index < 3; ++index) {
            const container::String name =
                "StaticTextCameoText" + std::to_string(index);
            setText(name, {});
            setVisible(name, false);
        }
        if (!mission) return;

        setText("StaticTextCameoText3", loadingText(mission->locationName));
        for (size_t index = 0; index < 5; ++index) {
            m_loadingObjectiveLines.push_back(loadingText(mission->objectives[index]));
        }
        for (size_t index = 0; index < 3; ++index) {
            container::String name = "StaticTextCameoText" + std::to_string(index);
            setText(name, loadingText(mission->unitNames[index]));
        }
    }

    if (info.mode != engine::GameMode::Challenge) return;
    const game::ChallengeGenerals& generals = game::ChallengeGenerals::instance();
    const int playerIndex = generals.getGeneralByTemplateName(info.sequence.challengeGeneral);
    const int opponentIndex = mission
        ? generals.getGeneralByGeneralName(mission->generalName)
        : -1;
    const game::GeneralPersona* player = playerIndex >= 0
        ? &generals.getGeneral(playerIndex) : nullptr;
    const game::GeneralPersona* opponent = opponentIndex >= 0
        ? &generals.getGeneral(opponentIndex) : nullptr;

    const auto populatePersona = [&](const game::GeneralPersona* persona,
                                     container::StringView suffix) {
        if (!persona) return;
        const container::String side{suffix};
        setText("BigNameEntry" + side, loadingText(persona->bioName));
        setText("BioNameEntry" + side, loadingText(persona->bioName));
        setText("BioBirthplaceEntry" + side, loadingText(persona->bioRank));
        setText("BioStrategyEntry" + side, loadingText(persona->bioStrategy));
        setImage("Portrait" + side, persona->imageBioPortraitLarge);
    };
    populatePersona(player, "Left");
    populatePersona(opponent, "Right");

    for (container::StringView name : {
             "BioNameLeft", "BioBirthplaceLeft", "BioStrategyLeft",
             "BioNameRight", "BioBirthplaceRight", "BioStrategyRight",
             "BigNameEntryLeft", "BioNameEntryLeft", "BioBirthplaceEntryLeft",
             "BioStrategyEntryLeft", "BigNameEntryRight", "BioNameEntryRight",
             "BioBirthplaceEntryRight", "BioStrategyEntryRight", "PortraitLeft",
             "PortraitRight", "CircleAlphaOuter", "CircleAlphaInner",
             "VersusBackdrop", "OverlayVs"}) {
        setVisible(name, false);
    }
}

void InGameGuiSubsystem::updateLoadingScreenPresentation() {
    if (m_loadingVisualRevision != m_gameProjection.loadingRevision) return;

    if (m_loadingFailureVisible) {
        m_loadingVisualPhase = LoadingVisualPhase::Active;
        m_loadingFadeT = 1.0f;
        static_cast<void>(postLoadingScreenPresentedNotification());
        if (!m_loadingVisible) return;
        m_loadingScreen.update();
        if (m_loadingWidgets.progress) m_loadingWidgets.progress->setVisible(false);
        if (m_loadingWidgets.percent) m_loadingWidgets.percent->setVisible(false);
        if (m_loadingWidgets.status) {
            m_loadingWidgets.status->setText("Loading failed");
            m_loadingWidgets.status->setVisible(true);
        }
        return;
    }

    if (!m_loadingVisible) {
        static_cast<void>(postLoadingScreenPresentedNotification());
        if (m_gameProjection.loadingStage == engine::GameLoadingStage::Ready) {
            if (postLoadingScreenDismissedNotification()) {
                m_loadingVisualPhase = LoadingVisualPhase::Dismissed;
            }
        }
        return;
    }

    m_loadingScreen.update();
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = std::clamp(
        std::chrono::duration<float>(now - m_loadingLastUpdateAt).count(),
        0.0f, 0.1f);
    m_loadingLastUpdateAt = now;
    const float targetProgress = std::clamp(
        m_gameProjection.loadingProgress, 0.0f, 1.0f);
    m_loadingDisplayedProgress = std::min(
        targetProgress, m_loadingDisplayedProgress + deltaSeconds * 1.75f);

    if (m_loadingWidgets.progress) {
        m_loadingWidgets.progress->setSliderValue(m_loadingDisplayedProgress);
    }
    if (m_loadingWidgets.percent) {
        m_loadingWidgets.percent->setText(std::to_string(static_cast<int>(
            std::clamp(m_loadingDisplayedProgress * 100.0f, 0.0f, 100.0f))) + "%");
    }
    if (m_loadingWidgets.status) {
        m_loadingWidgets.status->setText(m_gameProjection.loadingStatus);
    }

    const float elapsedSeconds = std::chrono::duration<float>(
        now - m_loadingAnimationStartedAt).count();
    animateLoadingScreenWidgets(elapsedSeconds);

    constexpr float kFadeSeconds = 0.20f;
    if (m_loadingVisualPhase == LoadingVisualPhase::FadingIn) {
        m_loadingFadeT = std::min(1.0f, m_loadingFadeT + deltaSeconds / kFadeSeconds);
        if (m_loadingFadeT >= 1.0f) {
            m_loadingVisualPhase = LoadingVisualPhase::Active;
            static_cast<void>(postLoadingScreenPresentedNotification());
        }
        return;
    }

    if (m_loadingVisualPhase == LoadingVisualPhase::Dismissed) {
        static_cast<void>(postLoadingScreenDismissedNotification());
        return;
    }

    if (m_loadingVisualPhase == LoadingVisualPhase::Active &&
        !postLoadingScreenPresentedNotification()) {
        return;
    }

    if (m_loadingVisualPhase == LoadingVisualPhase::Active &&
        m_gameProjection.loadingStage == engine::GameLoadingStage::Ready &&
        m_loadingDisplayedProgress >= 0.999f) {
        float minimumVisibleSeconds = 0.45f;
        const engine::GameMode mode = m_gameProjection.startInfo
            ? m_gameProjection.startInfo->mode : engine::GameMode::Invalid;
        if (mode == engine::GameMode::SinglePlayer) {
            minimumVisibleSeconds = 1.0f;
        } else if (mode == engine::GameMode::Challenge) {
            minimumVisibleSeconds = 0.90f;
        }
        if (elapsedSeconds >= minimumVisibleSeconds) {
            m_loadingVisualPhase = LoadingVisualPhase::FadingOut;
            m_loadingFadeT = 0.0f;
        }
        return;
    }

    if (m_loadingVisualPhase == LoadingVisualPhase::FadingOut) {
        m_loadingFadeT = std::min(1.0f, m_loadingFadeT + deltaSeconds / kFadeSeconds);
        if (m_loadingFadeT >= 1.0f) {
            m_loadingVisualPhase = LoadingVisualPhase::Dismissed;
            static_cast<void>(postLoadingScreenDismissedNotification());
        }
    }
}

void InGameGuiSubsystem::animateLoadingScreenWidgets(float elapsedSeconds) {
    if (m_loadingWidgets.runtimeGeneration != m_loadingScreen.generation()) return;
    const engine::GameMode mode = m_gameProjection.startInfo
        ? m_gameProjection.startInfo->mode : engine::GameMode::Invalid;
    if (mode == engine::GameMode::SinglePlayer) {
        if (m_loadingWidgets.location) {
            m_loadingWidgets.location->setVisible(
                elapsedSeconds >= 0.15f &&
                !m_loadingWidgets.location->text().empty());
        }
        const bool hasObjectives = std::any_of(
            m_loadingObjectiveLines.begin(), m_loadingObjectiveLines.end(),
            [](const container::String& line) { return !line.empty(); });
        if (m_loadingWidgets.objectives) {
            m_loadingWidgets.objectives->setVisible(
                elapsedSeconds >= 0.25f && hasObjectives);
        }
        size_t characterBudget = elapsedSeconds > 0.30f
            ? static_cast<size_t>((elapsedSeconds - 0.30f) * 70.0f)
            : 0;
        for (size_t index = 0; index < m_loadingObjectiveLines.size(); ++index) {
            const container::String& full = m_loadingObjectiveLines[index];
            const size_t visibleCharacters = std::min(characterBudget, full.size());
            if (gui::Widget* line = m_loadingWidgets.objectiveLines[index]) {
                line->setVisible(elapsedSeconds >= 0.30f && !full.empty());
                line->setText(full.substr(0, visibleCharacters));
            }
            characterBudget -= visibleCharacters;
        }
        container::Array<int, 3> populatedCameos{};
        size_t populatedCameoCount = 0;
        for (int index = 0; index < 3; ++index) {
            const gui::Widget* cameo = m_loadingWidgets.cameoTexts[index];
            if (cameo && !cameo->text().empty()) {
                populatedCameos[populatedCameoCount++] = index;
            }
        }
        int activeCameo = -1;
        if (elapsedSeconds >= 0.50f && populatedCameoCount != 0) {
            const size_t phase = static_cast<size_t>(
                (elapsedSeconds - 0.50f) / 0.35f);
            activeCameo = populatedCameos[phase % populatedCameoCount];
        }
        for (int index = 0; index < 3; ++index) {
            if (gui::Widget* cameo = m_loadingWidgets.cameoTexts[index]) {
                cameo->setVisible(index == activeCameo && !cameo->text().empty());
            }
        }
        return;
    }

    if (mode != engine::GameMode::Challenge) return;
    const auto reveal = [](const container::Vector<gui::Widget*>& widgets,
                           bool visible) {
        for (gui::Widget* widget : widgets) widget->setVisible(visible);
    };
    reveal(m_loadingWidgets.challengeEarly, elapsedSeconds >= 0.10f);
    reveal(m_loadingWidgets.challengeLeft, elapsedSeconds >= 0.22f);
    reveal(m_loadingWidgets.challengeRight, elapsedSeconds >= 0.38f);
    reveal(m_loadingWidgets.challengeCircles, elapsedSeconds >= 0.55f);
    reveal(m_loadingWidgets.challengeVersus, elapsedSeconds >= 0.70f);
}

float InGameGuiSubsystem::loadingFadeOverlayAlpha() const noexcept {
    if (m_loadingVisualPhase == LoadingVisualPhase::FadingIn) {
        return 1.0f - m_loadingFadeT;
    }
    if (m_loadingVisualPhase == LoadingVisualPhase::FadingOut) {
        return m_loadingFadeT;
    }
    if (m_loadingVisualPhase == LoadingVisualPhase::Dismissed) return 1.0f;
    return 0.0f;
}
