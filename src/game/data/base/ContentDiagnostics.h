#pragma once

#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "debug/debug.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace game {

struct ContentDiagnosticProvenance final {
    container::StringView source;
    uint32_t sourceLine = 0;
};

inline thread_local const ContentDiagnosticProvenance*
    g_activeContentDiagnosticProvenance = nullptr;

class ContentDiagnosticProvenanceScope final {
public:
    ContentDiagnosticProvenanceScope(container::StringView source,
                                     uint32_t sourceLine)
        : m_source(source),
          m_current{.source = m_source, .sourceLine = sourceLine},
          m_previous(g_activeContentDiagnosticProvenance) {
        if (!m_source.empty()) {
            g_activeContentDiagnosticProvenance = &m_current;
        }
    }

    ~ContentDiagnosticProvenanceScope() {
        g_activeContentDiagnosticProvenance = m_previous;
    }

    ContentDiagnosticProvenanceScope(
        const ContentDiagnosticProvenanceScope&) = delete;
    ContentDiagnosticProvenanceScope& operator=(
        const ContentDiagnosticProvenanceScope&) = delete;

private:
    container::String m_source;
    ContentDiagnosticProvenance m_current;
    const ContentDiagnosticProvenance* m_previous = nullptr;
};

[[nodiscard]] inline ContentDiagnosticProvenanceScope
contentDiagnosticProvenanceScope(const IniSourceLocation& source) {
    return ContentDiagnosticProvenanceScope{source.pathView(), source.line};
}

// Authored content is deliberately compatibility-first. A malformed field,
// missing definition or absent optional catalog degrades only the affected
// behavior; the complete diagnostic remains observable even when Release
// logging is compiled out.
struct ContentDiagnostic final {
    container::String source;
    uint32_t sourceLine = 0;
    container::String block;
    container::String definition;
    container::String module;
    container::String field;
    container::String rawValue;
    container::String adoptedValue;
    container::String reason;
};

class ContentDiagnosticCollector final {
public:
    void clear() {
        const std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_keys.clear();
        m_duplicateCount = 0;
    }

    void warn(ContentDiagnostic diagnostic) {
        if (g_activeContentDiagnosticProvenance &&
            !g_activeContentDiagnosticProvenance->source.empty()) {
            diagnostic.source = container::String{
                g_activeContentDiagnosticProvenance->source};
            diagnostic.sourceLine =
                g_activeContentDiagnosticProvenance->sourceLine;
        }
        container::String key;
        const auto appendPart = [&key](container::StringView part) {
            key += std::to_string(part.size());
            key.push_back(':');
            key.append(part.data(), part.size());
            key.push_back('|');
        };
        appendPart(diagnostic.source);
        appendPart(std::to_string(diagnostic.sourceLine));
        appendPart(diagnostic.block);
        appendPart(diagnostic.definition);
        appendPart(diagnostic.module);
        appendPart(diagnostic.field);
        appendPart(diagnostic.rawValue);
        appendPart(diagnostic.adoptedValue);
        appendPart(diagnostic.reason);

        const std::lock_guard lock(m_mutex);
        if (!m_keys.insert(key).second) {
            ++m_duplicateCount;
            return;
        }

        container::String sourceLabel = diagnostic.source;
        if (diagnostic.sourceLine != 0) {
            sourceLabel.push_back(':');
            sourceLabel += std::to_string(diagnostic.sourceLine);
        }
        TD_LOG_WARN(
            "[Content] source='{}' block='{}' definition='{}' module='{}' "
            "field='{}' raw='{}' adopted='{}': {}",
            sourceLabel, diagnostic.block, diagnostic.definition,
            diagnostic.module, diagnostic.field, diagnostic.rawValue,
            diagnostic.adoptedValue, diagnostic.reason);
        m_entries.push_back(std::move(diagnostic));
    }

    [[nodiscard]] bool degraded() const noexcept {
        const std::lock_guard lock(m_mutex);
        return !m_entries.empty();
    }
    [[nodiscard]] uint64_t duplicateCount() const noexcept {
        const std::lock_guard lock(m_mutex);
        return m_duplicateCount;
    }
    [[nodiscard]] container::Vector<ContentDiagnostic> entries() const {
        const std::lock_guard lock(m_mutex);
        return m_entries;
    }

    void logSummary() const {
        const std::lock_guard lock(m_mutex);
        if (m_entries.empty()) return;
        TD_LOG_WARN(
            "[Content] load completed with {} unique warning(s); {} duplicate "
            "warning(s) suppressed",
            m_entries.size(), m_duplicateCount);
    }

private:
    container::Vector<ContentDiagnostic> m_entries;
    container::HashSet<container::String> m_keys;
    uint64_t m_duplicateCount = 0;
    mutable std::mutex m_mutex;
};

// Content recipe compilation is split across several game shards. Keeping a
// single process-load collector here avoids reverse dependencies from those
// shards back into GameDataLoader while preserving its public accessor.
[[nodiscard]] inline ContentDiagnosticCollector& processContentDiagnostics() {
    static ContentDiagnosticCollector collector;
    return collector;
}

} // namespace game
