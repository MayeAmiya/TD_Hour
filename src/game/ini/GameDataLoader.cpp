#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "GameDataLoader.h"
#include "LegacyIniDirectory.h"
#include "game/command/CommandButtonStore.h"
#include "game/command/CommandSetStore.h"
#include "game/object/weapon/ArmorTemplate.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/creation/CrateTemplateCatalog.h"
#include "game/data/base/RankInfoCatalog.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/data/base/DamageFxCatalog.h"
#include "game/data/base/ContentFloatParsing.h"
#include "presentation/fx/content/FxListCatalog.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "core/config/GraphPreferences.h"
#include "game/base/MultiplayerData.h"
#include "VFS.h"
#include "debug/debug.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
namespace game {
namespace {

constexpr container::StringView iniPath(container::StringView file) {
    return file;
}

[[nodiscard]] bool loadSharedObjectSimulationRules(
    container::StringView path,
    math::q32_32& unitDamagedThreshold,
    math::q32_32& unitReallyDamagedThreshold,
    uint32_t& maxTunnelCapacity,
    math::q32_32& standardMinefieldDistance,
    math::q32_32& standardMinefieldDensity,
    math::q32_32& groupMoveClickToGatherFactor,
    container::String& specialPowerViewObject,
    container::String* error) {
    const auto equalIgnoreCase = [](container::StringView left,
                                    container::StringView right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const unsigned char a = static_cast<unsigned char>(left[index]);
            const unsigned char b = static_cast<unsigned char>(right[index]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    };
    GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        if (error) *error = "could not parse GameData source '" +
            container::String{path} + "'";
        return false;
    }
    uint32_t compiledTunnelCapacity = 10;
    float compiledUnitDamagedThreshold = 0.0f;
    float compiledUnitReallyDamagedThreshold = 0.0f;
    bool foundUnitDamagedThreshold = false;
    bool foundUnitReallyDamagedThreshold = false;
    float compiledMinefieldDistance = 40.0f;
    float compiledMinefieldDensity = 0.01f;
    float compiledGroupMoveClickToGatherFactor = 1.0f;
    container::String compiledSpecialPowerViewObject;
    for (const IniBlock& block : parser.blocks()) {
        if (!equalIgnoreCase(block.type, "GameData")) continue;
        const auto blockDiagnosticScope =
            contentDiagnosticProvenanceScope(block.source);
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, text] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            if (equalIgnoreCase(key, "MaxTunnelCapacity")) {
                if (text.empty() || text.front() == '-') {
                    if (error) *error =
                        "invalid GameData.MaxTunnelCapacity value '" + text + "'";
                    return false;
                }
                char* end = nullptr;
                errno = 0;
                const unsigned long long parsed =
                    std::strtoull(text.c_str(), &end, 10);
                if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
                    parsed > std::numeric_limits<uint32_t>::max()) {
                    if (error) *error =
                        "invalid GameData.MaxTunnelCapacity value '" + text + "'";
                    return false;
                }
                compiledTunnelCapacity = static_cast<uint32_t>(parsed);
                continue;
            }
            if (equalIgnoreCase(key, "SpecialPowerViewObject")) {
                // RefCode GlobalData m_specialPowerViewObjectName
                // (INI::parseAsciiString). Stock GameData.ini authors
                // `SuperweaponPing`; an authored `None` is the legacy spelling
                // for "no view object at all", and an unresolvable template is
                // tolerated by RefCode's findTemplate() null check rather than
                // failing the load.
                container::String name{
                    container::trimAsciiCopy(container::StringView{text})};
                if (equalIgnoreCase(name, "None")) name.clear();
                compiledSpecialPowerViewObject = std::move(name);
                continue;
            }
            float* destination = nullptr;
            container::StringView field;
            if (equalIgnoreCase(key, "UnitDamagedThreshold")) {
                destination = &compiledUnitDamagedThreshold;
                field = "UnitDamagedThreshold";
                foundUnitDamagedThreshold = true;
            } else if (equalIgnoreCase(key, "UnitReallyDamagedThreshold")) {
                destination = &compiledUnitReallyDamagedThreshold;
                field = "UnitReallyDamagedThreshold";
                foundUnitReallyDamagedThreshold = true;
            } else if (equalIgnoreCase(key, "StandardMinefieldDistance")) {
                destination = &compiledMinefieldDistance;
                field = "StandardMinefieldDistance";
            } else if (equalIgnoreCase(key, "StandardMinefieldDensity")) {
                destination = &compiledMinefieldDensity;
                field = "StandardMinefieldDensity";
            } else if (equalIgnoreCase(
                           key, "GroupMoveClickToGatherAreaFactor")) {
                destination = &compiledGroupMoveClickToGatherFactor;
                field = "GroupMoveClickToGatherAreaFactor";
            }
            if (!destination) continue;
            const std::optional<float> parsed = parseContentFloat(text, {
                .source = "GameData.ini", .block = "GameData",
                .field = field, .fallback = *destination});
            if (!parsed) {
                if (error) *error = "invalid GameData." +
                    container::String{field} + " value '" + text + "'";
                return false;
            }
            *destination = *parsed;
        }
    }
    if (!foundUnitDamagedThreshold || !foundUnitReallyDamagedThreshold) {
        if (error) *error =
            "required GameData damage thresholds are absent";
        return false;
    }
    if (compiledUnitDamagedThreshold < 0.0f ||
        compiledUnitDamagedThreshold > 1.0f ||
        compiledUnitReallyDamagedThreshold < 0.0f ||
        compiledUnitReallyDamagedThreshold > compiledUnitDamagedThreshold) {
        if (error) *error = "invalid GameData damage threshold ordering";
        return false;
    }
    unitDamagedThreshold = math::q32_32{compiledUnitDamagedThreshold};
    unitReallyDamagedThreshold =
        math::q32_32{compiledUnitReallyDamagedThreshold};
    maxTunnelCapacity = compiledTunnelCapacity;
    standardMinefieldDistance = math::q32_32{compiledMinefieldDistance};
    standardMinefieldDensity = math::q32_32{compiledMinefieldDensity};
    groupMoveClickToGatherFactor = math::q32_32{
        std::max(0.0f, compiledGroupMoveClickToGatherFactor)};
    specialPowerViewObject = std::move(compiledSpecialPowerViewObject);
    return true;
}

constexpr container::StringView kObjectLoadRoots[] = {
    "data/ini/default/Object",
    "data/ini/Object",
};

constexpr container::StringView kCommandButtonLoadRoots[] = {
    "data/ini/default/CommandButton",
    "data/ini/CommandButton",
};

constexpr container::StringView kCommandSetLoadRoots[] = {
    "data/ini/CommandSet",
};

constexpr container::StringView kTerrainLoadRoots[] = {
    "data/ini/default/Terrain",
    "data/ini/Terrain",
};

constexpr container::StringView kArmorLoadRoots[] = {
    "data/ini/Armor",
};

constexpr container::StringView kWeaponLoadRoots[] = {
    "data/ini/Weapon",
};

constexpr container::StringView kLocomotorLoadRoots[] = {
    "data/ini/Locomotor",
};

// RankInfoStore is initialized from Data/INI/Rank in RefCode. Preserve the
// root.ini + optional Rank/*.ini directory convention and its stable order.
constexpr container::StringView kRankInfoLoadRoots[] = {
    "data/ini/Rank",
};

// RefCode initializes ScienceStore with these two loadFileDirectory roots in
// this order. Each root expands to `Science.ini` and recursive `Science/*.ini`
// fragments; ScienceCatalog owns that expansion's stable order.
constexpr container::StringView kScienceLoadRoots[] = {
    "data/ini/default/Science",
    "data/ini/Science",
};

constexpr container::StringView kSpecialPowerLoadRoots[] = {
    "data/ini/default/SpecialPower",
    "data/ini/SpecialPower",
};

// RefCode initializes UpgradeCenter after ThingFactory has compiled Object
// content. Keep the same source priority (Default before normal root) while
// publishing a modern immutable catalog instead of a process-global linked
// template list.
constexpr container::StringView kUpgradeLoadRoots[] = {
    "data/ini/default/Upgrade",
    "data/ini/Upgrade",
};

constexpr container::StringView kObjectCreationListLoadRoots[] = {
    "data/ini/default/ObjectCreationList",
    "data/ini/ObjectCreationList",
};

constexpr container::StringView kCrateLoadRoots[] = {
    "data/ini/default/Crate",
    "data/ini/Crate",
};

// RefCode initializes DamageFXStore from this single loadFileDirectory root.
// It expands to DamageFX.ini followed by optional DamageFX/*.ini fragments.
constexpr container::StringView kDamageFxLoadRoots[] = {
    "data/ini/DamageFX",
};

constexpr container::StringView kParticleSystemLoadRoots[] = {
    "data/ini/ParticleSystem",
};

constexpr container::StringView kFxListLoadRoots[] = {
    "data/ini/default/FXList",
    "data/ini/FXList",
};

// FNV-1a is deliberately fed a framed stream rather than concatenated text:
// the logical path, present/missing state and byte count all participate.  It
// therefore identifies the exact ordered logical winner stream parsed by the
// INI loader, including optional files which are absent in one content
// install but present in another.
class Fnv64Hasher final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void boolean(bool value) noexcept {
        byte(value ? 1u : 0u);
    }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void string(container::StringView value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const unsigned char character : value) {
            byte(character);
        }
    }

    void bytes(container::StringView value) noexcept {
        for (const unsigned char character : value) {
            byte(character);
        }
    }

    [[nodiscard]] uint64_t finish() const noexcept {
        return m_value;
    }

private:
    static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;

    uint64_t m_value = kFnvOffsetBasis;
};

void hashEffectiveIniLayers(
    Fnv64Hasher& hash, container::StringView logicalPath, bool present,
    container::Span<const container::String> layers) {
    hash.string(logicalPath);
    hash.boolean(present);
    if (!present) return;

    // Ordinary INI loads contain exactly one VFS winner. Keep the framed
    // source representation because explicit load operations (for example a
    // map modifier) may still append a separate logical source later.
    hash.u64(static_cast<uint64_t>(layers.size()));
    for (const container::String& layer : layers) {
        hash.u64(static_cast<uint64_t>(layer.size()));
        hash.bytes(layer);
    }
}

void hashEffectiveVfsIni(Fnv64Hasher& hash, container::StringView logicalPath) {
    auto& vfs = io::VFS::instance();
    const bool present = vfs.exists(logicalPath);
    if (!present) {
        hashEffectiveIniLayers(hash, logicalPath, false, {});
        return;
    }
    container::Vector<container::String> winner;
    winner.push_back(vfs.readAll(logicalPath));
    hashEffectiveIniLayers(hash, logicalPath, true, winner);
}

bool loadRequired(container::StringView path, Fnv64Hasher& fingerprint, auto&& loader) {
    hashEffectiveVfsIni(fingerprint, path);

    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        processContentDiagnostics().warn({
            .source = container::String{path},
            .block = "INI",
            .module = "GameDataLoader",
            .adoptedValue = "compiled defaults/empty content",
            .reason = "authored INI is missing from VFS; loading continues with safe defaults",
        });
        return true;
    }
    if (loader(container::String{path})) return true;
    processContentDiagnostics().warn({
        .source = container::String{path},
        .block = "INI",
        .module = "GameDataLoader",
        .adoptedValue = "compiled defaults/partial content",
        .reason = "authored INI loader reported failure; loading continues in degraded mode",
    });
    return true;
}

bool loadOptional(container::StringView path, Fnv64Hasher& fingerprint,
                  auto&& loader) {
    hashEffectiveVfsIni(fingerprint, path);
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    return loader(container::String{path});
}

void warnLoaderFailure(container::StringView source,
                       container::StringView block,
                       container::StringView module,
                       container::StringView detail,
                       container::StringView adopted = "compiled defaults/partial content") {
    processContentDiagnostics().warn({
        .source = container::String{source},
        .block = container::String{block},
        .module = container::String{module},
        .rawValue = container::String{detail},
        .adoptedValue = container::String{adopted},
        .reason = "authored content could not be fully compiled; loading continues in degraded mode",
    });
}

bool loadOrderedIniFiles(const container::Vector<container::String>& sourceFiles,
                         container::StringView fingerprintDomain,
                         Fnv64Hasher& fingerprint, auto&& loader) {
    fingerprint.string(fingerprintDomain);
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
        if (loader(path)) continue;
        processContentDiagnostics().warn({
            .source = path,
            .block = "INI",
            .module = "GameDataLoader",
            .adoptedValue = "remaining/partial catalog content",
            .reason = "one authored INI source failed to compile; remaining sources continue",
        });
    }
    return true;
}

bool loadOptionalScienceCatalog(Fnv64Hasher& fingerprint,
                                container::SharedPtr<const engine::ScienceCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        engine::ScienceCatalog::enumerateVfsLoadFiles(kScienceLoadRoots);
    // Hash the exact ordered winner set consumed below for every root and
    // directory fragment. This keeps a fragment-only mod from
    // silently sharing a lockstep/replay identity with base content.
    fingerprint.string("GameDataLoader.science-catalog-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }

    auto catalog = std::make_shared<engine::ScienceCatalog>();
    if (sourceFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "Science",
            .module = "ScienceCatalog",
            .adoptedValue = "empty catalog",
            .reason = "Science INI directory is absent; affected science references are disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
        output = std::move(catalog);
        return true;
    }

    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "Science",
            .module = "ScienceCatalog",
            .rawValue = container::String{error},
            .adoptedValue = "empty catalog",
            .reason = "Science catalog compilation failed; affected science references are disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalRankInfoCatalog(
    Fnv64Hasher& fingerprint,
    container::SharedPtr<const engine::RankInfoCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        engine::RankInfoCatalog::enumerateVfsLoadFiles(kRankInfoLoadRoots);
    fingerprint.string("GameDataLoader.rank-info-catalog-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }

    auto catalog = std::make_shared<engine::RankInfoCatalog>();
    if (sourceFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Rank.ini",
            .block = "Rank",
            .module = "RankInfoCatalog",
            .adoptedValue = "sealed empty catalog",
            .reason = "Rank INI directory is absent; rank progression is disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
        output = std::move(catalog);
        return true;
    }

    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        processContentDiagnostics().warn({
            .source = "data/ini/Rank",
            .block = "Rank",
            .module = "RankInfoCatalog",
            .rawValue = std::move(error),
            .adoptedValue = "sealed empty catalog",
            .reason = "Rank catalog compilation failed; rank progression is disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalSpecialPowerCatalog(
    Fnv64Hasher& fingerprint,
    container::SharedPtr<const engine::SpecialPowerCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        engine::SpecialPowerCatalog::enumerateVfsLoadFiles(
            kSpecialPowerLoadRoots);
    fingerprint.string("GameDataLoader.special-power-catalog-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }
    auto catalog = std::make_shared<engine::SpecialPowerCatalog>();
    if (sourceFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "SpecialPower",
            .module = "SpecialPowerCatalog",
            .adoptedValue = "empty catalog",
            .reason = "SpecialPower INI directory is absent; affected powers are disabled",
        });
    }
    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "SpecialPower",
            .module = "SpecialPowerCatalog",
            .rawValue = container::String{error},
            .adoptedValue = "empty catalog",
            .reason = "SpecialPower catalog compilation failed; affected powers are disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalUpgradeCatalog(Fnv64Hasher& fingerprint,
                                container::SharedPtr<const engine::UpgradeCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        engine::UpgradeCatalog::enumerateVfsLoadFiles(kUpgradeLoadRoots);
    fingerprint.string("GameDataLoader.upgrade-catalog-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }

    auto catalog = std::make_shared<engine::UpgradeCatalog>();
    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        warnLoaderFailure(
            "data/ini/Upgrade", "Upgrade", "UpgradeCatalog", error,
            "empty catalog");
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalObjectCreationListCatalog(
    Fnv64Hasher& fingerprint,
    container::SharedPtr<const ObjectCreationListCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        ObjectCreationListCatalog::enumerateVfsLoadFiles(
            kObjectCreationListLoadRoots);
    fingerprint.string("GameDataLoader.object-creation-list-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }

    auto catalog = std::make_shared<ObjectCreationListCatalog>();
    if (sourceFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "ObjectCreationList",
            .module = "ObjectCreationListCatalog",
            .adoptedValue = "empty catalog",
            .reason = "ObjectCreationList INI directory is absent; affected OCL branches are disabled",
        });
    }
    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "ObjectCreationList",
            .module = "ObjectCreationListCatalog",
            .rawValue = container::String{error},
            .adoptedValue = "empty catalog",
            .reason = "ObjectCreationList catalog compilation failed; affected OCL branches are disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalCrateTemplateCatalog(
    Fnv64Hasher& fingerprint,
    container::SharedPtr<const CrateTemplateCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        CrateTemplateCatalog::enumerateVfsLoadFiles(kCrateLoadRoots);
    fingerprint.string("GameDataLoader.crate-template-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }
    auto catalog = std::make_shared<CrateTemplateCatalog>();
    if (sourceFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "CrateData",
            .module = "CrateTemplateCatalog",
            .adoptedValue = "empty catalog",
            .reason = "Crate INI directory is absent; affected crate branches are disabled",
        });
    }
    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "CrateData",
            .module = "CrateTemplateCatalog",
            .rawValue = container::String{error},
            .adoptedValue = "empty catalog",
            .reason = "CrateData catalog compilation failed; affected crate branches are disabled",
        });
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    output = std::move(catalog);
    return true;
}

bool loadOptionalDamageFxCatalog(
    Fnv64Hasher& fingerprint,
    const engine::fx::FxListCatalog& fxLists,
    container::SharedPtr<const DamageFxCatalog>& output) {
    const container::Vector<container::String> sourceFiles =
        DamageFxCatalog::enumerateVfsLoadFiles(kDamageFxLoadRoots);
    fingerprint.string("GameDataLoader.damage-fx-catalog-inputs");
    fingerprint.u64(static_cast<uint64_t>(sourceFiles.size()));
    for (const container::String& path : sourceFiles) {
        hashEffectiveVfsIni(fingerprint, path);
    }

    auto catalog = std::make_shared<DamageFxCatalog>();
    container::String error;
    if (!catalog->loadFromVfsFiles(sourceFiles, &error)) {
        warnLoaderFailure(
            "data/ini/DamageFX", "DamageFX", "DamageFxCatalog", error,
            "empty catalog");
        static_cast<void>(catalog->loadFromVfsFiles({}, nullptr));
    }
    catalog->resolveFxReferences(fxLists);
    for (const container::String& diagnostic : catalog->diagnostics()) {
        TD_LOG_WARN("[GameDataLoader] DamageFX: {}", diagnostic);
    }
    output = std::move(catalog);
    return true;
}

void loadOptionalPresentationFxCatalogs(
    container::SharedPtr<const engine::fx::ParticleSystemCatalog>& particleOutput,
    container::SharedPtr<const engine::fx::FxListCatalog>& fxListOutput) {
    auto particles = std::make_shared<engine::fx::ParticleSystemCatalog>();
    const container::Vector<container::String> particleFiles =
        engine::fx::ParticleSystemCatalog::enumerateVfsLoadFiles(kParticleSystemLoadRoots);
    container::String error;
    if (!particleFiles.empty() && !particles->loadFromVfsFiles(particleFiles, &error)) {
        TD_LOG_WARN("[GameDataLoader] ParticleSystem presentation catalog disabled: {}", error);
        particles = std::make_shared<engine::fx::ParticleSystemCatalog>();
    }

    auto fxLists = std::make_shared<engine::fx::FxListCatalog>();
    const container::Vector<container::String> fxListFiles =
        engine::fx::FxListCatalog::enumerateVfsLoadFiles(kFxListLoadRoots);
    error.clear();
    if (!fxListFiles.empty() && !fxLists->loadFromVfsFiles(fxListFiles, &error)) {
        TD_LOG_WARN("[GameDataLoader] FXList presentation catalog disabled: {}", error);
        fxLists = std::make_shared<engine::fx::FxListCatalog>();
    }
    fxLists->resolveReferences(*particles);

    TD_LOG_INFO("[GameDataLoader] Presentation FX content: particles={} fxLists={} diagnostics={}",
                particles->templates().size(), fxLists->definitions().size(),
                particles->diagnostics().size() + fxLists->diagnostics().size());
    particleOutput = std::move(particles);
    fxListOutput = std::move(fxLists);
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool parseLegacyBool(container::StringView value, bool fallback) noexcept {
    if (equalAsciiInsensitive(value, "yes") || equalAsciiInsensitive(value, "true") ||
        equalAsciiInsensitive(value, "on") || value == "1") {
        return true;
    }
    if (equalAsciiInsensitive(value, "no") || equalAsciiInsensitive(value, "false") ||
        equalAsciiInsensitive(value, "off") || value == "0") {
        return false;
    }
    return fallback;
}

[[nodiscard]] float parseFiniteLegacyReal(container::StringView value, float fallback) noexcept {
    return parseContentFloatOr(value, {
        .source = "Weather.ini", .block = "Weather",
        .field = "Real", .fallback = fallback});
}

void applyWeatherSetting(engine::script::ScriptWeatherSnowSettings& settings,
                         const IniBlock& block) {
    for (const auto& [key, value] : block.values) {
        if (equalAsciiInsensitive(key, "SnowTexture")) {
            if (!value.empty()) settings.texture = value;
        } else if (equalAsciiInsensitive(key, "SnowFrequencyScaleX")) {
            settings.frequencyScaleX = parseFiniteLegacyReal(value, settings.frequencyScaleX);
        } else if (equalAsciiInsensitive(key, "SnowFrequencyScaleY")) {
            settings.frequencyScaleY = parseFiniteLegacyReal(value, settings.frequencyScaleY);
        } else if (equalAsciiInsensitive(key, "SnowAmplitude")) {
            settings.amplitude = parseFiniteLegacyReal(value, settings.amplitude);
        } else if (equalAsciiInsensitive(key, "SnowPointSize")) {
            settings.pointSize = parseFiniteLegacyReal(value, settings.pointSize);
        } else if (equalAsciiInsensitive(key, "SnowMaxPointSize")) {
            settings.maximumPointSize = parseFiniteLegacyReal(value, settings.maximumPointSize);
        } else if (equalAsciiInsensitive(key, "SnowMinPointSize")) {
            settings.minimumPointSize = parseFiniteLegacyReal(value, settings.minimumPointSize);
        } else if (equalAsciiInsensitive(key, "SnowQuadSize")) {
            settings.quadSize = parseFiniteLegacyReal(value, settings.quadSize);
        } else if (equalAsciiInsensitive(key, "SnowBoxDimensions")) {
            settings.boxDimensions = parseFiniteLegacyReal(value, settings.boxDimensions);
        } else if (equalAsciiInsensitive(key, "SnowBoxDensity")) {
            settings.boxDensity = parseFiniteLegacyReal(value, settings.boxDensity);
        } else if (equalAsciiInsensitive(key, "SnowVelocity")) {
            settings.velocity = parseFiniteLegacyReal(value, settings.velocity);
        } else if (equalAsciiInsensitive(key, "SnowPointSprites")) {
            settings.usePointSprites = parseLegacyBool(value, settings.usePointSprites);
        } else if (equalAsciiInsensitive(key, "SnowEnabled")) {
            settings.enabled = parseLegacyBool(value, settings.enabled);
        }
    }

    // RefCode assumes these values are positive when it constructs its snow
    // volume.  Preserve sane authored values verbatim, but make malformed
    // modern input safe before it becomes a renderer-side frame value.
    settings.pointSize = std::max(0.0f, settings.pointSize);
    settings.minimumPointSize = std::max(0.0f, settings.minimumPointSize);
    settings.maximumPointSize = std::max(settings.minimumPointSize, settings.maximumPointSize);
    settings.quadSize = std::max(0.0f, settings.quadSize);
    settings.boxDimensions = std::max(1.0f, settings.boxDimensions);
    settings.boxDensity = std::max(0.001f, settings.boxDensity);
    settings.velocity = std::max(0.001f, settings.velocity);
}

bool loadOptionalWeatherIni(container::StringView path,
                            engine::script::ScriptWeatherSnowSettings& settings) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;

    GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) return false;
    for (const IniBlock& block : parser.blocks()) {
        if (equalAsciiInsensitive(block.type, "Weather")) {
            applyWeatherSetting(settings, block);
        }
    }
    return true;
}

void applySkyboxTextureSetting(engine::script::ScriptSkyboxTextureSet& settings,
                               const IniBlock& block) {
    // Match WaterTransparencySetting's field names and N/E/S/W/T order from
    // RefCode Water.h. Explicitly ordered logical files can still contribute
    // sparse fields, but physical copies of one path are resolved by VFS.
    for (const auto& [key, value] : block.values) {
        if (equalAsciiInsensitive(key, "SkyboxTextureN")) {
            settings.textureNames[0] = value;
        } else if (equalAsciiInsensitive(key, "SkyboxTextureE")) {
            settings.textureNames[1] = value;
        } else if (equalAsciiInsensitive(key, "SkyboxTextureS")) {
            settings.textureNames[2] = value;
        } else if (equalAsciiInsensitive(key, "SkyboxTextureW")) {
            settings.textureNames[3] = value;
        } else if (equalAsciiInsensitive(key, "SkyboxTextureT")) {
            settings.textureNames[4] = value;
        }
    }
}

bool loadOptionalWaterIni(
    container::StringView path,
    engine::script::ScriptSkyboxTextureSet& skyboxSettings,
    engine::script::ScriptWaterPresentationSettings& waterSettings) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;

    const container::String content = vfs.readAll(path);
    GeneralsIniParser parser;
    if (!parser.parse(content)) return false;
    for (const IniBlock& block : parser.blocks()) {
        if (equalAsciiInsensitive(block.type, "WaterTransparency")) {
            applySkyboxTextureSetting(skyboxSettings, block);
        }
    }
    container::String error;
    if (!engine::script::applyScriptWaterPresentationIni(
            content, waterSettings, &error)) {
        TD_LOG_WARN("[GameDataLoader] Water presentation parse failed: {}", error);
        return false;
    }
    return true;
}

bool loadOptionalRoadIni(
    container::StringView path,
    engine::script::ScriptTerrainRoadPresentationSettings& settings) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    if (!engine::script::applyScriptTerrainRoadPresentationIni(
            content, settings, &error)) {
        TD_LOG_WARN("[GameDataLoader] Roads presentation parse failed: {}", error);
        return false;
    }
    return true;
}

bool loadTrackMarksIniLayers(
    container::StringView path,
    bool gameData,
    engine::TrackMarksPresentationSettings& settings,
    container::Vector<container::String>& diagnostics) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    const bool parsed = gameData
        ? engine::applyTrackMarksGameDataIni(
              content, settings, &diagnostics, &error)
        : engine::applyTrackMarksGameLodIni(
              content, settings, &diagnostics, &error);
    if (!parsed) {
        TD_LOG_WARN("[GameDataLoader] TrackMarks presentation parse failed for {}: {}",
                    path, error);
        return false;
    }
    return true;
}

bool loadRenderSettingsIniLayers(
    container::StringView path, bool gameData,
    engine::RenderGameDataSettings& settings,
    container::Vector<container::String>& diagnostics) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    const bool parsed = gameData
        ? engine::applyRenderGameDataIni(
              content, settings, &diagnostics, &error)
        : engine::applyRenderGameLodIni(
              content, settings, &diagnostics, &error);
    if (!parsed) {
        TD_LOG_WARN(
            "[GameDataLoader] Render settings parse failed for {}: {}",
            path, error);
        return false;
    }
    return true;
}

bool loadMouseSettingsIni(
    container::StringView path,
    engine::RenderGameDataSettings& settings,
    container::Vector<container::String>& diagnostics) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    if (!engine::applyRenderMouseIni(
            content, settings, &diagnostics, &error)) {
        TD_LOG_WARN(
            "[GameDataLoader] Mouse input settings parse failed for {}: {}",
            path, error);
        return false;
    }
    return true;
}

bool loadObjectCaptionSettingsIniLayers(
    container::StringView path, bool language,
    engine::RenderGameDataSettings& settings,
    container::Vector<container::String>& diagnostics) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    const bool parsed = language
        ? engine::applyRenderLanguageIni(
              content, settings, &diagnostics, &error)
        : engine::applyRenderInGameUiIni(
              content, settings, &diagnostics, &error);
    if (!parsed) {
        TD_LOG_WARN(
            "[GameDataLoader] Object caption settings parse failed for {}: {}",
            path, error);
        return false;
    }
    return true;
}

bool loadMiscAudioPresentationSettingsIniLayers(
    container::StringView path,
    engine::RenderGameDataSettings& settings,
    container::Vector<container::String>& diagnostics) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return true;
    const container::String content = vfs.readAll(path);
    container::String error;
    if (!engine::applyPresentationMiscAudioIni(
            content, settings, &diagnostics, &error)) {
        TD_LOG_WARN(
            "[GameDataLoader] MiscAudio presentation parse failed for {}: {}",
            path, error);
        return false;
    }
    return true;
}

} // namespace

GameDataLoader& GameDataLoader::instance() {
    static GameDataLoader s_instance;
    return s_instance;
}

void GameDataLoader::clear() {
    CommandButtonStore::instance().clear();
    CommandSetStore::instance().clear();
    ArmorStore::instance().clear();
    WeaponStore::instance().clear();
    LocomotorStore::instance().clear();
    ThingFactory::instance().clear();
    m_ruleset.reset();
    m_rankInfoCatalog.reset();
    m_scienceCatalog.reset();
    m_specialPowerCatalog.reset();
    m_upgradeCatalog.reset();
    m_objectCreationListCatalog.reset();
    m_crateTemplateCatalog.reset();
    m_damageFxCatalog.reset();
    m_pristineBoneCatalog.reset();
    m_particleSystemCatalog.reset();
    m_fxListCatalog.reset();
    m_baseRegenerationRules = {};
    m_aiSimulationRules = {};
    m_unitDamagedThreshold = {};
    m_unitReallyDamagedThreshold = {};
    m_maxTunnelCapacity = 10;
    m_standardMinefieldDistance = math::q32_32{40.0f};
    m_standardMinefieldDensity = math::q32_32{0.01f};
    m_groupMoveClickToGatherFactor = math::q32_32{1.0f};
    m_buildPlacementSimulationRules = {};
    m_energySimulationRules = {};
    m_economySimulationRules = {};
    m_physicsSimulationRules = {};
    m_veterancySimulationRules = {};
    m_difficultySimulationRules = {};
    m_globalWeaponBonuses = {};
    m_weatherPresentationSettings = {};
    m_skyboxPresentationTextures = {};
    m_waterPresentationSettings = {};
    m_terrainRoadPresentationSettings = {};
    m_trackMarksPresentationSettings = {};
    m_trackMarksPresentationDiagnostics.clear();
    m_renderGameDataSettings = {};
    m_renderQualitySettingsManager.configure(
        engine::renderFeaturePresetSetFromGameData(m_renderGameDataSettings),
        engine::renderDisplayPresetSetFromGameData(m_renderGameDataSettings),
        engine::renderFeatureQualityFromGameData(m_renderGameDataSettings),
        engine::renderDisplaySettingsFromGameData(m_renderGameDataSettings));
    m_renderGameDataDiagnostics.clear();
    m_loaded = false;
    m_simulationContentFingerprint = 0;
}

void GameDataLoader::setRenderQualityExternalOverrides(
    const engine::RenderFeatureQualityOverrides& feature,
    const engine::RenderDisplayOverrides& display) {
    m_renderQualitySettingsManager.setOverrides(feature, display);
    if (const auto quality = m_renderQualitySettingsManager.snapshot()) {
        engine::projectRenderFeatureQualityToGameData(
            quality->feature.requested, m_renderGameDataSettings);
        engine::projectRenderDisplaySettingsToGameData(
            quality->display.effective, m_renderGameDataSettings);
    }
}

void GameDataLoader::setRenderQualityRuntimeOverrides(
    const engine::RenderFeatureQualityOverrides& feature,
    const engine::RenderDisplayOverrides& display) {
    m_renderQualitySettingsManager.setOverrides(
        engine::RenderQualityOverrideLayer::Runtime, feature, display);
    if (const auto quality = m_renderQualitySettingsManager.snapshot()) {
        // Active sessions keep their frozen Feature snapshot; this projection
        // supplies future sessions and legacy readers. Display remains live
        // through the manager revision consumed at the renderer boundary.
        engine::projectRenderFeatureQualityToGameData(
            quality->feature.requested, m_renderGameDataSettings);
        engine::projectRenderDisplaySettingsToGameData(
            quality->display.effective, m_renderGameDataSettings);
    }
}

void GameDataLoader::clearRenderQualityRuntimeOverrides() {
    m_renderQualitySettingsManager.clearOverrides(
        engine::RenderQualityOverrideLayer::Runtime);
    if (const auto quality = m_renderQualitySettingsManager.snapshot()) {
        engine::projectRenderFeatureQualityToGameData(
            quality->feature.requested, m_renderGameDataSettings);
        engine::projectRenderDisplaySettingsToGameData(
            quality->display.effective, m_renderGameDataSettings);
    }
}

void GameDataLoader::updateRenderWindowExtent(
    uint32_t width, uint32_t height) {
    if (width == 0u || height == 0u) return;
    const auto current = m_renderQualitySettingsManager.snapshot();
    if (!current || current->display.effective.displayMode !=
            engine::RenderDisplayMode::Windowed) {
        return;
    }
    engine::RenderDisplayOverrides patch;
    patch.width = width;
    patch.height = height;
    m_renderQualitySettingsManager.patchDisplayOverrides(
        engine::RenderQualityOverrideLayer::Runtime, patch);
    if (const auto quality = m_renderQualitySettingsManager.snapshot()) {
        engine::projectRenderDisplaySettingsToGameData(
            quality->display.effective, m_renderGameDataSettings);
    }
}

void GameDataLoader::setRenderDisplayCapabilities(
    const engine::RenderDisplayCapabilities& capabilities) {
    m_renderQualitySettingsManager.setCapabilities(capabilities);
    if (const auto quality = m_renderQualitySettingsManager.snapshot()) {
        engine::projectRenderDisplaySettingsToGameData(
            quality->display.effective, m_renderGameDataSettings);
    }
}

bool GameDataLoader::loadAll() {
    // The process-wide VFS mount set is frozen after FileSystemSubsystem
    // initialization.  Campaign Next/Retry and consecutive in-game sessions
    // therefore reuse the already validated immutable catalogs instead of
    // rereading every INI and authoritative W3D source.  Tests/tools that
    // replace content call clear() explicitly before loading another set.
    if (m_loaded) return true;

    processContentDiagnostics().clear();
    TD_LOG_INFO("[GameDataLoader] Loading game data from VFS/BIG");
    clear();

    Fnv64Hasher contentFingerprint;
    // This domain/version prefix protects the byte stream against accidental
    // reuse by another FNV-based content identity in the future.
    contentFingerprint.string("GameDataLoader.simulation-content");
    // v20 adds the frozen RankInfo stream used by authoritative player
    // progression and generals-point grants.
    contentFingerprint.u32(20);

    bool ok = true;
    ok &= loadRequired(iniPath("data/ini/GameData.ini"), contentFingerprint,
        [this](const container::String& path) {
            container::String error;
            bool gameDataOk = true;
            if (!engine::PhysicsSimulationRules::loadFromLegacyGameData(
                    path, m_physicsSimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "PhysicsSimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::BaseRegenerationRules::loadFromLegacyGameData(
                    path, m_baseRegenerationRules, &error)) {
                warnLoaderFailure(path, "GameData", "BaseRegenerationRules", error);
                gameDataOk = false;
            }
            if (!loadSharedObjectSimulationRules(
                    path, m_unitDamagedThreshold,
                    m_unitReallyDamagedThreshold, m_maxTunnelCapacity,
                    m_standardMinefieldDistance,
                    m_standardMinefieldDensity,
                    m_groupMoveClickToGatherFactor,
                    m_specialPowerViewObject, &error)) {
                warnLoaderFailure(path, "GameData", "ObjectSimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::BuildPlacementSimulationRules::loadFromLegacyGameData(
                    path, m_buildPlacementSimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "BuildPlacementSimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::EnergySimulationRules::loadFromLegacyGameData(
                    path, m_energySimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "EnergySimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::EconomySimulationRules::loadFromLegacyGameData(
                    path, m_economySimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "EconomySimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::VeterancySimulationRules::loadFromLegacyGameData(
                    path, m_veterancySimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "VeterancySimulationRules", error);
                gameDataOk = false;
            }
            if (!engine::DifficultySimulationRules::loadFromLegacyGameData(
                    path, m_difficultySimulationRules, &error)) {
                warnLoaderFailure(path, "GameData", "DifficultySimulationRules", error);
                gameDataOk = false;
            }
            if (!WeaponBonusSet::loadFromLegacyGameData(
                    path, m_globalWeaponBonuses, &error)) {
                warnLoaderFailure(path, "GameData", "WeaponBonusSet", error);
                gameDataOk = false;
            }
            if (!loadTrackMarksIniLayers(
                    path, true, m_trackMarksPresentationSettings,
                    m_trackMarksPresentationDiagnostics)) {
                gameDataOk = false;
            }
            if (!loadRenderSettingsIniLayers(
                    path, true, m_renderGameDataSettings,
                    m_renderGameDataDiagnostics)) {
                gameDataOk = false;
            }
            return gameDataOk;
        });
    // AIData is authored as a base + override pair, exactly like the Object /
    // Science / Upgrade directory roots above. RefCode's GameEngine loads
    // `Data\INI\Default\AIData` and then `Data\INI\AIData` (GameEngine.cpp:605)
    // and every shipped value lives in the Default layer: the sibling
    // `Data\INI\AIData.ini` is an empty `AIData ... End` block in both content
    // trees. These are two DISTINCT logical paths, not two VFS layers of one
    // path, so the single-winner VFS policy still applies to each of them
    // independently. Missing either file degrades to the previous layer.
    ok &= loadRequired(iniPath("data/ini/default/AIData.ini"),
        contentFingerprint,
        [this](const container::String& path) {
            container::String error;
            if (!engine::AISimulationRules::loadFromLegacyAIData(
                    path, m_aiSimulationRules, &error)) {
                warnLoaderFailure(path, "AIData", "AISimulationRules", error);
                return false;
            }
            return true;
        });
    ok &= loadRequired(iniPath("data/ini/AIData.ini"), contentFingerprint,
        [this](const container::String& path) {
            container::String error;
            if (!engine::AISimulationRules::applyLegacyAIDataFile(
                    path, m_aiSimulationRules, &error)) {
                warnLoaderFailure(path, "AIData", "AISimulationRules", error);
                return false;
            }
            return true;
        });
    const auto applyTerrainConstruction = [this](
            const container::String& path) {
        container::String error;
        if (!m_buildPlacementSimulationRules.terrainTypes.applyLegacyIniFile(
                path, &error)) {
            warnLoaderFailure(
                path, "Terrain", "TerrainConstructionCatalog", error);
            return false;
        }
        return true;
    };
    const container::Vector<container::String> terrainFiles =
        game::ini::enumerateLegacyIniDirectories(kTerrainLoadRoots);
    if (terrainFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Terrain",
            .block = "Terrain",
            .module = "TerrainConstructionCatalog",
            .adoptedValue = "empty terrain-construction catalog",
            .reason = "Terrain INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            terrainFiles, "GameDataLoader.terrain-inputs", contentFingerprint,
            applyTerrainConstruction);
    }
    const container::Vector<container::String> armorFiles =
        game::ini::enumerateLegacyIniDirectories(kArmorLoadRoots);
    if (armorFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Armor",
            .block = "Armor",
            .module = "ArmorStore",
            .adoptedValue = "empty armor store",
            .reason = "Armor INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            armorFiles, "GameDataLoader.armor-inputs", contentFingerprint,
            [](const container::String& path) {
                return ArmorStore::instance().loadFromIni(
                    path, game::ini::LegacyIniLoadType::Overwrite);
            });
    }
    const container::Vector<container::String> weaponFiles =
        game::ini::enumerateLegacyIniDirectories(kWeaponLoadRoots);
    if (weaponFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Weapon",
            .block = "Weapon",
            .module = "WeaponStore",
            .adoptedValue = "empty weapon store",
            .reason = "Weapon INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            weaponFiles, "GameDataLoader.weapon-inputs", contentFingerprint,
            [](const container::String& path) {
                return WeaponStore::instance().loadFromIni(
                    path, game::ini::LegacyIniLoadType::Overwrite);
            });
    }
    // OCL definitions refer to Weapons and Objects by name. Freeze the
    // recipes now, then resolve those references only after ThingFactory has
    // compiled the full object universe into the session snapshot.
    ok &= loadOptionalObjectCreationListCatalog(
        contentFingerprint, m_objectCreationListCatalog);
    ok &= loadOptionalCrateTemplateCatalog(
        contentFingerprint, m_crateTemplateCatalog);
    const container::Vector<container::String> locomotorFiles =
        game::ini::enumerateLegacyIniDirectories(kLocomotorLoadRoots);
    if (locomotorFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Locomotor",
            .block = "Locomotor",
            .module = "LocomotorStore",
            .adoptedValue = "empty locomotor store",
            .reason = "Locomotor INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            locomotorFiles, "GameDataLoader.locomotor-inputs",
            contentFingerprint, [](const container::String& path) {
                return LocomotorStore::instance().loadFromIni(
                    path, game::ini::LegacyIniLoadType::Overwrite);
            });
    }
    ok &= loadOptionalRankInfoCatalog(contentFingerprint, m_rankInfoCatalog);
    ok &= loadOptionalScienceCatalog(contentFingerprint, m_scienceCatalog);
    ok &= loadOptionalSpecialPowerCatalog(
        contentFingerprint, m_specialPowerCatalog);
    // Weather is presentation-only and therefore intentionally excluded from
    // the simulation fingerprint below. SHOW_WEATHER changes only the copied
    // visibility bit in a GameSession; it must not alter lockstep state.
    static_cast<void>(loadOptionalWeatherIni("data/ini/Weather.ini",
                                             m_weatherPresentationSettings));
    // WaterTransparency controls the one W3DWater-owned new_skybox model.
    // It is visual-only just like Weather.ini, so it intentionally stays out
    // of the simulation fingerprint. The RefCode defaults remain installed
    // when Water.ini is absent or only contains its shipped comment stub.
    static_cast<void>(loadOptionalWaterIni("data/ini/Water.ini",
                                           m_skyboxPresentationTextures,
                                           m_waterPresentationSettings));
    static_cast<void>(loadOptionalRoadIni("data/ini/Roads.ini",
                                          m_terrainRoadPresentationSettings));
    static_cast<void>(loadTrackMarksIniLayers(
        "data/ini/GameLOD.ini", false, m_trackMarksPresentationSettings,
        m_trackMarksPresentationDiagnostics));
    static_cast<void>(loadRenderSettingsIniLayers(
        "data/ini/GameLOD.ini", false, m_renderGameDataSettings,
        m_renderGameDataDiagnostics));
    static_cast<void>(loadMouseSettingsIni(
        "data/ini/Mouse.ini", m_renderGameDataSettings,
        m_renderGameDataDiagnostics));
    // Drawable caption style belongs to InGameUI, then the active locale's
    // Language block may replace the complete font descriptor.  Both inputs
    // are visual-only and therefore deliberately excluded from lockstep's
    // simulation-content fingerprint.
    constexpr container::Array<container::StringView, 1> inGameUiRoots{{
        "data/ini/InGameUI",
    }};
    for (const container::String& path :
         game::ini::enumerateLegacyIniDirectories(inGameUiRoots)) {
        static_cast<void>(loadObjectCaptionSettingsIniLayers(
            path, false, m_renderGameDataSettings,
            m_renderGameDataDiagnostics));
    }
    constexpr container::Array<container::StringView, 1> miscAudioRoots{{
        "data/ini/MiscAudio",
    }};
    for (const container::String& path :
         game::ini::enumerateLegacyIniDirectories(miscAudioRoots)) {
        static_cast<void>(loadMiscAudioPresentationSettingsIniLayers(
            path, m_renderGameDataSettings,
            m_renderGameDataDiagnostics));
    }
    const container::StringView drawableCaptionLanguagePath =
        io::VFS::instance().exists("Language.ini")
            ? container::StringView{"Language.ini"}
            : container::StringView{"data/ini/Language.ini"};
    static_cast<void>(loadObjectCaptionSettingsIniLayers(
        drawableCaptionLanguagePath, true, m_renderGameDataSettings,
        m_renderGameDataDiagnostics));
    config::GraphPreferences renderPreferences;
    if (io::VFS::instance().exists("Options.ini")) {
        static_cast<void>(renderPreferences.load("Options.ini"));
    }
    engine::applyRenderOptions(
        renderPreferences, m_renderGameDataSettings,
        m_renderQualitySettingsManager,
        &m_renderGameDataDiagnostics);
    for (const container::String& diagnostic : m_trackMarksPresentationDiagnostics) {
        TD_LOG_WARN("[GameDataLoader] TrackMarks presentation: {}", diagnostic);
    }
    for (const container::String& diagnostic : m_renderGameDataDiagnostics) {
        TD_LOG_WARN("[GameDataLoader] Render settings: {}", diagnostic);
    }
    loadOptionalPresentationFxCatalogs(m_particleSystemCatalog, m_fxListCatalog);
    ok &= loadOptionalDamageFxCatalog(
        contentFingerprint, *m_fxListCatalog, m_damageFxCatalog);
    const container::Vector<container::String> commandButtonFiles =
        game::ini::enumerateLegacyIniDirectories(kCommandButtonLoadRoots);
    if (commandButtonFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/CommandButton",
            .block = "CommandButton",
            .module = "CommandButtonStore",
            .adoptedValue = "empty command-button store",
            .reason = "CommandButton INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            commandButtonFiles, "GameDataLoader.command-button-inputs",
            contentFingerprint, [](const container::String& path) {
                return CommandButtonStore::instance().loadFromIni(
                    path, game::ini::LegacyIniLoadType::Overwrite);
            });
    }
    const container::Vector<container::String> commandSetFiles =
        game::ini::enumerateLegacyIniDirectories(kCommandSetLoadRoots);
    if (commandSetFiles.empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/CommandSet",
            .block = "CommandSet",
            .module = "CommandSetStore",
            .adoptedValue = "empty command-set store",
            .reason = "CommandSet INI directory is empty",
        });
    } else {
        ok &= loadOrderedIniFiles(
            commandSetFiles, "GameDataLoader.command-set-inputs",
            contentFingerprint, [](const container::String& path) {
                return CommandSetStore::instance().loadFromIni(
                    path, game::ini::LegacyIniLoadType::Overwrite);
            });
    }

    const container::Vector<container::String> objectFiles =
        game::ini::enumerateLegacyIniDirectories(kObjectLoadRoots);
    contentFingerprint.string("GameDataLoader.object-inputs");
    contentFingerprint.u64(static_cast<uint64_t>(objectFiles.size()));
    container::Vector<ThingIniSource> objectSources;
    objectSources.reserve(objectFiles.size());
    auto& objectVfs = io::VFS::instance();
    for (const container::String& path : objectFiles) {
        const bool present = objectVfs.exists(path);
        container::Vector<container::String> layers;
        if (present) layers.push_back(objectVfs.readAll(path));
        hashEffectiveIniLayers(
            contentFingerprint, path, present, layers);
        if (!present || layers.empty()) {
            processContentDiagnostics().warn({
                .source = path,
                .block = "Object",
                .module = "ThingFactory",
                .adoptedValue = "source omitted",
                .reason = "Object INI disappeared from VFS during load",
            });
            continue;
        }
        objectSources.push_back({
            .path = path,
            .layers = std::move(layers),
        });
    }
    if (!objectSources.empty()) {
        if (!ThingFactory::instance().loadFromIniSources(objectSources)) {
            processContentDiagnostics().warn({
                .source = "data/ini/Object",
                .block = "Object",
                .module = "ThingFactory",
                .adoptedValue = "successfully compiled object recipes only",
                .reason = "one or more Object recipes failed; loading continues with partial content",
            });
        }
    }
    // RefCode's Crate.ini top-level parser accepts both CrateData and Object
    // blocks. The split modern catalogs must therefore feed the same source
    // to ThingFactory as well, otherwise the CrateObject templates referenced
    // by Elite/Heroic crate recipes never enter the object universe.
    for (const container::String& path :
         CrateTemplateCatalog::enumerateVfsLoadFiles(kCrateLoadRoots)) {
        if (!ThingFactory::instance().loadFromIni(path)) {
            processContentDiagnostics().warn({
                .source = path,
                .block = "Object",
                .module = "ThingFactory",
                .adoptedValue = "successfully compiled Crate Object recipes only",
                .reason = "Crate Object recipes failed; loading continues with partial content",
            });
        }
    }
    ThingFactory::instance().finalizeDerivedMetadata();
    // In RefCode UpgradeCenter is initialized after ThingFactory. Keep that
    // dependency order even though UpgradeCatalog itself is value-only: later
    // production/object-upgrade stages can then resolve one complete frozen
    // directory against the already compiled recipe universe.
    ok &= loadOptionalUpgradeCatalog(contentFingerprint, m_upgradeCatalog);

    // FileSystem creates the immutable multiplayer snapshot before UI and
    // game startup.  Capture a shared frozen handle here so a running session
    // does not query the mutable UI facade or a process-global singleton.
    m_ruleset = MultiplayerData::instance().rulesetSnapshot();
    if (!m_ruleset) {
        auto emptyRuleset = std::make_shared<engine::MultiplayerRuleset>();
        emptyRuleset->sealEmpty();
        m_ruleset = std::move(emptyRuleset);
        processContentDiagnostics().warn({
            .source = "data/ini/Multiplayer+PlayerTemplate",
            .block = "MultiplayerRuleset",
            .module = "MultiplayerData",
            .adoptedValue = "sealed empty ruleset",
            .reason = "immutable multiplayer rules could not be loaded; no factions or colors were fabricated",
        });
    } else if (m_ruleset->factionTemplates().empty()) {
        processContentDiagnostics().warn({
            .source = "data/ini/Multiplayer+PlayerTemplate",
            .block = "MultiplayerRuleset",
            .module = "MultiplayerData",
            .adoptedValue = "sealed empty ruleset",
            .reason = "multiplayer/player-template content produced no factions; session setup will reject unavailable factions safely",
        });
    }

    if (!ok || ThingFactory::instance().all().empty()) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "GameData",
            .module = "ThingFactory",
            .adoptedValue = "partial/empty game content",
            .reason = !ok
                ? "one or more authored catalogs degraded during load"
                : "no Object recipes were compiled",
        });
    }

    auto pristineBones = std::make_shared<W3dPristineBoneCatalog>();
    container::String pristineBoneError;
    if (!pristineBones->build(
            ThingFactory::instance(), &pristineBoneError)) {
        processContentDiagnostics().warn({
            .source = "GameDataLoader",
            .block = "W3D",
            .module = "W3dPristineBoneCatalog",
            .rawValue = pristineBoneError,
            .adoptedValue = "empty/partial pristine-bone catalog",
            .reason = "W3D pristine-bone catalog could not be fully compiled",
        });
    }
    for (const container::String& diagnostic : pristineBones->diagnostics()) {
        TD_LOG_WARN("[GameDataLoader] Pristine bone: {}", diagnostic);
    }
    // W3D files consulted by authoritative bone placement are simulation
    // input. The catalog hashes canonical path, missing state and exact bytes.
    contentFingerprint.u64(pristineBones->sourceFingerprint());
    m_pristineBoneCatalog = std::move(pristineBones);

    // The frozen multiplayer rules are gameplay input too.  Fold their
    // simulation-only identity in after the INI source stream, so UI/audio
    // differences retained by MultiplayerRuleset::contentFingerprint do not
    // affect lockstep or replay compatibility.
    contentFingerprint.u64(m_ruleset->simulationFingerprint());
    m_simulationContentFingerprint = contentFingerprint.finish();
    m_loaded = true;
    processContentDiagnostics().logSummary();
    TD_LOG_INFO("[GameDataLoader] All game data loaded successfully (simulation-content={:016X})",
                m_simulationContentFingerprint);
    return true;
}

} // namespace game
