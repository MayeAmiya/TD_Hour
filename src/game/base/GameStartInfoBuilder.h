#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
namespace engine {

class CommandLine;

class GameStartInfoBuilder {
public:
    static GameStartInfo makeDirectStart(const CommandLine& commandLine);

    // Normalizes the three authored campaign selector identities (USA, GLA,
    // China) into the PlayerTemplate identity consumed by a game session.
    // Custom PlayerTemplate names remain intact so campaign mods do not get
    // silently rewritten to a stock faction.
    static void applyCampaignFaction(GameStartInfo& info, container::StringView requestedFaction);
    static void applyLocalSide(GameStartInfo& info, const container::String& requestedSide);
    static GameMode parseMode(const container::String& value, GameMode fallback = GameMode::Skirmish);
};

} // namespace engine
