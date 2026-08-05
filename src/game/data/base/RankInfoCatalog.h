#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine {

// Immutable projection of one ZH Rank block. Levels are one-based and their
// vector position is deliberately fixed at level - 1, matching RankInfoStore.
struct RankInfoDefinition final {
    uint32_t level = 0;
    container::String rankName;
    int32_t skillPointsNeeded = 0;
    container::Vector<container::String> sciencesGranted;
    uint32_t sciencePurchasePointsGranted = 0;
};

// A sealed Rank.ini directory. Base content must declare Rank 1..N in order;
// explicit map/mod overrides may only replace fields of existing levels.
// Failed compilation is reported to the caller without publishing a partial
// table, allowing GameDataLoader to disable this gameplay directory safely.
class RankInfoCatalog final {
public:
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsFiles(
        const container::Vector<container::String>& logicalFiles,
        container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(
        container::Span<const container::StringView> loadRoots,
        container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] uint64_t simulationFingerprint() const noexcept {
        return m_simulationFingerprint;
    }
    [[nodiscard]] const RankInfoDefinition* find(uint32_t level) const noexcept;
    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }

private:
    [[nodiscard]] static uint64_t calculateFingerprint(
        const container::Vector<RankInfoDefinition>& entries);
    [[nodiscard]] bool loadFromVfsFilesImpl(
        const container::Vector<container::String>& logicalFiles,
        bool overrideExisting, container::String* error);

    container::Vector<RankInfoDefinition> m_entries;
    uint64_t m_simulationFingerprint = 0;
    bool m_loaded = false;
};

} // namespace engine
