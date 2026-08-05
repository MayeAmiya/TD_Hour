#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/base/GameLaunchDescriptor.h"

#include "game/base/GameStartInfoBuilder.h"
#include "game/base/MapContentIdentity.h"
#include "VFS.h"
#include "core/constants/Paths.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
namespace engine {

namespace {

using Section = container::TreeMap<container::String, container::String>;
using Document = container::TreeMap<container::String, Section>;

constexpr auto trim = container::trimAsciiCopy;

container::String lower(container::String value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

Document parseIni(const container::String& content)
{
    Document document;
    container::String section;
    size_t lineStart = 0;
    while (lineStart <= content.size()) {
        const size_t lineEnd = content.find('\n', lineStart);
        const container::String line = trim(content.substr(lineStart,
            lineEnd == container::String::npos ? container::String::npos : lineEnd - lineStart));
        if (!line.empty() && line[0] != ';' && line[0] != '#') {
            if (line.front() == '[' && line.back() == ']') {
                section = lower(trim(container::StringView{line}.substr(1, line.size() - 2)));
            } else if (!section.empty()) {
                const size_t equals = line.find('=');
                if (equals != container::String::npos) {
                    document[section][lower(trim(container::StringView{line}.substr(0, equals)))] =
                        trim(container::StringView{line}.substr(equals + 1));
                }
            }
        }
        if (lineEnd == container::String::npos) break;
        lineStart = lineEnd + 1;
    }
    return document;
}

const container::String* findValue(const Document& document, container::StringView section, container::StringView key)
{
    const auto sectionIt = document.find(lower(container::String{section}));
    if (sectionIt == document.end()) return nullptr;
    const auto valueIt = sectionIt->second.find(lower(container::String{key}));
    return valueIt == sectionIt->second.end() ? nullptr : &valueIt->second;
}

bool readInt(const Document& document, container::StringView section, container::StringView key, int& value,
             container::String& error, bool required = false)
{
    const auto* text = findValue(document, section, key);
    if (!text) {
        if (required) error = "missing " + container::String{section} + "." + container::String{key};
        return !required;
    }
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) {
        error = "invalid integer " + container::String{section} + "." + container::String{key};
        return false;
    }
    return true;
}

bool readUint32(const Document& document, container::StringView section, container::StringView key, uint32_t& value,
                container::String& error, bool required = false)
{
    const auto* text = findValue(document, section, key);
    if (!text) {
        if (required) error = "missing " + container::String{section} + "." + container::String{key};
        return !required;
    }
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) {
        error = "invalid unsigned integer " + container::String{section} + "." + container::String{key};
        return false;
    }
    return true;
}

bool readBool(const Document& document, container::StringView section, container::StringView key, bool& value,
              container::String& error)
{
    const auto* text = findValue(document, section, key);
    if (!text) return true;
    const container::String normalized = lower(*text);
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        value = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        value = false;
        return true;
    }
    error = "invalid boolean " + container::String{section} + "." + container::String{key};
    return false;
}

bool readString(const Document& document, container::StringView section, container::StringView key, container::String& value,
                container::String& error, bool required = false)
{
    const auto* text = findValue(document, section, key);
    if (!text) {
        if (required) error = "missing " + container::String{section} + "." + container::String{key};
        return !required;
    }
    value = *text;
    if (required && value.empty()) {
        error = "empty " + container::String{section} + "." + container::String{key};
        return false;
    }
    return true;
}

bool parseSlotState(container::String value, SlotState& state)
{
    value = lower(std::move(value));
    if (value == "open" || value == "0") state = SLOT_OPEN;
    else if (value == "human" || value == "player" || value == "1") state = SLOT_HUMAN;
    else if (value == "ai" || value == "2") state = SLOT_AI;
    else if (value == "easyai" || value == "3") state = SLOT_EASY_AI;
    else if (value == "normalai" || value == "4") state = SLOT_NORMAL_AI;
    else if (value == "hardai" || value == "5") state = SLOT_HARD_AI;
    else if (value == "closed" || value == "6") state = SLOT_CLOSED;
    else return false;
    return true;
}

bool readSequence(const Document& document, GameSequenceIdentity& sequence,
                  container::String& error)
{
    const auto section = document.find("sequence");
    if (section == document.end()) return true;

    const container::String* type = findValue(document, "sequence", "type");
    if (!type) type = findValue(document, "sequence", "kind");
    if (!type || type->empty()) {
        error = "missing sequence.type";
        return false;
    }

    const container::String normalized = lower(*type);
    if (normalized == "none") sequence.type = GameSequenceType::None;
    else if (normalized == "campaign") sequence.type = GameSequenceType::Campaign;
    else if (normalized == "challenge") sequence.type = GameSequenceType::Challenge;
    else {
        error = "invalid sequence.type";
        return false;
    }

    if (!readString(document, "sequence", "campaignname", sequence.campaignName, error) ||
        !readString(document, "sequence", "missionname", sequence.missionName, error) ||
        !readString(document, "sequence", "challengegeneral", sequence.challengeGeneral, error)) {
        return false;
    }

    if (sequence.type == GameSequenceType::None) {
        if (!sequence.campaignName.empty() || !sequence.missionName.empty() ||
            !sequence.challengeGeneral.empty()) {
            error = "sequence.type None cannot carry sequence identity fields";
            return false;
        }
        return true;
    }
    if (sequence.campaignName.empty() || sequence.missionName.empty()) {
        error = "sequence requires campaignName and missionName";
        return false;
    }
    if (sequence.type == GameSequenceType::Challenge && sequence.challengeGeneral.empty()) {
        error = "challenge sequence requires challengeGeneral";
        return false;
    }
    return true;
}

bool parseGameStartInfo(const Document& document, GameStartInfo& info,
                        container::String& error)
{
    GameStartInfo parsed;
    container::String mode;
    int descriptorVersion = 1;
    const bool hasDescriptorVersion =
        findValue(document, "game", "descriptorversion") != nullptr;
    const bool hasNetworkSection = document.find("network") != document.end();
    bool networkEnabled = hasNetworkSection;
    int serverPort = 0;
    int protocolVersion = parsed.network.protocolVersion;
    if (!(hasDescriptorVersion
              ? readInt(document, "game", "descriptorversion", descriptorVersion,
                        error, true)
              : readInt(document, "game", "version", descriptorVersion, error)) ||
        !readString(document, "game", "mode", mode, error, true) ||
        !readString(document, "game", "map", parsed.mapName, error, true) ||
        !readUint32(document, "game", "mapcrc", parsed.mapCRC, error) ||
        !readUint32(document, "game", "mapsize", parsed.mapSize, error) ||
        !readUint32(document, "game", "rulescrc", parsed.rulesCRC, error) ||
        !readInt(document, "game", "difficulty", parsed.difficulty, error) ||
        !readInt(document, "game", "rankpoints", parsed.rankPoints, error) ||
        !readInt(document, "game", "gamespeedfps", parsed.gameSpeedFPS, error) ||
        !readInt(document, "game", "seed", parsed.seed, error, true) ||
        !readInt(document, "game", "startingmoney", parsed.startingMoney, error, true) ||
        !readBool(document, "game", "superweaponrestricted", parsed.superweaponRestricted, error) ||
        !readBool(document, "game", "oldfactionsonly", parsed.oldFactionsOnly, error) ||
        !readInt(document, "localplayer", "slot", parsed.localPlayerSlot, error, true) ||
        !readString(document, "localplayer", "template", parsed.localPlayerTemplateName, error) ||
        !readString(document, "localplayer", "side", parsed.localPlayerSide, error, true) ||
        !readString(document, "localplayer", "baseside", parsed.localPlayerBaseSide, error, true) ||
        !readSequence(document, parsed.sequence, error) ||
        !readBool(document, "network", "enabled", networkEnabled, error)) {
        return false;
    }

    parsed.mode = GameStartInfoBuilder::parseMode(mode, GameMode::Invalid);
    parsed.mapName = game::canonicalMapSourcePath(parsed.mapName);
    if (parsed.mapName.empty()) {
        error = "session descriptor contains an invalid map source path";
        return false;
    }
    parsed.network.enabled = networkEnabled;
    if (descriptorVersion != 1) {
        error = "unsupported game descriptor version";
        return false;
    }
    if (parsed.localPlayerSlot < 0 || parsed.localPlayerSlot >= MAX_SLOTS) {
        error = "session descriptor contains an invalid local slot";
        return false;
    }
    if (parsed.mode == GameMode::Invalid || parsed.mode == GameMode::Replay) {
        error = "session descriptor mode must be a playable game mode";
        return false;
    }
    if (parsed.mode == GameMode::SinglePlayer) {
        if (parsed.sequence.type != GameSequenceType::Campaign ||
            parsed.localPlayerTemplateName.empty()) {
            error =
                "single-player descriptor requires a campaign sequence and local player template";
            return false;
        }
    } else if (parsed.mode == GameMode::Challenge) {
        if (parsed.sequence.type != GameSequenceType::Challenge ||
            parsed.localPlayerTemplateName.empty()) {
            error =
                "challenge descriptor requires a challenge sequence and local player template";
            return false;
        }
    } else if (parsed.sequence.type != GameSequenceType::None) {
        error = "non-campaign descriptor must not carry campaign sequence identity";
        return false;
    }
    if (networkEnabled &&
        (!readString(document, "network", "serverhost", parsed.network.serverHost,
                     error, true) ||
         !readInt(document, "network", "serverport", serverPort, error, true) ||
         !readString(document, "network", "sessionid", parsed.network.sessionId,
                     error, true) ||
         !readString(document, "network", "jointoken", parsed.network.joinToken,
                     error, true) ||
         !readInt(document, "network", "protocolversion", protocolVersion, error) ||
         !readUint32(document, "network", "framesendrate",
                     parsed.network.frameSendRate, error, true))) {
        return false;
    }
    if (networkEnabled &&
        (serverPort <= 0 || serverPort > std::numeric_limits<uint16_t>::max() ||
         protocolVersion <= 0 ||
         protocolVersion > std::numeric_limits<uint16_t>::max() ||
         parsed.network.frameSendRate == 0)) {
        error =
            "session descriptor contains an invalid network port, protocolVersion, or frameSendRate";
        return false;
    }
    if (networkEnabled) {
        parsed.network.serverPort = static_cast<uint16_t>(serverPort);
        parsed.network.protocolVersion = static_cast<uint16_t>(protocolVersion);
    }
    for (int index = 0; index < MAX_SLOTS; ++index) {
        const container::String section = "slot" + std::to_string(index);
        auto& slot = parsed.slots[index];
        if (const auto* state = findValue(document, section, "state")) {
            if (!parseSlotState(*state, slot.state)) {
                error = "invalid " + section + ".state";
                return false;
            }
        }
        if (!readInt(document, section, "color", slot.color, error) ||
            !readInt(document, section, "startpos", slot.startPos, error) ||
            !readInt(document, section, "playertemplate", slot.playerTemplate, error) ||
            !readInt(document, section, "team", slot.teamNumber, error) ||
            !readString(document, section, "name", slot.name, error)) {
            return false;
        }
    }

    parsed.slots[parsed.localPlayerSlot].state = SLOT_HUMAN;
    info = std::move(parsed);
    return true;
}

bool normalizeRoot(const container::String& authored, container::StringView key,
                   bool mustExist, container::String& normalized,
                   container::String& error)
{
    namespace fs = std::filesystem;
    if (authored.empty()) {
        error = "empty content." + container::String{key};
        return false;
    }
    fs::path path{authored};
    if (!path.is_absolute()) {
        error = "content." + container::String{key} + " must be an absolute path";
        return false;
    }
    path = path.lexically_normal();
    std::error_code ec;
    if (mustExist && !fs::is_directory(path, ec)) {
        error = "content." + container::String{key} + " is not an existing directory";
        return false;
    }
    ec.clear();
    const fs::path canonical = fs::weakly_canonical(path, ec);
    normalized = (ec ? path : canonical).string();
    return true;
}

bool normalizeModRoot(const container::String& authored,
                      container::String& normalized, container::String& error)
{
    namespace fs = std::filesystem;
    if (authored.empty()) {
        normalized.clear();
        return true;
    }
    fs::path path{authored};
    if (!path.is_absolute()) {
        error = "content.modRoot must be an absolute path";
        return false;
    }
    path = path.lexically_normal();
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    ec.clear();
    const bool regularFile = fs::is_regular_file(path, ec);
    if (!directory && !regularFile) {
        error = "content.modRoot is not an existing directory or BIG file";
        return false;
    }
    if (regularFile && lower(path.extension().string()) != ".big") {
        error = "content.modRoot file must have a .big extension";
        return false;
    }
    ec.clear();
    const fs::path canonical = fs::weakly_canonical(path, ec);
    normalized = (ec ? path : canonical).string();
    return true;
}

bool parseContentContract(const Document& document, LaunchContentContract& contract,
                          container::String& error)
{
    container::String product;
    container::String contentRoot;
    container::String baseContentRoot;
    container::String modRoot;
    container::String userRoot;
    container::String locale;
    if (!readString(document, "content", "product", product, error, true) ||
        !readString(document, "content", "contentroot", contentRoot, error, true) ||
        !readString(document, "content", "basecontentroot", baseContentRoot, error, true) ||
        !readString(document, "content", "modroot", modRoot, error) ||
        !readString(document, "content", "userroot", userRoot, error, true) ||
        !readString(document, "content", "locale", locale, error, true)) {
        return false;
    }
    const container::String normalizedProduct = lower(trim(product));
    if (normalizedProduct != "zerohour" && normalizedProduct != "zero-hour" &&
        normalizedProduct != "zh") {
        error = "content.product must be ZeroHour";
        return false;
    }
    if (!std::all_of(locale.begin(), locale.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_';
        })) {
        error = "content.locale contains an invalid character";
        return false;
    }

    LaunchContentContract parsed;
    parsed.product = LaunchProduct::ZeroHour;
    if (!normalizeRoot(contentRoot, "contentRoot", true, parsed.contentRoot, error) ||
        !normalizeRoot(baseContentRoot, "baseContentRoot", true,
                       parsed.baseContentRoot, error) ||
        !normalizeModRoot(modRoot, parsed.modRoot, error) ||
        !normalizeRoot(userRoot, "userRoot", false, parsed.userRoot, error)) {
        return false;
    }
    parsed.locale = std::move(locale);
    contract = std::move(parsed);
    return true;
}

bool readBootstrapFile(container::StringView descriptorPath,
                       container::String& content, container::String& error)
{
    namespace fs = std::filesystem;
    const fs::path path{container::String{descriptorPath}};
    if (!path.is_absolute()) {
        error = "session descriptor path must be absolute";
        return false;
    }

    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    constexpr uintmax_t kMaximumDescriptorSize = 1024u * 1024u;
    if (ec || size == 0 || size > kMaximumDescriptorSize) {
        error = "session descriptor is empty, unreadable, or too large";
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "session descriptor is unreadable";
        return false;
    }
    content.resize(static_cast<size_t>(size));
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream || static_cast<size_t>(stream.gcount()) != content.size()) {
        content.clear();
        error = "session descriptor could not be read completely";
        return false;
    }
    return true;
}

} // namespace

bool GameLaunchDescriptor::loadFromVfs(container::StringView ticket, GameStartInfo& info, container::String* error)
{
    container::String localError;
    bool parsed = false;
    if (!isValidTicket(ticket)) {
        localError = "invalid session ticket";
    } else {
        const container::String content = io::VFS::instance().readAll(pathForTicket(ticket));
        if (content.empty()) {
            localError = "session descriptor is empty or unreadable";
        } else {
            const Document document = parseIni(content);
            parsed = parseGameStartInfo(document, info, localError);
            if (!parsed && localError.empty()) {
                localError = "session descriptor could not be parsed";
            }
        }
    }

    const bool success = parsed && localError.empty();
    if (!success && error) {
        *error = std::move(localError);
    }
    return success;
}

bool GameLaunchDescriptor::loadFromBootstrapFile(
    container::StringView descriptorPath, LauncherSessionDescriptor& descriptor,
    container::String* error)
{
    container::String localError;
    container::String content;
    LauncherSessionDescriptor parsed;
    bool parsedDescriptor = false;
    if (readBootstrapFile(descriptorPath, content, localError)) {
        const Document document = parseIni(content);
        if (parseContentContract(document, parsed.content, localError) &&
            parseGameStartInfo(document, parsed.startInfo, localError)) {
            descriptor = std::move(parsed);
            parsedDescriptor = true;
        } else if (localError.empty()) {
            localError = "launcher descriptor could not be parsed";
        }
    }

    const bool success = parsedDescriptor && localError.empty();
    if (!success && error) *error = std::move(localError);
    return success;
}

container::String GameLaunchDescriptor::pathForTicket(container::StringView ticket)
{
    return container::String{USER_SESSION_ROOT} + "/" + container::String{ticket} + ".ini";
}

bool GameLaunchDescriptor::isValidTicket(container::StringView ticket)
{
    if (ticket.empty() || ticket.size() > kMaximumTicketLength) return false;
    return std::all_of(ticket.begin(), ticket.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_';
    });
}

} // namespace engine
