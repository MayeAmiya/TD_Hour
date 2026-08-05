#pragma once

#include "core/container/container_types.h"

#include <cstddef>

namespace engine::text {

// Presentation/input-side equivalent of the shipped LanguageFilter.  The
// encrypted word list is optional content; an absent or malformed file leaves
// text unchanged instead of making command ingress depend on locale assets.
class LanguageFilter final {
public:
    static constexpr container::StringView DefaultWordListPath = "langdata.dat";

    [[nodiscard]] static LanguageFilter& instance();

    [[nodiscard]] bool reloadFromVfs(
        container::StringView path = DefaultWordListPath);
    // Optional locale content is loaded only when the local player first
    // submits beacon text. A failed attempt remains a deterministic
    // passthrough instead of rescanning VFS on every edit.
    [[nodiscard]] bool ensureLoaded(
        container::StringView path = DefaultWordListPath);
    void clear() noexcept;

    [[nodiscard]] container::String filterLine(
        container::StringView utf8) const;
    [[nodiscard]] size_t wordCount() const noexcept { return m_words.size(); }

private:
    container::TreeSet<container::U32String> m_words;
    bool m_loadAttempted = false;
};

} // namespace engine::text
