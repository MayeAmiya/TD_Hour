#include "ApplicationHost.h"

#include "FramePacer.h"
#include "GameContentSubsystem.h"
#include "InputCoordinator.h"
#include "PresentationCoordinator.h"
#include "ui/ingame/InGameGuiSubsystem.h"
#include "runtime/GameLogicIntent.h"
#include "runtime/GameUiProjection.h"
#include "runtime/LaunchOutcome.h"

#include "CommandLine.h"
#include "StringTable.h"
#include "TextureManager.h"
#include "core/container/string_utils.h"
#include "core/constants/Paths.h"
#include "core/constants/Strings.h"
#include "core/platform/dedicated_thread.h"
#include "core/platform/runtime_mailbox.h"
#include "debug/debug.h"
#include "game/base/GameLaunchDescriptor.h"
#include "app/runtime/GameLogic.h"
#include "game/session/core/GameSession.h"
#include "game/base/GameStartInfoBuilder.h"
#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "game/ini/GameDataLoader.h"
#include "engine/texture/MappedImageCollection.h"
#include "engine/resource/ResourceSchedulerRuntime.h"
#include "system/AudioSubsystem.h"
#include "system/FileSystemSubsystem.h"
#include "system/FontLibrarySubsystem.h"
#include "system/GameTextSubsystem.h"
#include "system/GuiSubsystem.h"
#include "system/RendererSubsystem.h"
#include "system/SubsystemList.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <utility>

std::atomic<bool> g_quitRequested{false};

namespace app {

namespace {

container::String canonicalMapPath(container::StringView path) {
    container::String result = container::trimAsciiCopy(path);
    std::replace(result.begin(), result.end(), '/', '\\');
    std::transform(result.begin(), result.end(), result.begin(),
        [](char character) { return container::asciiLower(character); });
    return result;
}

const game::Mission* findMission(
    const game::Campaign& campaign, container::StringView missionName) {
    const auto found = std::find_if(
        campaign.missions.begin(), campaign.missions.end(),
        [missionName](const game::Mission& mission) {
            return container::asciiEqualIgnoreCase(
                mission.name, missionName);
        });
    return found == campaign.missions.end() ? nullptr : &*found;
}

bool validateAuthoredSequenceIdentity(
    const engine::GameStartInfo& info, container::String& error) {
    if (info.sequence.type == engine::GameSequenceType::None) return true;

    game::Campaign* campaign = game::CampaignManager::instance().findCampaign(
        info.sequence.campaignName);
    if (!campaign) {
        error = "campaign catalog does not contain sequence campaign '" +
            info.sequence.campaignName + "'";
        return false;
    }
    const game::Mission* mission = findMission(
        *campaign, info.sequence.missionName);
    if (!mission || canonicalMapPath(mission->mapName) !=
                        canonicalMapPath(info.mapName)) {
        error = "sequence mission does not own the requested map";
        return false;
    }

    if (info.sequence.type == engine::GameSequenceType::Campaign) {
        if (campaign->isChallengeCampaign ||
            campaign->playerFactionName.empty() ||
            !container::asciiEqualIgnoreCase(
                campaign->playerFactionName,
                info.localPlayerTemplateName)) {
            error =
                "campaign sequence and local player template do not match Campaign.ini";
            return false;
        }
        return true;
    }

    if (!campaign->isChallengeCampaign ||
        !container::asciiEqualIgnoreCase(
            info.sequence.challengeGeneral,
            info.localPlayerTemplateName)) {
        error =
            "challenge sequence and local player template do not match";
        return false;
    }
    const int generalIndex = game::ChallengeGenerals::instance()
        .getGeneralByTemplateName(info.sequence.challengeGeneral);
    if (generalIndex < 0) {
        error = "challenge general is absent from ChallengeMode.ini";
        return false;
    }
    const game::GeneralPersona& general =
        game::ChallengeGenerals::instance().getGeneral(generalIndex);
    if (!container::asciiEqualIgnoreCase(
            general.campaign, campaign->name)) {
        error =
            "challenge general does not own the requested challenge campaign";
        return false;
    }
    return true;
}

} // namespace

class ApplicationHost::Impl final {
public:
    Impl() = default;
    ~Impl() { shutdown(); }

    void initialize();
    int run();

private:
    void applyHostRenderQualityOverrides();
    bool prepareLauncherSessionBootstrap();
    void startRequestedSession();
    void publishLaunchOutcomeForProjection(
        const runtime::GameUiProjection& projection);
    void failExternalLaunch(
        runtime::LaunchOutcomeStage stage,
        runtime::LaunchOutcomeCode code,
        container::StringView reason, bool retryable, int exitCode);
    void shutdown() noexcept;

    SubsystemList m_subsystems;
    std::unique_ptr<engine::resource::ResourceSchedulerRuntime>
        m_resourceScheduler;
    std::unique_ptr<FileSystemSubsystem> m_fileSystem;
    std::unique_ptr<GameContentSubsystem> m_gameContent;
    std::unique_ptr<engine::AudioSubsystem> m_audio;
    std::unique_ptr<GameTextSubsystem> m_gameText;
    std::unique_ptr<RendererSubsystem> m_renderer;
    std::unique_ptr<FontLibrarySubsystem> m_fontLibrary;
    std::unique_ptr<GuiSubsystem> m_gui;
    std::unique_ptr<InGameGuiSubsystem> m_inGameGui;
    std::unique_ptr<engine::TextureManager> m_textureManager;
    std::unique_ptr<PresentationCoordinator> m_presentation;
    std::unique_ptr<InputCoordinator> m_input;
    std::unique_ptr<FramePacer> m_framePacer;
    runtime::GameLogicIntentMailbox m_logicIntents;
    runtime::GameUiProjectionPublisher m_gameUiProjectionPublisher;
    runtime::GameUiProjectionMailbox m_gameUiProjectionMailbox;
    runtime::GameUiProjection m_mainGameProjection;
    platform::runtime::DedicatedThread m_logicThread;
    std::atomic<bool> m_logicExitRequested{false};
    platform::runtime::LatestValueMailbox<engine::UiDrawList> m_renderFrames;
    platform::runtime::DedicatedThread m_renderThread;
    runtime::LaunchOutcomePublisher m_launchOutcome;
    std::optional<engine::LauncherSessionDescriptor> m_launcherSession;
    container::String m_launcherTicket;
    int m_exitCode = runtime::LaunchExitCode::Success;
    bool m_startupExitRequested = false;
    bool m_normalGameExit = false;
    bool m_shutdownComplete = false;
    bool m_hasRun = false;
    uint64_t m_reportedInputOverflow = 0;
};

bool ApplicationHost::Impl::prepareLauncherSessionBootstrap() {
    const auto& commandLine = engine::CommandLine::instance();
    if (!commandLine.hasParam("session-descriptor")) return true;

    const container::String descriptorPath =
        commandLine.getParam("session-descriptor");
    m_launcherTicket = commandLine.getParam("session-ticket");
    if (m_launcherTicket.empty()) {
        m_launcherTicket =
            std::filesystem::path(descriptorPath).stem().string();
    }
    if (!engine::GameLaunchDescriptor::isValidTicket(m_launcherTicket)) {
        m_exitCode = runtime::LaunchExitCode::DescriptorRejected;
        m_startupExitRequested = true;
        TD_LOG_ERROR("[SessionLaunch] Invalid launcher session ticket");
        return false;
    }

    const std::filesystem::path bootstrapDescriptor{descriptorPath};
    const std::filesystem::path bootstrapOutcome =
        bootstrapDescriptor.parent_path() /
        (m_launcherTicket + ".outcome.ini");
    m_launchOutcome.beginBootstrap(m_launcherTicket,
                                   bootstrapOutcome.string());
    engine::LauncherSessionDescriptor descriptor;
    container::String error;
    if (!engine::GameLaunchDescriptor::loadFromBootstrapFile(
            descriptorPath, descriptor, &error)) {
        TD_LOG_ERROR("[SessionLaunch] Failed to load launcher descriptor");
        failExternalLaunch(
            runtime::LaunchOutcomeStage::Descriptor,
            runtime::LaunchOutcomeCode::DescriptorRejected,
            error.empty() ? container::StringView{"invalid launcher descriptor"}
                          : container::StringView{error},
            false, runtime::LaunchExitCode::DescriptorRejected);
        return false;
    }
    m_launcherSession = std::move(descriptor);
    return true;
}

void ApplicationHost::Impl::applyHostRenderQualityOverrides() {
    engine::RenderFeatureQualityOverrides feature;
    engine::RenderDisplayOverrides display;
    bool hasOverride = false;
    auto& commandLine = engine::CommandLine::instance();
    if (commandLine.hasParam("fxaa")) {
        display.antiAliasingMode = commandLine.getBoolParam("fxaa", false)
            ? engine::RenderAntiAliasingMode::Fxaa
            : engine::RenderAntiAliasingMode::Off;
        hasOverride = true;
    }
    if (commandLine.hasParam("texture-filter")) {
        display.textureFilter = static_cast<uint32_t>(std::max(
            0, commandLine.getIntParam("texture-filter", 2)));
        hasOverride = true;
    }
    if (commandLine.hasParam("anisotropy")) {
        display.anisotropyLevel = static_cast<uint32_t>(std::max(
            1, commandLine.getIntParam("anisotropy", 2)));
        hasOverride = true;
    }
    if (commandLine.hasParam("resolution")) {
        if (const auto resolution = commandLine.getResolutionParam()) {
            display.width = resolution->first;
            display.height = resolution->second;
            hasOverride = true;
        }
    }
    if (commandLine.hasParam("render-width")) {
        display.width = static_cast<uint32_t>(std::max(
            1, commandLine.getIntParam("render-width", 800)));
        hasOverride = true;
    }
    if (commandLine.hasParam("render-height")) {
        display.height = static_cast<uint32_t>(std::max(
            1, commandLine.getIntParam("render-height", 600)));
        hasOverride = true;
    }
    if (commandLine.hasParam("maximum-particles")) {
        feature.maximumParticles = static_cast<uint32_t>(std::max(
            0, commandLine.getIntParam("maximum-particles", 2500)));
        hasOverride = true;
    }
    if (commandLine.hasParam("texture-reduction")) {
        feature.textureReductionFactor = static_cast<uint32_t>(std::max(
            0, commandLine.getIntParam("texture-reduction", 0)));
        hasOverride = true;
    }
    if (hasOverride) {
        game::GameDataLoader::instance().setRenderQualityExternalOverrides(
            feature, display);
    }
}

void ApplicationHost::Impl::startRequestedSession() {
    auto& commandLine = engine::CommandLine::instance();
    bool launchQueued = false;
    if (m_launcherSession) {
        engine::GameStartInfo sessionInfo =
            std::move(m_launcherSession->startInfo);
        m_launcherSession.reset();
        container::String sequenceError;
        if (!validateAuthoredSequenceIdentity(sessionInfo, sequenceError)) {
            TD_LOG_ERROR("[SessionLaunch] Invalid authored sequence identity");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Descriptor,
                runtime::LaunchOutcomeCode::DescriptorRejected,
                sequenceError, false,
                runtime::LaunchExitCode::DescriptorRejected);
            return;
        }
        if (!m_logicIntents.post(
                runtime::StartGameIntent{.info = std::move(sessionInfo)},
                0)) {
            TD_LOG_ERROR("[SessionLaunch] Failed to queue launcher descriptor");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Queued,
                runtime::LaunchOutcomeCode::QueueRejected,
                "could not queue launcher descriptor", true,
                runtime::LaunchExitCode::QueueRejected);
            return;
        }
        launchQueued = true;
        TD_LOG_INFO("[SessionLaunch] Queued launcher descriptor");
        if (!m_launchOutcome.publish(
                runtime::LaunchOutcomeStage::Queued,
                runtime::LaunchOutcomeCode::Pending,
                "launcher descriptor queued", false, false,
                runtime::LaunchExitCode::Success)) {
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Queued,
                runtime::LaunchOutcomeCode::OutcomePublicationFailed,
                "could not publish launcher outcome", true,
                runtime::LaunchExitCode::OutcomePublicationFailed);
            return;
        }
    }
    if (!launchQueued &&
        commandLine.hasParam("session-ticket")) {
        engine::GameStartInfo sessionInfo;
        container::String error;
        const container::String ticket =
            commandLine.getParam("session-ticket");
        if (engine::GameLaunchDescriptor::isValidTicket(ticket)) {
            m_launchOutcome.begin(ticket);
        }
        if (!engine::GameLaunchDescriptor::loadFromVfs(
                ticket, sessionInfo, &error)) {
            TD_LOG_ERROR("[SessionLaunch] Failed to load session descriptor");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Descriptor,
                runtime::LaunchOutcomeCode::DescriptorRejected,
                error.empty() ? container::StringView{"invalid session descriptor"}
                              : container::StringView{error},
                false, runtime::LaunchExitCode::DescriptorRejected);
            return;
        } else if (!validateAuthoredSequenceIdentity(sessionInfo, error)) {
            TD_LOG_ERROR("[SessionLaunch] Invalid authored sequence identity");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Descriptor,
                runtime::LaunchOutcomeCode::DescriptorRejected,
                error, false, runtime::LaunchExitCode::DescriptorRejected);
            return;
        } else if (!m_logicIntents.post(
                       runtime::StartGameIntent{.info = std::move(sessionInfo)},
                       0)) {
            TD_LOG_ERROR("[SessionLaunch] Failed to queue session descriptor");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Queued,
                runtime::LaunchOutcomeCode::QueueRejected,
                "could not queue session descriptor", true,
                runtime::LaunchExitCode::QueueRejected);
            return;
        } else {
            launchQueued = true;
            TD_LOG_INFO("[SessionLaunch] Queued session descriptor");
            if (!m_launchOutcome.publish(
                    runtime::LaunchOutcomeStage::Queued,
                    runtime::LaunchOutcomeCode::Pending,
                    "session descriptor queued", false, false,
                    runtime::LaunchExitCode::Success)) {
                failExternalLaunch(
                    runtime::LaunchOutcomeStage::Queued,
                    runtime::LaunchOutcomeCode::OutcomePublicationFailed,
                    "could not publish launcher outcome", true,
                    runtime::LaunchExitCode::OutcomePublicationFailed);
                return;
            }
        }
    }

    container::String debugWorldMap;
#if TD_DEBUG_ENABLED
    debugWorldMap = commandLine.getParam("debug-world-map");
#endif
    if (!launchQueued &&
        (commandLine.hasParam("direct-start") || !debugWorldMap.empty())) {
        auto directStartInfo =
            engine::GameStartInfoBuilder::makeDirectStart(commandLine);
        if (!debugWorldMap.empty()) directStartInfo.mapName = debugWorldMap;
        if (directStartInfo.mode == engine::GameMode::SinglePlayer) {
            const game::Campaign* campaign = nullptr;
            const game::Mission* mission =
                game::CampaignManager::instance().findMissionByMap(
                    directStartInfo.mapName, &campaign);
            if (campaign && mission) {
                const bool hasExplicitFaction =
                    !directStartInfo.localPlayerTemplateName.empty() ||
                    !directStartInfo.localPlayerSide.empty();
                const container::StringView faction =
                    campaign->playerFactionName;
                if (!hasExplicitFaction) {
                    if (faction.empty()) {
                        TD_LOG_ERROR(
                            "[DirectStart] Campaign '{}' does not declare PlayerFaction for map '{}'",
                            campaign->name, directStartInfo.mapName);
                        failExternalLaunch(
                            runtime::LaunchOutcomeStage::Descriptor,
                            runtime::LaunchOutcomeCode::DescriptorRejected,
                            "campaign does not declare PlayerFaction", false,
                            runtime::LaunchExitCode::DescriptorRejected);
                        return;
                    }
                    engine::GameStartInfoBuilder::applyCampaignFaction(
                        directStartInfo, faction);
                }
                directStartInfo.sequence.type =
                    engine::GameSequenceType::Campaign;
                directStartInfo.sequence.campaignName = campaign->name;
                directStartInfo.sequence.missionName = mission->name;
                TD_LOG_INFO(
                    "[DirectStart] Resolved campaign='{}' mission='{}' faction='{}' explicitFaction={} from map '{}'",
                    campaign->name, mission->name, faction,
                    hasExplicitFaction,
                    directStartInfo.mapName);
            } else {
                const bool hasExplicitFaction =
                    !directStartInfo.localPlayerTemplateName.empty() ||
                    !directStartInfo.localPlayerSide.empty();
                if (!hasExplicitFaction) {
                    TD_LOG_ERROR(
                        "[DirectStart] Campaign map '{}' is absent from Campaign.ini and no faction identity was supplied",
                        directStartInfo.mapName);
                    failExternalLaunch(
                        runtime::LaunchOutcomeStage::Descriptor,
                        runtime::LaunchOutcomeCode::DescriptorRejected,
                        "campaign map is absent from Campaign.ini and has no explicit faction",
                        false, runtime::LaunchExitCode::DescriptorRejected);
                    return;
                }
                TD_LOG_WARN(
                    "[DirectStart] Starting standalone campaign map '{}' with explicit faction and no continuable sequence",
                    directStartInfo.mapName);
            }
        } else if (directStartInfo.mode == engine::GameMode::Challenge) {
            const int generalIndex = game::ChallengeGenerals::instance()
                .getGeneralByTemplateName(
                    directStartInfo.localPlayerTemplateName);
            const game::GeneralPersona* general = generalIndex >= 0
                ? &game::ChallengeGenerals::instance().getGeneral(generalIndex)
                : nullptr;
            game::Campaign* campaign = general && !general->campaign.empty()
                ? game::CampaignManager::instance().findCampaign(
                    general->campaign)
                : nullptr;
            const game::Campaign* mapCampaign = nullptr;
            const game::Mission* mission =
                game::CampaignManager::instance().findMissionByMap(
                    directStartInfo.mapName, &mapCampaign);
            if (!general || !campaign || !campaign->isChallengeCampaign ||
                !mission || mapCampaign != campaign) {
                TD_LOG_ERROR(
                    "[DirectStart] Challenge map '{}' and player template '{}' do not resolve to one challenge campaign",
                    directStartInfo.mapName,
                    directStartInfo.localPlayerTemplateName);
                failExternalLaunch(
                    runtime::LaunchOutcomeStage::Descriptor,
                    runtime::LaunchOutcomeCode::DescriptorRejected,
                    "challenge map and player template do not resolve to one challenge campaign",
                    false, runtime::LaunchExitCode::DescriptorRejected);
                return;
            }
            directStartInfo.sequence.type =
                engine::GameSequenceType::Challenge;
            directStartInfo.sequence.campaignName = campaign->name;
            directStartInfo.sequence.missionName = mission->name;
            directStartInfo.sequence.challengeGeneral =
                general->playerTemplateName;
            TD_LOG_INFO(
                "[DirectStart] Resolved challenge campaign='{}' mission='{}' general='{}'",
                campaign->name, mission->name,
                general->playerTemplateName);
        }
        container::String sequenceError;
        if (!validateAuthoredSequenceIdentity(
                directStartInfo, sequenceError)) {
            TD_LOG_ERROR("[DirectStart] Invalid authored sequence identity");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Descriptor,
                runtime::LaunchOutcomeCode::DescriptorRejected,
                sequenceError, false,
                runtime::LaunchExitCode::DescriptorRejected);
            return;
        }
        TD_LOG_INFO(
            "[DirectStart] Starting normal game session from command line: map='{}' debugMap={}",
            directStartInfo.mapName, !debugWorldMap.empty());
        if (!m_logicIntents.post(
                runtime::StartGameIntent{
                    .info = std::move(directStartInfo)},
                0)) {
            TD_LOG_ERROR("[DirectStart] Failed to queue game start");
            failExternalLaunch(
                runtime::LaunchOutcomeStage::Queued,
                runtime::LaunchOutcomeCode::QueueRejected,
                "could not queue direct game start", true,
                runtime::LaunchExitCode::QueueRejected);
            return;
        }
        launchQueued = true;
    }

    if (!launchQueued) {
        TD_LOG_ERROR(
            "[SessionLaunch] No session descriptor, ticket or direct-start request was supplied");
        failExternalLaunch(
            runtime::LaunchOutcomeStage::Descriptor,
            runtime::LaunchOutcomeCode::DescriptorRejected,
            "the in-game host requires a session launch request", false,
            runtime::LaunchExitCode::DescriptorRejected);
    }
}

void ApplicationHost::Impl::failExternalLaunch(
    runtime::LaunchOutcomeStage stage,
    runtime::LaunchOutcomeCode code,
    container::StringView reason, bool retryable, int exitCode) {
    m_exitCode = exitCode;
    m_startupExitRequested = true;
    if (m_launchOutcome.active()) {
        if (!m_launchOutcome.publish(
                stage, code, reason, retryable, true, exitCode)) {
            m_exitCode = runtime::LaunchExitCode::OutcomePublicationFailed;
        }
    }
}

void ApplicationHost::Impl::publishLaunchOutcomeForProjection(
    const runtime::GameUiProjection& projection) {
    if (!m_launchOutcome.active() || m_launchOutcome.terminalPublished()) return;

    if (!projection.loadingError.empty()) {
        m_exitCode = runtime::LaunchExitCode::LoadingFailed;
        m_startupExitRequested = true;
        static_cast<void>(m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Loading,
            runtime::LaunchOutcomeCode::LoadingFailed,
            projection.loadingError, true, true, m_exitCode));
        return;
    }

    switch (projection.gameState) {
    case engine::GameState::Loading:
        if (!m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Loading,
            runtime::LaunchOutcomeCode::Pending,
            projection.loadingStatus, false, false,
            runtime::LaunchExitCode::Success)) {
            m_exitCode = runtime::LaunchExitCode::OutcomePublicationFailed;
            m_startupExitRequested = true;
        }
        break;
    case engine::GameState::Running:
    case engine::GameState::Paused:
        if (!m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Running,
            runtime::LaunchOutcomeCode::Running,
            "session running", false, false,
            runtime::LaunchExitCode::Success)) {
            m_exitCode = runtime::LaunchExitCode::OutcomePublicationFailed;
            m_startupExitRequested = true;
        }
        break;
    case engine::GameState::Result:
    case engine::GameState::Transitioning:
        if (!m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Result,
            runtime::LaunchOutcomeCode::Running,
            "session result available", false, false,
            runtime::LaunchExitCode::Success)) {
            m_exitCode = runtime::LaunchExitCode::OutcomePublicationFailed;
            m_startupExitRequested = true;
        }
        break;
    case engine::GameState::Idle:
        break;
    }
}

void ApplicationHost::Impl::initialize() {
    TheSubsystemList = &m_subsystems;

    if (!prepareLauncherSessionBootstrap()) return;

    engine::resource::ResourceSchedulerConfig resourceConfig;
    resourceConfig.maxInFlight =
        platform::runtime::resourceWorkerCount() +
        platform::runtime::sceneResourceWorkerCount();
    resourceConfig.perKind[static_cast<size_t>(
        engine::resource::ResourceKind::Scene)].maxInFlight =
            platform::runtime::sceneResourceWorkerCount();
    m_resourceScheduler =
        std::make_unique<engine::resource::ResourceSchedulerRuntime>(
            resourceConfig);
    engine::resource::installResourceSchedulerRuntime(
        m_resourceScheduler.get());

    m_fileSystem = m_launcherSession
        ? std::make_unique<FileSystemSubsystem>(m_launcherSession->content)
        : std::make_unique<FileSystemSubsystem>();
    m_subsystems.initSubsystem(m_fileSystem.get());
    m_gameContent = std::make_unique<GameContentSubsystem>();
    m_subsystems.initSubsystem(m_gameContent.get());

    TD_LOG_INFO("[Audio] Constructing subsystem");
    m_audio = std::make_unique<engine::AudioSubsystem>();
    TD_LOG_INFO("[Audio] Initializing subsystem");
    m_subsystems.initSubsystem(m_audio.get());

    m_gameText = std::make_unique<GameTextSubsystem>();
    m_subsystems.initSubsystem(m_gameText.get());
    m_renderer = std::make_unique<RendererSubsystem>();
    m_subsystems.initSubsystem(m_renderer.get());
    m_fontLibrary = std::make_unique<FontLibrarySubsystem>();
    m_subsystems.initSubsystem(m_fontLibrary.get());
    m_gui = std::make_unique<GuiSubsystem>();
    m_subsystems.initSubsystem(m_gui.get());
    m_inGameGui = std::make_unique<InGameGuiSubsystem>(
        m_logicIntents, *m_audio);
    m_subsystems.initSubsystem(m_inGameGui.get());

    m_subsystems.postInitAll();
    game::GameDataLoader::instance().setRenderDisplayCapabilities(
        m_renderer->renderDisplayCapabilities());
    applyHostRenderQualityOverrides();
    startRequestedSession();
    m_textureManager = std::make_unique<engine::TextureManager>();

#if TD_DEBUG_ENABLED
    TD_LOG_INFO(LOG_DEBUG_HINT.data());
#endif
}

int ApplicationHost::Impl::run() {
    if (m_hasRun) return 0;
    m_hasRun = true;
    if (m_startupExitRequested) {
        shutdown();
        return m_exitCode;
    }
    TD_LOG_INFO("Starting render loop...");

    bool running = true;
    int frameCount = 0;
    m_presentation =
        std::make_unique<PresentationCoordinator>(
            *m_renderer, *m_audio, m_logicIntents);
    m_input = std::make_unique<InputCoordinator>(
        *m_renderer, *m_inGameGui, *m_textureManager,
        *m_presentation, m_logicIntents);
    m_framePacer = std::make_unique<FramePacer>();
    m_gameUiProjectionMailbox.reset();
    engine::MappedImageCollection::instance().clearSession();
    engine::StringTable::instance().clearMapStringFile();
    m_input->setGameProjection(m_mainGameProjection);
    m_inGameGui->setGameProjection(m_mainGameProjection);
    m_presentation->setGameProjection(m_mainGameProjection);
    m_presentation->configureDebugOptions();
    m_renderFrames.reset();
    auto renderAttached = std::make_shared<std::promise<void>>();
    std::future<void> renderAttachedFuture = renderAttached->get_future();
    m_renderThread.start(
        platform::runtime::ThreadRole::Render, L"GeneralsTD Render",
        [this, renderAttached](std::stop_token stopToken) {
            try {
                m_renderer->attachRenderThread();
                renderAttached->set_value();
            } catch (...) {
                const std::exception_ptr initializationFailure =
                    std::current_exception();
                // Backend initialization may have progressed far enough to
                // own D3D12 resources before a later setup step failed. Keep
                // that partial teardown on the render owner thread too.
                try {
                    m_renderer->releaseRenderResourcesOnRenderThread();
                } catch (...) {
                    // Preserve the initialization exception reported through
                    // the startup barrier.
                }
                renderAttached->set_exception(initializationFailure);
                std::rethrow_exception(initializationFailure);
            }
            std::exception_ptr failure;
            {
                engine::TextureManager renderTextureManager;
                try {
                    engine::UiDrawList uiDrawList;
                    while (m_renderFrames.waitTake(uiDrawList, stopToken)) {
                        m_presentation->renderRecordedUi(
                            renderTextureManager, uiDrawList);
                        uiDrawList.clear();
                    }
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            // D3D12/world resources are released on their owner thread. SDL
            // window teardown remains in the main-thread subsystem shutdown.
            m_renderer->releaseRenderResourcesOnRenderThread();
            if (failure) std::rethrow_exception(failure);
        });
    // Startup ownership barrier only: logic must not submit a render control
    // before RendererSubsystem has switched from direct calls to its mailbox.
    try {
        renderAttachedFuture.get();
    } catch (const std::exception& exception) {
        TD_LOG_ERROR(
            "[RenderThread] Startup ownership/backend attach failed: {}",
            exception.what());
        m_exitCode = runtime::LaunchExitCode::RenderThreadFailed;
        static_cast<void>(m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Runtime,
            runtime::LaunchOutcomeCode::RenderThreadFailed,
            exception.what(), true, true, m_exitCode));
        shutdown();
        return m_exitCode;
    } catch (...) {
        constexpr container::StringView reason =
            "unknown render-thread startup failure";
        TD_LOG_ERROR("[RenderThread] {}", reason);
        m_exitCode = runtime::LaunchExitCode::RenderThreadFailed;
        static_cast<void>(m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Runtime,
            runtime::LaunchOutcomeCode::RenderThreadFailed,
            reason, true, true, m_exitCode));
        shutdown();
        return m_exitCode;
    }
    // SDL-only initialization cannot report backend-dependent capabilities
    // such as FXAA. Refresh the quality resolver after D3D12 is live.
    game::GameDataLoader::instance().setRenderDisplayCapabilities(
        m_renderer->renderDisplayCapabilities());
    m_logicExitRequested.store(false, std::memory_order_release);
    m_logicThread.start(
        platform::runtime::ThreadRole::Logic, L"GeneralsTD Logic",
        [this](std::stop_token stopToken) {
            std::exception_ptr failure;
            try {
                auto nextTick = std::chrono::steady_clock::now();
                constexpr int maxCatchUpPeriods = 2;
                int logicFrame = 0;
                while (!stopToken.stop_requested()) {
                    const auto iterationStarted =
                        std::chrono::steady_clock::now();
                    // Apply local ScoreScreen music before a staged Next/Retry
                    // intent can replace the session. New-session extraction
                    // later in this iteration therefore remains authoritative.
                    m_audio->applyPendingScoreScreenMusicRequest();
                    static_cast<void>(m_logicIntents.drainAndApply(
                        engine::GameLogic::instance()));
                    // Endpoint publication belongs to the preceding render
                    // boundary and must be visible before this confirmed
                    // logic update. Loading admits only endpoint facts; the
                    // coordinator keeps resource/natural completion deferred.
                    engine::GameLogic& gameLogic =
                        engine::GameLogic::instance();
                    engine::GameSession* feedbackSession =
                        gameLogic.currentSession();
                    if (!feedbackSession ||
                        !feedbackSession->framePort().result().faulted()) {
                        m_presentation->admitRenderAnimationFeedback();
                    }
                    gameLogic.update();
                    // ZH local pause and deterministic FREEZE_TIME are
                    // deliberately different authorities. Only GameState::
                    // Paused stops playback; script popup pause leaves music
                    // running, while the in-game menu pauses every bus.
                    const engine::GameState gameState =
                        gameLogic.getState();
                    const bool locallyPaused =
                        gameState == engine::GameState::Paused;
                    // Result keeps the final world alive for transactional
                    // Next/Retry rollback, unlike ZH's immediate teardown.
                    // Pause its non-music voices so that retained ambient and
                    // looping SFX cannot leak through the ScoreScreen. Keep
                    // the same hold during Loading/Transitioning and resume
                    // only when the committed session reaches Running.
                    const bool pauseWorldAudio = locallyPaused ||
                        gameState == engine::GameState::Loading ||
                        gameState == engine::GameState::Result ||
                        gameState == engine::GameState::Transitioning;
                    m_audio->setLocalPausePolicy(
                        pauseWorldAudio,
                        locallyPaused && gameLogic.localPauseSourceActive(
                            engine::LocalPauseSource::InGameMenu));
                    const auto simulationFinished =
                        std::chrono::steady_clock::now();

                    // A structural fault may occur after a phase has already
                    // changed live ECS or presentation journals.  Keep the
                    // previously published UI/world/audio endpoint instead
                    // of exposing that uncommitted suffix.
                    engine::GameSession* endpointSession =
                        gameLogic.currentSession();
                    const bool endpointCommitted = !endpointSession ||
                        !endpointSession->framePort().result().faulted();
                    if (endpointCommitted) {
                        static_cast<void>(m_gameUiProjectionMailbox.publish(
                            m_gameUiProjectionPublisher.build(gameLogic)));
                    }
                    const auto uiProjectionFinished =
                        std::chrono::steady_clock::now();
                    if (endpointCommitted) {
                        m_presentation->extractAndSubmit(logicFrame);
                    }
                    const auto extractionFinished =
                        std::chrono::steady_clock::now();

                    // Audio presentation is logic-ordered but owns no ECS.
                    // Natural completions admitted here become visible to the
                    // following confirmed tick.
                    m_audio->update();
                    if (endpointCommitted) {
                        m_presentation->admitAudioCompletions();
                    }
                    m_presentation->admitRenderStartupReadiness();
                    m_presentation->applyNonSessionRenderQuality();
                    const auto iterationWorkFinished =
                        std::chrono::steady_clock::now();

#if TD_DEBUG_ENABLED
                    const auto micros = [](auto begin, auto end) {
                        return std::chrono::duration_cast<
                            std::chrono::microseconds>(end - begin).count();
                    };
                    const auto totalMicros = micros(
                        iterationStarted, iterationWorkFinished);
                    const auto logicState =
                        engine::GameLogic::instance().getState();
                    const auto confirmedTick =
                        engine::GameLogic::instance().currentTick();
                    const bool reportRunningTick =
                        logicState == engine::GameState::Running &&
                        (confirmedTick <= 16u || confirmedTick % 30u == 0u);
                    const bool reportSlowNonRunning =
                        logicState != engine::GameState::Running &&
                        totalMicros >= 100'000 &&
                        (logicFrame % 30 == 0);
                    if (reportRunningTick || reportSlowNonRunning) {
                        TD_LOG_INFO(
                            "[LogicTiming] loop={} tick={} state={} simulation={}us uiProjection={}us presentationExtract={}us audioFeedback={}us total={}us",
                            logicFrame,
                            confirmedTick,
                            static_cast<uint32_t>(logicState),
                            micros(iterationStarted, simulationFinished),
                            micros(simulationFinished, uiProjectionFinished),
                            micros(uiProjectionFinished, extractionFinished),
                            micros(extractionFinished, iterationWorkFinished),
                            totalMicros);
                    }
#endif

                    if (engine::GameLogic::instance().takeExitRequest()) {
                        m_logicExitRequested.store(
                            true, std::memory_order_release);
                    }
                    ++logicFrame;

                    const int framesPerSecond = std::clamp(
                        engine::GameLogic::instance()
                            .getCurrentGameInfo().gameSpeedFPS,
                        1, 120);
                    const auto period = std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            1.0 / static_cast<double>(framesPerSecond)));
                    nextTick += period;
                    const auto now = std::chrono::steady_clock::now();
                    if (now < nextTick) {
                        std::this_thread::sleep_until(nextTick);
                    } else if (now - nextTick >=
                               period * maxCatchUpPeriods) {
                        // Drop stale catch-up debt without delaying the next
                        // tick. A later on-time tick continues from this new
                        // absolute deadline.
                        nextTick = now;
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }
            engine::GameLogic::instance().clearGameData();
            if (failure) std::rethrow_exception(failure);
        });
#if TD_DEBUG_ENABLED
    const int exitAfterFrames = engine::CommandLine::instance().getIntParam(
        "exit-after-frames", 0);
    const bool debugWorldOnly = engine::CommandLine::instance().getBoolParam(
        "debug-world-only", false);
#else
    constexpr bool debugWorldOnly = false;
#endif

    while (running) {
        bool inGameUiUpdatedForProjection = false;
        if (std::exception_ptr renderFailure = m_renderThread.failure()) {
            container::String failureReason = "render thread failed";
            try {
                std::rethrow_exception(renderFailure);
            } catch (const std::exception& exception) {
                static_cast<void>(exception);
                failureReason = exception.what();
                TD_LOG_ERROR("[RenderThread] {}", exception.what());
            } catch (...) {
                TD_LOG_ERROR("[RenderThread] Unknown failure");
            }
            m_exitCode = runtime::LaunchExitCode::RenderThreadFailed;
            static_cast<void>(m_launchOutcome.publish(
                runtime::LaunchOutcomeStage::Runtime,
                runtime::LaunchOutcomeCode::RenderThreadFailed,
                failureReason, true, true, m_exitCode));
            running = false;
            break;
        }
        if (std::exception_ptr logicFailure = m_logicThread.failure()) {
            container::String failureReason = "logic thread failed";
            try {
                std::rethrow_exception(logicFailure);
            } catch (const std::exception& exception) {
                static_cast<void>(exception);
                failureReason = exception.what();
                TD_LOG_ERROR("[LogicThread] {}", exception.what());
            } catch (...) {
                TD_LOG_ERROR("[LogicThread] Unknown failure");
            }
            m_exitCode = runtime::LaunchExitCode::LogicThreadFailed;
            static_cast<void>(m_launchOutcome.publish(
                runtime::LaunchOutcomeStage::Runtime,
                runtime::LaunchOutcomeCode::LogicThreadFailed,
                failureReason, true, true, m_exitCode));
            running = false;
            break;
        }
        runtime::GameUiProjection gameUiProjection;
        if (m_gameUiProjectionMailbox.tryTake(gameUiProjection)) {
            m_mainGameProjection = gameUiProjection;
            if (gameUiProjection.hasSession) {
                const auto& layers =
                    gameUiProjection.mappedImageContentLayers;
                engine::MappedImageCollection::instance().activateSession(
                    gameUiProjection.presentationEpoch,
                    layers
                        ? container::Span<
                              const engine::ui::MappedImageContentLayer>{
                              *layers}
                        : container::Span<
                              const engine::ui::MappedImageContentLayer>{});
                const auto& mapStrings =
                    gameUiProjection.mapStringContentLayer;
                if (mapStrings && gameUiProjection.presentationEpoch != 0) {
                    container::String mapStringError;
                    if (!engine::StringTable::instance().activateMapStringFile(
                            gameUiProjection.presentationEpoch,
                            mapStrings->content, &mapStringError)) {
                        TD_LOG_WARN(
                            "[GameText] Map string layer '{}' degraded: {}",
                            mapStrings->sourcePath, mapStringError);
                    }
                } else {
                    engine::StringTable::instance().clearMapStringFile();
                }
            } else {
                engine::MappedImageCollection::instance().clearSession();
                engine::StringTable::instance().clearMapStringFile();
            }
            m_input->setGameProjection(gameUiProjection);
            m_inGameGui->setGameProjection(gameUiProjection);
            m_presentation->setGameProjection(gameUiProjection);
            // Install/deactivate the WND tree before SDL input can observe
            // the new projection. A later update in this frame is skipped so
            // timers and one-shot UI consumers still advance exactly once.
            m_inGameGui->update();
            inGameUiUpdatedForProjection = true;
            publishLaunchOutcomeForProjection(gameUiProjection);
            if (m_startupExitRequested ||
                (m_launchOutcome.terminalPublished() &&
                 m_exitCode != runtime::LaunchExitCode::Success)) {
                running = false;
                continue;
            }
        }
        if (m_logicExitRequested.exchange(
                false, std::memory_order_acq_rel)) {
            TD_LOG_INFO("[Main] In-game result requested return to launcher");
            m_normalGameExit = true;
            running = false;
            continue;
        }
        const float presentationDeltaSeconds = m_framePacer->beginFrame();
        if (!m_input->processFrame(presentationDeltaSeconds, running)) {
            continue;
        }
        // Mouse motion, wheel ticks and repeated presentation intents are
        // allowed to lose stale samples when the logic thread is behind. An
        // input backlog must never tear down an otherwise valid game or block
        // the window thread; the next accepted sample still takes effect on
        // the next logic boundary. Keep a monotonic Debug diagnostic without
        // changing Release control flow.
        const runtime::GameLogicIntentStats intentStats =
            m_logicIntents.stats();
        if (intentStats.rejectedOverflow > m_reportedInputOverflow) {
            const uint64_t dropped = intentStats.rejectedOverflow -
                m_reportedInputOverflow;
            m_reportedInputOverflow = intentStats.rejectedOverflow;
            TD_LOG_WARN(
                "[LogicThread] Input mailbox full; dropped {} stale input intents",
                dropped);
        }

        m_input->updateAfterLogicTick(frameCount);
        m_presentation->admitExtractedWorldFrame();
        m_input->synchronizePresentationState();

        // Mutable WND state remains main-thread-owned. Audio has moved to the
        // logic thread so updateAll() is intentionally no longer used here.
        if (!inGameUiUpdatedForProjection) {
            m_inGameGui->update();
        }

        try {
            engine::UiDrawList uiDrawList = m_presentation->recordUi(
                *m_textureManager, *m_inGameGui, debugWorldOnly);
            if (!m_renderFrames.publish(std::move(uiDrawList))) {
                running = false;
                break;
            }
        } catch (const std::exception& e) {
            static_cast<void>(e);
            TD_LOG_ERROR("[Main] EXCEPTION in render: {}", e.what());
            m_exitCode = runtime::LaunchExitCode::MainPresentationFailed;
            static_cast<void>(m_launchOutcome.publish(
                runtime::LaunchOutcomeStage::Runtime,
                runtime::LaunchOutcomeCode::MainPresentationFailed,
                e.what(), true, true, m_exitCode));
            running = false;
            break;
        } catch (...) {
            TD_LOG_ERROR("[Main] UNKNOWN EXCEPTION in render");
            m_exitCode = runtime::LaunchExitCode::MainPresentationFailed;
            static_cast<void>(m_launchOutcome.publish(
                runtime::LaunchOutcomeStage::Runtime,
                runtime::LaunchOutcomeCode::MainPresentationFailed,
                "unknown main presentation failure", true, true,
                m_exitCode));
            running = false;
            break;
        }

        ++frameCount;
        if (frameCount == 1) TD_LOG_INFO(LOG_FIRST_FRAME.data());
#if TD_DEBUG_ENABLED
        if (exitAfterFrames > 0 && frameCount >= exitAfterFrames) {
            TD_LOG_INFO("[Main] Debug frame limit reached ({})",
                        exitAfterFrames);
            running = false;
        }
#endif
        m_framePacer->pace(m_mainGameProjection);
    }

    if (m_launchOutcome.active() && !m_launchOutcome.terminalPublished()) {
        static_cast<void>(m_launchOutcome.publish(
            runtime::LaunchOutcomeStage::Shutdown,
            m_normalGameExit ? runtime::LaunchOutcomeCode::Completed
                             : runtime::LaunchOutcomeCode::Cancelled,
            m_normalGameExit ? container::StringView{"session completed"}
                             : container::StringView{"application closed"},
            false, true, m_exitCode));
    }

    shutdown();
    TD_LOG_INFO(LOG_DONE.data());
    return m_exitCode;
}

void ApplicationHost::Impl::shutdown() noexcept {
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;

    m_logicIntents.close();
    if (m_presentation) m_presentation->closeWorldFrameIngress();
    m_logicThread.requestStop();
    m_logicThread.join();
    m_gameUiProjectionMailbox.close();
    m_renderFrames.close();
    m_renderThread.requestStop();
    m_renderThread.join();
    engine::MappedImageCollection::instance().clearSession();
    engine::StringTable::instance().clearMapStringFile();
    m_input.reset();
    m_presentation.reset();
    m_framePacer.reset();
    // Registered subsystems are owned here and are destroyed below in exact
    // reverse registration order. Their destructors perform the one shutdown;
    // do not call shutdownAll() first and then repeat shutdown from each dtor.
    m_inGameGui.reset();
    m_gui.reset();
    m_fontLibrary.reset();
    m_renderer.reset();
    m_gameText.reset();
    m_audio.reset();
    m_gameContent.reset();
    engine::resource::installResourceSchedulerRuntime(nullptr);
    if (m_resourceScheduler) m_resourceScheduler->shutdown();
    m_resourceScheduler.reset();
    m_fileSystem.reset();
    m_textureManager.reset();
    TheSubsystemList = nullptr;
}

ApplicationHost::ApplicationHost() : m_impl(std::make_unique<Impl>()) {
    m_impl->initialize();
}

ApplicationHost::~ApplicationHost() = default;

int ApplicationHost::run() {
    return m_impl->run();
}

} // namespace app
