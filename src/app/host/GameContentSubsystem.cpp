#include "GameContentSubsystem.h"

#include "core/constants/Paths.h"
#include "game/base/CampaignManager.h"
#include "game/base/ChallengeGenerals.h"
#include "game/base/MapCache.h"
#include "game/base/MultiplayerData.h"
#include "game/text/LanguageFilter.h"
#include "CommandLine.h"

namespace app {

GameContentSubsystem::GameContentSubsystem() {
    setName("GameContent");
}

GameContentSubsystem::~GameContentSubsystem() {
    shutdown();
}

void GameContentSubsystem::init() {
    if (m_initialized) return;

    const auto& commandLine = engine::CommandLine::instance();
    game::TheCampaignManager = &game::CampaignManager::instance();
    game::TheChallengeGenerals = &game::ChallengeGenerals::instance();
    // Direct-start still needs the campaign catalog to derive the authored
    // PlayerFaction from a mission map. It does not become continuable merely
    // because the read-only catalog is present; ScoreScreen/Next ownership
    // remains in the launcher/session descriptor contract.
    game::TheCampaignManager->loadFromIni(CAMPAIGN_INI.data());
    game::TheChallengeGenerals->loadFromIni(CHALLENGEMODE_INI.data());

    game::TheMapCache = &game::MapCache::instance();
    bool debugWorldSession = false;
#if TD_DEBUG_ENABLED
    debugWorldSession = commandLine.hasParam("debug-world-map");
#endif
    const bool externallySelectedMap =
        commandLine.hasParam("session-descriptor") ||
        commandLine.hasParam("session-ticket") ||
        commandLine.hasParam("direct-start") ||
        debugWorldSession;
    if (!externallySelectedMap) game::TheMapCache->init();

    game::MultiplayerData::instance().loadFromIni(
        MULTIPLAYER_INI.data(), PLAYERTEMPLATE_INI.data());
    m_initialized = true;
}

void GameContentSubsystem::shutdown() {
    if (!m_initialized) return;
    engine::text::LanguageFilter::instance().clear();
    m_initialized = false;
}

} // namespace app
