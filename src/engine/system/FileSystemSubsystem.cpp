#include "core/container/container_types.h"
#include "FileSystemSubsystem.h"
#include "debug/debug.h"
#include "VFS.h"
#include "LocaleResourceLocator.h"
#include "core/constants/Paths.h"
#include "core/platform/CommandLine.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace {

container::String toLower(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

container::Vector<container::String> collectBigFiles(const container::String& dataPath) {
    container::Vector<container::String> bigFiles;
    std::error_code error;
    const fs::path root{dataPath};
    fs::recursive_directory_iterator current{
        root, fs::directory_options::skip_permission_denied, error};
    const fs::recursive_directory_iterator end;
    while (!error && current != end) {
        const fs::directory_entry& entry = *current;
        if (entry.is_regular_file(error) && !error &&
            toLower(entry.path().extension().string()) == EXT_BIG.data()) {
            fs::path relative = fs::relative(entry.path(), root, error);
            if (error) break;
            container::String relativeName = toLower(relative.generic_string());
            // Shipped English/Chinese/Korean ZH installations can contain a
            // duplicate Data/INI/INIZH.big. The original ZH filesystem skips
            // this nested copy to keep catalog/CRC selection stable.
            if (relativeName == "data/ini/inizh.big") {
                current.increment(error);
                continue;
            }
            bigFiles.push_back(entry.path().string());
        }
        current.increment(error);
    }
    if (error) {
        TD_LOG_WARN("[FileSystem] Failed to scan content directory '{}': {}",
                    dataPath, error.message());
    }

    std::sort(bigFiles.begin(), bigFiles.end(),
        [](const container::String& a, const container::String& b) {
            return toLower(a) < toLower(b);
        });
    return bigFiles;
}

bool samePath(const container::String& a, const container::String& b) {
    if (a.empty() || b.empty()) return false;

    std::error_code ecA;
    std::error_code ecB;
    fs::path ca = fs::weakly_canonical(fs::path(a), ecA);
    fs::path cb = fs::weakly_canonical(fs::path(b), ecB);
    if (!ecA && !ecB) {
        return toLower(ca.string()) == toLower(cb.string());
    }
    return toLower(a) == toLower(b);
}

void mountBigDirectory(io::VFS& vfs, const container::String& dataPath, int& priority,
                       container::Vector<container::String>& mountedBigFiles, const char* label) {
    if (dataPath.empty()) return;

    const container::Vector<container::String> bigFiles = collectBigFiles(dataPath);
    // Product precedence is represented by separate mount groups: the caller
    // mounts Generals first and active ZH second. Within either product root,
    // RefCode uses one case-insensitive lexical ordering for every BIG. This
    // naturally preserves !!!! > !!! > !! > ! without inventing a special
    // *ZH class that would reorder ordinary third-party archives.
    const int groupBasePriority = priority;
    const int groupSize = static_cast<int>(bigFiles.size());
    for (int index = 0; index < groupSize; ++index) {
        const container::String& bigPath = bigFiles[static_cast<size_t>(index)];
        const int archivePriority = groupBasePriority + groupSize - index - 1;
        if (vfs.mountArchive(bigPath, archivePriority)) {
            mountedBigFiles.push_back(bigPath);
        } else {
            TD_LOG_WARN("[FileSystem] Failed to mount {} {}", label, fs::path(bigPath).filename().string());
        }
    }
    priority += groupSize;
}

void mountModPath(io::VFS& vfs, const container::String& modPath, int& priority,
                  container::Vector<container::String>& mountedBigFiles) {
    if (modPath.empty()) return;
    const fs::path path{modPath};
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    ec.clear();
    if (directory) {
        mountBigDirectory(vfs, modPath, priority, mountedBigFiles, "Mod");
        return;
    }
    if (fs::is_regular_file(path, ec) &&
        toLower(path.extension().string()) == EXT_BIG.data()) {
        if (vfs.mountArchive(modPath, priority++)) {
            mountedBigFiles.push_back(modPath);
        } else {
            TD_LOG_WARN("[FileSystem] Failed to mount Mod BIG {}",
                        path.filename().string());
        }
        return;
    }
    TD_LOG_WARN("[FileSystem] Configured Mod path is not a directory or BIG: {}",
                modPath);
}

fs::path configuredOrCurrentPath(const container::String& path) {
    if (!path.empty()) return fs::path(path);
    return fs::current_path();
}

fs::path configuredOrUserPath(const fs::path& userPath, const container::String& configuredPath, container::StringView defaultDirectory) {
    if (!configuredPath.empty()) return fs::path(configuredPath);
    return userPath / container::String(defaultDirectory);
}

void mountLocalIfDirectory(io::VFS& vfs, const fs::path& path, container::StringView virtualRoot, int& priority, const char* label) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        fs::create_directories(path, ec);
    }
    if (!fs::is_directory(path, ec)) {
        TD_LOG_WARN("[FileSystem] Configured {} path does not exist: {}", label, path.string());
        return;
    }

    if (virtualRoot.empty()) {
        vfs.mountLocal(path.string(), priority++);
    } else {
        vfs.mountLocal(path.string(), virtualRoot, priority++);
    }
    TD_LOG_INFO("[FileSystem] Mounted {} path '{}' as '{}'", label, path.string(), virtualRoot);
}

void mountExistingLocal(io::VFS& vfs, const fs::path& path, int& priority,
                        const char* label) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        TD_LOG_WARN("[FileSystem] Configured {} path does not exist: {}", label,
                    path.string());
        return;
    }
    vfs.mountLocal(path.string(), priority++);
    TD_LOG_INFO("[FileSystem] Mounted {} path '{}'", label, path.string());
}

void mountExistingLocal(io::VFS& vfs, const fs::path& path,
                        container::StringView virtualRoot, int& priority,
                        const char* label) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return;

    vfs.mountLocal(path.string(), virtualRoot, priority++);
    TD_LOG_INFO("[FileSystem] Mounted {} path '{}' as '{}'", label,
                path.string(), virtualRoot);
}

void mountUserDataPaths(io::VFS& vfs, const fs::path& userRoot,
                        const fs::path& saveRoot, const fs::path& replayRoot,
                        int& priority) {
    // RefCode never inserts the complete user-data directory into
    // TheLocalFileSystem. User files are addressed through explicit paths:
    // UserData/INI/MappedImages, Maps, Save, Replays and three explicit loose
    // art fallback directories. Mounting userRoot at the VFS root would
    // incorrectly let an arbitrary UserData/Data/... file replace active ZH
    // content. The art namespaces below are consulted only after installed
    // locale/common candidates miss (LocaleResourceLocator).
    mountExistingLocal(vfs, userRoot / "INI" / "MappedImages",
                       "ini/mappedimages", priority,
                       "user mapped images");
    mountExistingLocal(vfs, userRoot / "W3D", USER_W3D_VFS_ROOT,
                       priority, "user W3D fallback");
    mountExistingLocal(vfs, userRoot / "Textures",
                       USER_TEXTURE_VFS_ROOT, priority,
                       "user texture fallback");
    mountExistingLocal(vfs, userRoot / "MapPreviews",
                       USER_MAP_PREVIEW_VFS_ROOT, priority,
                       "user map-preview fallback");
    mountLocalIfDirectory(vfs, saveRoot, USER_SAVE_ROOT, priority, "save");
    mountLocalIfDirectory(vfs, replayRoot, USER_REPLAY_ROOT, priority,
                          "replay");
    mountLocalIfDirectory(vfs, userRoot / container::String(USER_MAP_ROOT),
                          USER_MAP_VFS_ROOT, priority, "user map");
}

void loadContractOptions(config::GlobalData& globalData,
                         const engine::LaunchContentContract& contract) {
    const fs::path userOptions =
        fs::path(contract.userRoot) / container::String(GAME_OPTIONS_INI);
    const fs::path contentOptions =
        fs::path(contract.contentRoot) / container::String(GAME_OPTIONS_INI);
    std::error_code ec;
    if (fs::is_regular_file(userOptions, ec)) {
        globalData.loadFromIni(userOptions.string());
        return;
    }
    ec.clear();
    if (fs::is_regular_file(contentOptions, ec)) {
        globalData.loadFromIni(contentOptions.string());
    }
}

void mountLocalMapPaths(io::VFS& vfs, const config::GlobalData& globalData, int& priority) {
    for (const auto& mapPath : globalData.getLocalMapPaths()) {
        if (mapPath.sourcePath.empty() || mapPath.vfsRoot.empty()) continue;

        std::error_code ec;
        if (!fs::is_directory(mapPath.sourcePath, ec)) {
            TD_LOG_WARN("[FileSystem] Configured map path does not exist: {}", mapPath.sourcePath);
            continue;
        }

        vfs.mountLocal(mapPath.sourcePath, mapPath.vfsRoot, priority++);
        TD_LOG_INFO("[FileSystem] Mounted map path '{}' as '{}'", mapPath.sourcePath, mapPath.vfsRoot);
    }
}

container::String configuredLegacyLocale(
    const config::GlobalData& globalData) {
    if (globalData.getLocaleDataPath().empty()) return {};
    const container::String directory =
        fs::path(globalData.getLocaleDataPath()).filename().string();
    return directory;
}

container::String resolveLegacyLocale(
    const config::GlobalData& globalData, const io::VFS& vfs) {
    if (container::String configured = configuredLegacyLocale(globalData);
        !configured.empty()) {
        return configured;
    }
    // A direct developer launch has no launcher-authored locale. Infer only
    // from exact mounted CSF paths, never from cwd or a basename scan. This
    // makes a Chinese-only ZH installation select the resources it actually
    // owns while retaining English as the deterministic multi-language
    // fallback.
    constexpr container::Array<container::StringView, 9> languages{{
        "english", "chinese", "german", "french", "spanish", "italian",
        "japanese", "korean", "ukenglish",
    }};
    for (const container::StringView language : languages) {
        const container::String csf =
            "data/" + container::String{language} + "/generals.csf";
        if (vfs.exists(csf)) return container::String{language};
    }
    return "english";
}

} // namespace

FileSystemSubsystem::FileSystemSubsystem() {
    setName("FileSystem");
}

FileSystemSubsystem::FileSystemSubsystem(
    const engine::LaunchContentContract& contentContract)
    : m_contentContract(contentContract), m_hasContentContract(true) {
    setName("FileSystem");
}

FileSystemSubsystem::~FileSystemSubsystem() {
    shutdown();
}

void FileSystemSubsystem::init() {
    TD_LOG_INFO("[FileSystem] Initializing...");

    config::GlobalData& globalData = *config::TheWritableGlobalData;

    auto& vfs = io::VFS::instance();

    if (m_hasContentContract) {
        // Formal launcher startup is intentionally independent of cwd.  The
        // absolute bootstrap descriptor is read before VFS construction and
        // all legacy path fields in GameOptions.ini are ignored here.
        loadContractOptions(globalData, m_contentContract);

        int priority = 0;
        if (!samePath(m_contentContract.baseContentRoot,
                      m_contentContract.contentRoot)) {
            mountBigDirectory(vfs, m_contentContract.baseContentRoot, priority,
                              m_mountedBigFiles, "base");
        }
        mountBigDirectory(vfs, m_contentContract.contentRoot, priority,
                          m_mountedBigFiles, "ZH");
        mountExistingLocal(vfs, fs::path(m_contentContract.contentRoot), priority,
                           "ZH loose content");
        // Mod is the final product-content layer. It must win over both ZH
        // archives and loose ZH files, while the explicit user namespaces
        // below remain outside product-content election.
        mountModPath(vfs, m_contentContract.modRoot, priority,
                     m_mountedBigFiles);

        const fs::path userRoot{m_contentContract.userRoot};
        mountUserDataPaths(
            vfs, userRoot, userRoot / container::String(USER_SAVE_ROOT),
            userRoot / container::String(USER_REPLAY_ROOT), priority);

        vfs.rebuildIndex();
        const auto locator = io::LocaleResourceLocator::build(
            vfs, m_contentContract.locale);
        io::publishLocaleResourceLocator(locator);
        TD_LOG_INFO(
            "[FileSystem] Initialized launcher ZeroHour content: locale='{}' resolved='{}', {} BIG archives mounted, {} winning VFS files indexed",
            m_contentContract.locale, locator->localeDirectory(),
            m_mountedBigFiles.size(), vfs.indexedFileCount());
        return;
    }

    // Direct/developer startup reads three explicit product layers from
    // GameOptions.ini. Formal launcher sessions use LaunchContentContract and
    // never enter this cwd-dependent branch.
    globalData.loadFromIni(GAME_OPTIONS_INI.data());
    const container::String& generalsDataPath =
        globalData.getGeneralsDataPath();
    const container::String& zeroHourDataPath =
        globalData.getZeroHourDataPath();
    if (generalsDataPath.empty() || zeroHourDataPath.empty()) {
        TD_LOG_ERROR(
            "[FileSystem] GeneralsDataPath and ZeroHourDataPath are required in GameOptions.ini");
        return;
    }
    std::error_code generalsError;
    std::error_code zeroHourError;
    if (!fs::is_directory(generalsDataPath, generalsError) ||
        !fs::is_directory(zeroHourDataPath, zeroHourError)) {
        TD_LOG_ERROR(
            "[FileSystem] Required content directory is missing: GeneralsDataPath='{}' ZeroHourDataPath='{}'",
            generalsDataPath, zeroHourDataPath);
        return;
    }
    if (samePath(generalsDataPath, zeroHourDataPath)) {
        TD_LOG_ERROR(
            "[FileSystem] GeneralsDataPath and ZeroHourDataPath must identify separate product roots");
        return;
    }

    int priority = 0;
    mountBigDirectory(vfs, generalsDataPath, priority, m_mountedBigFiles,
                      "Generals");
    mountBigDirectory(vfs, zeroHourDataPath, priority, m_mountedBigFiles,
                      "ZeroHour");

    // Loose Zero Hour files are part of the Zero Hour layer and therefore
    // still precede Mod in the winner election.
    mountExistingLocal(vfs, fs::path(zeroHourDataPath), priority,
                       "ZeroHour loose content");

    // Loose locale data is also part of the active Zero Hour layer.
    if (container::String locale = configuredLegacyLocale(globalData);
        !locale.empty()) {
        vfs.mountLocal(
            globalData.getLocaleDataPath(), "Data/" + locale, priority++);
    }

    container::String modDataPath = globalData.getModDataPath();
    const container::String modArgument =
        engine::CommandLine::instance().getParam("mod");
    if (!modArgument.empty() && modArgument != "true") {
        fs::path modPath{modArgument};
        if (!modPath.is_absolute()) {
            modPath = configuredOrCurrentPath(globalData.getUserDataPath()) /
                modPath;
        }
        modDataPath = modPath.lexically_normal().string();
    }
    mountModPath(vfs, modDataPath, priority, m_mountedBigFiles);

    const fs::path userDataPath = configuredOrCurrentPath(globalData.getUserDataPath());
    mountUserDataPaths(
        vfs, userDataPath,
        configuredOrUserPath(userDataPath, globalData.getSaveDataPath(),
                             USER_SAVE_ROOT),
        configuredOrUserPath(userDataPath, globalData.getReplayDataPath(),
                             USER_REPLAY_ROOT),
        priority);
    mountLocalMapPaths(vfs, globalData, priority);

    vfs.rebuildIndex();
    const auto locator = io::LocaleResourceLocator::build(
        vfs, resolveLegacyLocale(globalData, vfs));
    io::publishLocaleResourceLocator(locator);

    TD_LOG_INFO(
        "[FileSystem] Initialized: locale='{}', {} BIG archives mounted, {} winning VFS files indexed",
        locator->localeDirectory(), m_mountedBigFiles.size(),
        vfs.indexedFileCount());

}

void FileSystemSubsystem::reset() {
    // VFS doesn't need reset between games
}

void FileSystemSubsystem::shutdown() {
    io::clearLocaleResourceLocator();
    m_mountedBigFiles.clear();
    TD_LOG_INFO("[FileSystem] Shutdown");
}
