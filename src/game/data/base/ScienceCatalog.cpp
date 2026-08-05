#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "ScienceCatalog.h"

#include "ContentDiagnostics.h"
#include "LegacyIniDirectory.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr auto trim = container::trimAsciiCopy;

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String result = trim(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] container::String canonicalPath(container::StringView value) {
    container::String result = canonical(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

[[nodiscard]] bool equalInsensitive(container::StringView left, container::StringView right) {
    return canonical(left) == canonical(right);
}

[[nodiscard]] bool parseInt32(container::StringView value, int32_t& output) {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    int32_t parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool parseBool(container::StringView value, bool& output) {
    const container::String normalized = canonical(value);
    if (normalized == "yes" || normalized == "true" || normalized == "on" || normalized == "1") {
        output = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "off" || normalized == "0") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] container::Vector<container::String> scienceNames(container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) || value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) && value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) result.emplace_back(value.substr(begin, cursor - begin));
    }

    // Content authors use `None` as the empty ScienceVec spelling.  Treat it
    // as a sentinel only when it is the entire value so a malformed mixed
    // value remains observable rather than silently discarding real names.
    if (result.size() == 1 && equalInsensitive(result.front(), "None")) result.clear();
    return result;
}

void normalizePrerequisites(container::Vector<container::String>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void warnScienceContent(
    container::StringView source, container::StringView definition,
    container::StringView field, container::StringView rawValue,
    container::StringView adoptedValue, container::String reason) {
    game::processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "Science",
        .definition = container::String{definition},
        .module = "ScienceCatalog",
        .field = container::String{field},
        .rawValue = container::String{rawValue},
        .adoptedValue = container::String{adoptedValue},
        .reason = std::move(reason),
    });
}

[[nodiscard]] container::String layerSource(
    container::StringView path, size_t layerIndex) {
    return container::String{path} + " [layer " +
        std::to_string(layerIndex) + "]";
}

class CanonicalHasher final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }

    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }

    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

} // namespace

uint64_t ScienceCatalog::calculateFingerprint(const container::Vector<Entry>& entries) {
    CanonicalHasher hash;
    hash.string("ScienceCatalog.simulation");
    hash.u32(static_cast<uint32_t>(entries.size()));
    for (const Entry& entry : entries) {
        hash.string(entry.name);
        hash.i32(entry.definition.purchasePointCost);
        hash.boolean(entry.definition.grantable);
        hash.u32(static_cast<uint32_t>(entry.definition.prerequisiteSciences.size()));
        for (const container::String& prerequisite : entry.definition.prerequisiteSciences) {
            hash.string(prerequisite);
        }
    }
    return hash.finish();
}

container::Vector<container::String>
ScienceCatalog::enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots) {
    return game::ini::enumerateLegacyIniDirectories(loadRoots);
}

bool ScienceCatalog::loadFromVfs(container::StringView path, container::String* error) {
    return loadFromVfsFiles(container::Vector<container::String>{container::String{path}}, error);
}

bool ScienceCatalog::applyOverridesFromVfs(
    container::StringView path, container::String* error) {
    return loadFromVfsFilesImpl(
        container::Vector<container::String>{container::String{path}},
        false, error);
}

bool ScienceCatalog::loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                                container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool ScienceCatalog::loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                      container::String* error) {
    return loadFromVfsFilesImpl(logicalFiles, true, error);
}

bool ScienceCatalog::loadFromVfsFilesImpl(
    const container::Vector<container::String>& logicalFiles,
    bool resetCatalog, container::String* error) {
    if (error) error->clear();
    if (resetCatalog) clear();
    else if (!m_loaded) {
        if (error) *error = "Science override requires a loaded base catalog";
        return false;
    }

    // A missing authored Science directory degrades to a sealed empty
    // catalog. References are diagnosed when the immutable session snapshot
    // is assembled; fabricating sciences here would be less compatible than
    // the original null/invalid behavior.
    if (logicalFiles.empty()) {
        m_simulationFingerprint = calculateFingerprint(m_entries);
        m_loaded = true;
        return true;
    }

    struct ScienceVersion final {
        Entry entry;
        container::String source;
    };
    struct ScienceHistory final {
        container::String name;
        container::Vector<ScienceVersion> versions;
    };
    container::Vector<ScienceHistory> histories;
    histories.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        ScienceHistory history;
        history.name = entry.name;
        history.versions.push_back({
            .entry = entry,
            .source = "previous loaded Science catalog",
        });
        histories.push_back(std::move(history));
    }

    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty()) {
            warnScienceContent(
                __FILE__, {}, "LogicalPath", rawPath, "input skipped",
                "Science INI input set contains an empty logical path");
            continue;
        }
        if (!parsedFiles.insert(path).second) continue;

        if (!io::VFS::instance().exists(path)) {
            warnScienceContent(
                path, {}, "File", path, "input skipped",
                "Science INI disappeared from VFS during load");
            continue;
        }
        const container::Vector<container::String> layers{
            io::VFS::instance().readAll(path)};
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            const container::String source = layerSource(path, layerIndex);
            game::GeneralsIniParser parser;
            if (!parser.parse(layers[layerIndex], source)) {
                warnScienceContent(
                    source, {}, "IniLayer", {}, "layer skipped",
                    "could not parse Science INI layer");
                continue;
            }

            // A duplicate Science declaration in one physical source layer
            // has no deterministic legacy meaning. Later files/layers remain
            // valid overrides, but same-layer duplicates are rejected rather
            // than silently depending on parser traversal accidents.
            container::HashSet<container::String> definitionsInLayer;
            for (const game::IniBlock& block : parser.blocks()) {
                if (!equalInsensitive(block.type, "Science")) continue;
                const auto diagnosticScope =
                    game::contentDiagnosticProvenanceScope(block.source);

                const container::String name = trim(block.name);
                if (name.empty()) {
                    warnScienceContent(
                        source, {}, "Name", block.name, "block skipped",
                        "Science block has an empty definition name");
                    continue;
                }
                if (!definitionsInLayer.insert(name).second) {
                    warnScienceContent(
                        source, name, "Name", block.name,
                        "duplicate block skipped",
                        "duplicate Science declaration in one physical INI layer");
                    continue;
                }

                const auto found = std::find_if(
                    histories.begin(), histories.end(),
                    [&name](const ScienceHistory& history) {
                        return history.name == name;
                    });
                const bool createsDefinition = found == histories.end();
                Entry candidate = createsDefinition
                    ? Entry{
                        .name = name,
                        .definition = {.name = name},
                    }
                    : found->versions.back().entry;
                bool validDefinition = true;

                // Omitted fields retain a lower-priority source value. A
                // malformed recognized field rejects only this declaration;
                // parsing a candidate keeps the prior definition atomic.
                for (size_t valueIndex = 0;
                     valueIndex < block.values.size(); ++valueIndex) {
                    const auto& [fieldName, rawValue] =
                        block.values[valueIndex];
                    const auto fieldDiagnosticScope =
                        game::contentDiagnosticProvenanceScope(
                            block.valueSource(valueIndex));
                    const container::String field = canonical(fieldName);
                    if (field == "prerequisitesciences") {
                        candidate.definition.prerequisiteSciences = scienceNames(rawValue);
                        normalizePrerequisites(candidate.definition.prerequisiteSciences);
                    } else if (field == "sciencepurchasepointcost") {
                        int32_t cost = 0;
                        if (!parseInt32(rawValue, cost)) {
                            warnScienceContent(
                                source, name, fieldName, rawValue,
                                createsDefinition ? "block skipped; no definition created"
                                                  : "block skipped; retained prior definition",
                                "invalid signed 32-bit SciencePurchasePointCost");
                            validDefinition = false;
                            break;
                        }
                        candidate.definition.purchasePointCost = cost;
                    } else if (field == "isgrantable") {
                        bool grantable = true;
                        if (!parseBool(rawValue, grantable)) {
                            warnScienceContent(
                                source, name, fieldName, rawValue,
                                createsDefinition ? "block skipped; no definition created"
                                                  : "block skipped; retained prior definition",
                                "invalid boolean IsGrantable");
                            validDefinition = false;
                            break;
                        }
                        candidate.definition.grantable = grantable;
                    } else if (field == "displayname") {
                        // Shipped Science.ini spells these exactly
                        // `DisplayName` and `Description` (84 of 96 blocks
                        // author both) and both values are string-table
                        // labels, never literal display text. An empty value
                        // is a real authored clear, so no rejection here.
                        candidate.definition.displayNameLabel = trim(rawValue);
                    } else if (field == "description") {
                        candidate.definition.descriptionLabel = trim(rawValue);
                    }
                }

                if (!validDefinition) continue;
                if (createsDefinition) {
                    ScienceHistory history;
                    history.name = name;
                    history.versions.push_back({
                        .entry = std::move(candidate),
                        .source = source,
                    });
                    histories.push_back(std::move(history));
                } else {
                    found->versions.push_back({
                        .entry = std::move(candidate),
                        .source = source,
                    });
                }
            }
        }
    }

    // A semantic failure in a high-priority override exposes the preceding
    // successful version of that definition. Repeating to a fixed point also
    // handles dependants when an invalid definition has no prior version.
    for (;;) {
        container::HashSet<container::String> knownNames;
        knownNames.reserve(histories.size());
        for (const ScienceHistory& history : histories) {
            if (!history.versions.empty()) knownNames.insert(history.name);
        }

        container::Vector<size_t> invalidHistories;
        for (size_t index = 0; index < histories.size(); ++index) {
            const ScienceHistory& history = histories[index];
            if (history.versions.empty()) continue;
            const ScienceVersion& version = history.versions.back();
            const Entry& entry = version.entry;
            for (const container::String& prerequisite :
                 entry.definition.prerequisiteSciences) {
                if (knownNames.contains(prerequisite)) continue;
                warnScienceContent(
                    version.source, entry.name,
                    "PrerequisiteSciences", prerequisite,
                    history.versions.size() > 1
                        ? "invalid override skipped; lower-priority definition retained"
                        : "definition skipped",
                    "unknown prerequisite Science reference");
                invalidHistories.push_back(index);
                break;
            }
        }
        if (invalidHistories.empty()) break;
        for (const size_t index : invalidHistories) {
            histories[index].versions.pop_back();
        }
    }

    m_entries.clear();
    m_entries.reserve(histories.size());
    for (ScienceHistory& history : histories) {
        if (!history.versions.empty()) {
            m_entries.push_back(std::move(history.versions.back().entry));
        }
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& left, const Entry& right) {
        return left.name < right.name;
    });
    const auto findEntry = [this](container::StringView name)
        -> const Entry* {
        const auto found = std::lower_bound(
            m_entries.begin(), m_entries.end(), name,
            [](const Entry& entry, container::StringView wanted) {
                return container::StringView{entry.name} < wanted;
            });
        return found != m_entries.end() && found->name == name
            ? &*found : nullptr;
    };
    for (Entry& entry : m_entries) {
        entry.definition.rootSciences.clear();
        container::Vector<container::String> visiting;
        std::function<bool(container::StringView)> collectRoots =
            [&](container::StringView name) {
                if (std::find(visiting.begin(), visiting.end(), name) !=
                    visiting.end()) {
                    return false;
                }
                const Entry* current = findEntry(name);
                if (!current) return false;
                if (current->definition.prerequisiteSciences.empty()) {
                    entry.definition.rootSciences.emplace_back(name);
                    return true;
                }
                visiting.emplace_back(name);
                bool valid = true;
                for (const container::String& prerequisite :
                     current->definition.prerequisiteSciences) {
                    valid = collectRoots(prerequisite) && valid;
                }
                visiting.pop_back();
                return valid;
            };
        if (!collectRoots(entry.name)) {
            entry.definition.rootSciences.clear();
            warnScienceContent(
                "sealed Science catalog", entry.name,
                "PrerequisiteSciences", {}, "root branch hidden",
                "cyclic prerequisite Science chain");
        }
        normalizePrerequisites(entry.definition.rootSciences);
    }
    m_simulationFingerprint = calculateFingerprint(m_entries);
    m_loaded = true;
    return true;
}

void ScienceCatalog::clear() noexcept {
    m_entries.clear();
    m_simulationFingerprint = 0;
    m_loaded = false;
}

const ScienceDefinition* ScienceCatalog::find(container::StringView name) const {
    if (!m_loaded || name.empty()) return nullptr;
    const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), name,
        [](const Entry& entry, container::StringView wanted) {
            return container::StringView{entry.name} < wanted;
        });
    return found == m_entries.end() || container::StringView{found->name} != name
        ? nullptr : &found->definition;
}

} // namespace engine
