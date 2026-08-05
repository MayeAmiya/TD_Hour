#pragma once

#include "core/container/container_types.h"

#include "game/scenario/source/LegacyMapScriptSource.h"
#include "LegacyScriptCompiler.h"

#include <cstddef>
#include <cstdint>
namespace engine::script::legacy {

// A map can contain a valid terrain stream but unsupported legacy script
// instructions.  Keep the parse/compile result structured so callers can
// distinguish a broken map from a safely-installed partial ScriptProgram.
enum class LegacyMapScriptLoadDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error,
};

struct LegacyMapScriptLoadDiagnostic final {
    LegacyMapScriptLoadDiagnosticSeverity severity = LegacyMapScriptLoadDiagnosticSeverity::Info;
    container::String message;
    uint64_t sourceOffset = 0;
    uint64_t sourceSize = 0;
    container::String chunkPath;
};

struct LegacyMapScriptLoadReport final {
    container::String sourcePath;
    bool sourceParsed = false;
    bool sourceComplete = false;
    bool programProduced = false;
    bool hasError = false;
    size_t sourceScriptCount = 0;
    size_t runnableScriptCount = 0;
    size_t blockedScriptCount = 0;
    // Diagnostic collection is intentionally bounded.  A corrupt or heavily
    // modded map must not retain an unbounded number of formatted messages in
    // its live GameSession merely because every instruction is unsupported.
    size_t diagnosticCount = 0;
    size_t suppressedDiagnosticCount = 0;
    container::Vector<LegacyMapScriptLoadDiagnostic> diagnostics;

    [[nodiscard]] bool usable() const noexcept {
        return sourceParsed && programProduced;
    }
    [[nodiscard]] bool degraded() const noexcept {
        return usable() &&
            (hasError || !sourceComplete || blockedScriptCount != 0);
    }
};

struct LegacyMapScriptLoadResult final {
    // Keep the loss-aware source alive with the session.  Later opcode work
    // can recompile it without rereading a potentially changed map file.
    container::SharedPtr<const LegacyMapScriptSource> source;
    container::SharedPtr<const ScriptProgram> program;
    LegacyMapScriptLoadReport report;
};

// Pure map-script pipeline.  It receives the exact map bytes already chosen
// by the session/terrain loader; it deliberately does not perform VFS I/O or
// mutate any ECS, renderer, audio, UI, command, or player state.
class LegacyMapScriptLoader final {
public:
    explicit LegacyMapScriptLoader(LegacyMapScriptParseOptions parseOptions = {},
                                   LegacyScriptCompileOptions compileOptions = {}) noexcept;

    [[nodiscard]] LegacyMapScriptLoadResult load(container::Span<const uint8_t> mapBytes,
                                                  container::StringView sourcePath) const;

    // Compile a source the caller already parsed, and possibly replaced.  The
    // skirmish `.scb` substitution needs this: it must graft the standalone
    // skirmish ScriptLists and Team records onto the map's sides *before* any
    // name resolution happens, because the compiler resolves Script, Group and
    // Team hook names once against the final program.  `parseDiagnostics` are
    // folded into the report so a substituted load reports what a plain load
    // would.
    [[nodiscard]] LegacyMapScriptLoadResult loadSource(
        container::SharedPtr<const LegacyMapScriptSource> source,
        bool sourceComplete,
        container::Span<const LegacyScriptDiagnostic> parseDiagnostics,
        container::StringView sourcePath) const;

    [[nodiscard]] const LegacyMapScriptParseOptions& parseOptions() const noexcept {
        return m_parseOptions;
    }
    [[nodiscard]] const LegacyScriptCompileOptions& compileOptions() const noexcept {
        return m_compileOptions;
    }

private:
    LegacyMapScriptParseOptions m_parseOptions;
    LegacyScriptCompileOptions m_compileOptions;
};

} // namespace engine::script::legacy
