#pragma once

#include "core/container/container_types.h"

#include <cstdint>
namespace engine {

// Immutable gameplay definition compiled from Science.ini. The legacy science
// name is case-sensitive identity: `SCIENCE_FOO` and `science_foo` are
// distinct authored NameKey values.
struct ScienceDefinition final {
    container::String name;
    container::Vector<container::String> prerequisiteSciences;
    // Transitive prerequisite roots, derived after the complete override set
    // is sealed. ZH uses these to hide branches belonging to another chosen
    // general while still showing currently unaffordable descendants.
    container::Vector<container::String> rootSciences;
    // Zero means the science cannot be purchased.  Negative authored values
    // are rejected by the purchase transaction instead of becoming a point
    // duplication exploit through the old signed subtraction path.
    int32_t purchasePointCost = 0;
    bool grantable = true;

    // Presentation metadata. RefCode ScienceInfo::m_name / m_description,
    // authored as `DisplayName` / `Description` in Science.ini and always
    // written as string-table labels ("SCIENCE:USAPaladin",
    // "CONTROLBAR:ToolTipUSASciencePaladin"). RefCode uses
    // INI::parseAndTranslateLabel, i.e. it resolves the label to display text
    // at parse time; this project keeps the authored label and resolves it in
    // the presentation layer through StringTable, so these strings
    // deliberately do not participate in the simulation fingerprint.
    container::String displayNameLabel;
    container::String descriptionLabel;
};

// A sealed Science.ini projection.  GameDataLoader creates one shared
// instance for a content load and GameContentSnapshot retains that immutable
// handle for an active session, so scripts never consult a mutable global
// ScienceStore after match startup.
class ScienceCatalog final {
public:
    // Legacy INI::loadFileDirectory(root) loads root + ".ini" first, then
    // all `root/*.ini` fragments (direct files before recursive children) in
    // stable lexical order.  The caller supplies roots in authored priority
    // order, e.g. Default/Science before Science.
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfs(container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                        container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                                  container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] uint64_t simulationFingerprint() const noexcept {
        return m_simulationFingerprint;
    }
    [[nodiscard]] const ScienceDefinition* find(container::StringView name) const;
    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }

private:
    struct Entry final {
        container::String name;
        ScienceDefinition definition;
    };

    [[nodiscard]] static uint64_t calculateFingerprint(const container::Vector<Entry>& entries);
    [[nodiscard]] bool loadFromVfsFilesImpl(
        const container::Vector<container::String>& logicalFiles,
        bool resetCatalog, container::String* error);

    container::Vector<Entry> m_entries;
    uint64_t m_simulationFingerprint = 0;
    bool m_loaded = false;
};

} // namespace engine
