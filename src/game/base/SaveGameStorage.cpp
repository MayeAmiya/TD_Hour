#include "core/container/hash_containers.h"
#include "game/base/SaveGameStorage.h"
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

bool hasExtension(const container::String& path, container::StringView extension)
{
    return lower(std::filesystem::path(path).extension().string()) == extension;
}

bool isSafeSaveFileName(container::StringView fileName)
{
    if (fileName.empty() || fileName.find("..") != container::StringView::npos ||
        fileName.find_first_of("/\\") != container::StringView::npos) {
        return false;
    }
    const std::filesystem::path path{fileName};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        path.has_parent_path() || path.filename().string() != fileName ||
        lower(path.extension().string()) != EXT_SAVE) {
        return false;
    }
    return true;
}

std::optional<container::String> userSavePath(container::StringView fileName)
{
    if (!isSafeSaveFileName(fileName)) return std::nullopt;
    return container::String(USER_SAVE_ROOT) + "/" + container::String(fileName);
}

} // namespace

SaveGameStorage& SaveGameStorage::instance()
{
    static SaveGameStorage storage;
    return storage;
}

container::Vector<SaveGameEntry> SaveGameStorage::listSaves() const
{
    container::Vector<SaveGameEntry> result;
    container::HashSet<container::String> seen;

    const container::String savePattern =
        container::String(USER_SAVE_ROOT) + "/*";
    for (const auto& path : io::VFS::instance().getFileList(savePattern)) {
        if (!hasExtension(path, EXT_SAVE)) continue;

        const auto fileName = std::filesystem::path(path).filename().string();
        if (!seen.insert(lower(fileName)).second) continue;

        SaveGameEntry save;
        save.fileName = fileName;
        save.displayName = std::filesystem::path(fileName).stem().string();
        result.push_back(std::move(save));
    }

    std::sort(result.begin(), result.end(), [](const SaveGameEntry& a, const SaveGameEntry& b) {
        return lower(a.displayName) < lower(b.displayName);
    });
    return result;
}

container::Vector<uint8_t> SaveGameStorage::readSave(const container::String& fileName) const
{
    const auto path = userSavePath(fileName);
    if (!path) return {};
    container::Vector<uint8_t> buffer;
    io::VFS::instance().readToBuffer(*path, buffer);
    return buffer;
}

bool SaveGameStorage::writeSave(const container::String& fileName, const container::Vector<uint8_t>& content) const
{
    const auto path = userSavePath(fileName);
    return path && io::VFS::instance().writeBuffer(*path, content);
}

bool SaveGameStorage::deleteSave(const container::String& fileName) const
{
    const auto path = userSavePath(fileName);
    return path && io::VFS::instance().remove(*path);
}

} // namespace game
