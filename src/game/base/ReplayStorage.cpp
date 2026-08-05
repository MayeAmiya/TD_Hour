#include "core/container/hash_containers.h"
#include "game/base/ReplayStorage.h"
#include "game/base/ReplayFileCodec.h"
#include "VFS.h"
#include "core/constants/Paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <utility>

namespace game {

namespace {

container::String lower(container::String value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isSafeReplayFileName(container::StringView fileName)
{
    if (fileName.empty() || fileName.find("..") != container::StringView::npos ||
        fileName.find_first_of("/\\") != container::StringView::npos) {
        return false;
    }
    const std::filesystem::path path{fileName};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        path.has_parent_path() || path.filename().string() != fileName ||
        lower(path.extension().string()) != EXT_REPLAY) {
        return false;
    }
    return true;
}

std::optional<container::String> userReplayPath(container::StringView fileName)
{
    if (!isSafeReplayFileName(fileName)) return std::nullopt;
    return container::String(USER_REPLAY_ROOT) + "/" + container::String(fileName);
}

} // namespace

ReplayStorage& ReplayStorage::instance()
{
    static ReplayStorage storage;
    return storage;
}

container::Vector<ReplayEntry> ReplayStorage::listReplays() const
{
    container::Vector<ReplayEntry> result;
    container::HashSet<container::String> seen;

    const container::String replayPattern =
        container::String(USER_REPLAY_ROOT) + "/*";
    for (const auto& path : io::VFS::instance().getFileList(replayPattern)) {
        if (lower(std::filesystem::path(path).extension().string()) != EXT_REPLAY) continue;

        const auto fileName = std::filesystem::path(path).filename().string();
        if (!seen.insert(lower(fileName)).second) continue;

        ReplayEntry replay;
        replay.fileName = fileName;
        replay.displayName = std::filesystem::path(fileName).stem().string();
        result.push_back(std::move(replay));
    }

    std::sort(result.begin(), result.end(), [](const ReplayEntry& a, const ReplayEntry& b) {
        return lower(a.displayName) < lower(b.displayName);
    });
    return result;
}

container::Vector<uint8_t> ReplayStorage::readReplay(const container::String& fileName) const
{
    const auto path = userReplayPath(fileName);
    if (!path) return {};
    container::Vector<uint8_t> buffer;
    io::VFS::instance().readToBuffer(*path, buffer);
    return buffer;
}

bool ReplayStorage::writeReplay(const container::String& fileName, const container::Vector<uint8_t>& content) const
{
    const auto path = userReplayPath(fileName);
    return path && io::VFS::instance().writeBuffer(*path, content);
}

ReplayCommandReadResult ReplayStorage::readReplayCommandStream(const container::String& fileName) const
{
    ReplayCommandReadResult result;
    auto buffer = readReplay(fileName);
    if (buffer.empty()) {
        result.error = "replay file is empty or unreadable";
        return result;
    }

    auto decoded = ReplayFileCodec::decode(buffer);
    if (!decoded.ok) {
        result.error = decoded.error;
        return result;
    }

    result.ok = true;
    result.startInfo = std::move(decoded.replay.startInfo);
    result.resolvedMatchSetup = std::move(decoded.replay.resolvedMatchSetup);
    result.commands = std::move(decoded.replay.commands);
    return result;
}

container::Vector<engine::GameCommand> ReplayStorage::readReplayCommands(const container::String& fileName) const
{
    auto result = readReplayCommandStream(fileName);
    return result.ok ? std::move(result.commands) : container::Vector<engine::GameCommand>{};
}

bool ReplayStorage::writeReplayData(const container::String& fileName,
                                    const engine::ResolvedMatchSetup& resolvedMatchSetup,
                                    const container::Vector<engine::GameCommand>& commands) const
{
    const auto encoded = ReplayFileCodec::encode(resolvedMatchSetup, commands);
    return !encoded.empty() && writeReplay(fileName, encoded);
}

bool ReplayStorage::deleteReplay(const container::String& fileName) const
{
    const auto path = userReplayPath(fileName);
    return path && io::VFS::instance().remove(*path);
}

} // namespace game
