#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/base/GameStartInfoBuilder.h"
#include "game/base/MapContentIdentity.h"
#include "CommandLine.h"
#include "game/base/GameBalanceConstants.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <utility>

namespace engine {

namespace {

container::String lower(container::String value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

constexpr auto trim = container::trimAsciiCopy;

struct CanonicalFactionIdentity final {
    container::StringView templateName;
    container::StringView side;
    container::StringView baseSide;
};

const CanonicalFactionIdentity* standardFactionFor(container::StringView requestedFaction)
{
    static constexpr CanonicalFactionIdentity kAmerica{
        .templateName = "FactionAmerica", .side = "America", .baseSide = "USA"};
    static constexpr CanonicalFactionIdentity kChina{
        .templateName = "FactionChina", .side = "China", .baseSide = "China"};
    static constexpr CanonicalFactionIdentity kGla{
        .templateName = "FactionGLA", .side = "GLA", .baseSide = "GLA"};

    const container::String key = lower(trim(requestedFaction));
    if (key == "usa" || key == "us" || key == "america" || key == "factionamerica") {
        return &kAmerica;
    }
    if (key == "china" || key == "cn" || key == "factionchina") return &kChina;
    if (key == "gla" || key == "factiongla") return &kGla;
    return nullptr;
}

const CanonicalFactionIdentity* standardSideFor(container::StringView requestedSide)
{
    return standardFactionFor(requestedSide);
}

uint32_t unsignedParam(const CommandLine& commandLine, const container::String& key,
                       uint32_t fallback = 0)
{
    if (!commandLine.hasParam(key)) return fallback;
    const container::String value = commandLine.getParam(key);
    if (value.empty() || value.front() == '-') return fallback;
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() ? parsed : fallback;
}

uint16_t uint16Param(const CommandLine& commandLine, const container::String& key,
                     uint16_t fallback = 0)
{
    const uint32_t parsed = unsignedParam(commandLine, key, fallback);
    return parsed <= std::numeric_limits<uint16_t>::max()
        ? static_cast<uint16_t>(parsed)
        : fallback;
}

} // namespace

GameMode GameStartInfoBuilder::parseMode(const container::String& value, GameMode fallback)
{
    const container::String mode = lower(value);
    if (mode == "singleplayer" || mode == "campaign") return GameMode::SinglePlayer;
    if (mode == "skirmish") return GameMode::Skirmish;
    if (mode == "challenge") return GameMode::Challenge;
    if (mode == "replay") return GameMode::Replay;
    return fallback;
}

void GameStartInfoBuilder::applyLocalSide(GameStartInfo& info, const container::String& requestedSide)
{
    if (const CanonicalFactionIdentity* identity = standardSideFor(requestedSide)) {
        info.localPlayerSide = container::String{identity->side};
        info.localPlayerBaseSide = container::String{identity->baseSide};
        return;
    }

    // An unknown explicit side is meaningful mod input. Preserve it as a
    // constraint and let the frozen ruleset either resolve it or report a
    // clear missing-faction error; never silently turn it into America.
    info.localPlayerSide = trim(requestedSide);
    info.localPlayerBaseSide.clear();
}

void GameStartInfoBuilder::applyCampaignFaction(GameStartInfo& info,
                                                container::StringView requestedFaction)
{
    info.localPlayerTemplateName.clear();
    info.localPlayerSide.clear();
    info.localPlayerBaseSide.clear();

    if (const CanonicalFactionIdentity* identity = standardFactionFor(requestedFaction)) {
        info.localPlayerTemplateName = container::String{identity->templateName};
        info.localPlayerSide = container::String{identity->side};
        info.localPlayerBaseSide = container::String{identity->baseSide};
        return;
    }

    const container::String authored = trim(requestedFaction);
    if (authored.empty()) {
        // Campaign identity is authored data. Leaving it unresolved lets the
        // launch boundary report the missing catalog field; manufacturing USA
        // here would also select the wrong ControlBar/loading presentation.
        return;
    }

    // Campaign.ini's PlayerFaction field is an authored PlayerTemplate name
    // (for example FactionAmericaAirForceGeneral). Preserve every non-stock
    // name exactly: custom templates do not have to use a Faction prefix,
    // and guessing a Side here could conflict with the frozen template.
    info.localPlayerTemplateName = authored;
}

GameStartInfo GameStartInfoBuilder::makeDirectStart(const CommandLine& commandLine)
{
    GameStartInfo info;
    info.mode = parseMode(commandLine.getParam("mode"), GameMode::Skirmish);
    info.difficulty = commandLine.getIntParam("difficulty", DIFFICULTY_NORMAL);
    info.gameSpeedFPS = commandLine.getIntParam("fps", DEFAULT_GAME_SPEED_FPS);
    info.startingMoney = commandLine.getIntParam("cash", DEFAULT_STARTING_CASH);
    info.seed = commandLine.getIntParam("seed", 1);
    info.mapName = game::canonicalMapSourcePath(commandLine.getParam("map"));
    info.mapCRC = unsignedParam(commandLine, "map-crc");
    info.mapSize = unsignedParam(commandLine, "map-size");
    info.rulesCRC = unsignedParam(commandLine, "rules-crc");
    info.localPlayerTemplateName = commandLine.getParam("template");
    info.saveFileName = commandLine.getParam("save");
    info.replayFileName = commandLine.getParam("replay");

    // An explicit template is authoritative. Do not silently add the old
    // America side default and turn a valid `--template=FactionChina` into a
    // contradictory request; side/base-side constrain it only when the user
    // actually supplied those flags.
    if (commandLine.hasParam("side")) {
        applyLocalSide(info, commandLine.getParam("side"));
    } else if (info.localPlayerTemplateName.empty() &&
               info.mode == GameMode::Skirmish) {
        applyLocalSide(info, "America");
    }
    if (commandLine.hasParam("base-side")) {
        info.localPlayerBaseSide = commandLine.getParam("base-side");
    }
    if (!info.replayFileName.empty()) {
        info.mode = GameMode::Replay;
    }

    const int requestedLocalSlot = commandLine.getIntParam("local-slot", 0);
    // This debug convenience builder cannot report a structured parse error,
    // so normalize an invalid CLI value to the documented default rather than
    // creating a Human in slot zero and claiming a different local slot.
    info.localPlayerSlot = requestedLocalSlot >= 0 && requestedLocalSlot < MAX_SLOTS
        ? requestedLocalSlot
        : 0;
    GameSlot& localSlot = info.playerSlot();
    localSlot.state = SLOT_HUMAN;
    localSlot.name = commandLine.getParam("player", "DirectStart");
    localSlot.teamNumber = commandLine.getIntParam("team", 1);

    const container::String serverHost = commandLine.getParam("server");
    if (!serverHost.empty()) {
        info.network.enabled = true;
        info.network.serverHost = serverHost;
        info.network.serverPort = uint16Param(commandLine, "server-port");
        info.network.sessionId = commandLine.getParam("session-id");
        info.network.joinToken = commandLine.getParam("join-token");
        info.network.protocolVersion = uint16Param(commandLine, "network-version", 1);
        info.network.frameSendRate = unsignedParam(commandLine, "frame-send-rate", 3);
    }
    return info;
}

} // namespace engine
