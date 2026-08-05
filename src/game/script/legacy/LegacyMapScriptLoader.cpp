#include "core/container/container_types.h"
#include "LegacyMapScriptLoader.h"

#include <algorithm>
#include <utility>

namespace engine::script::legacy {
namespace {

constexpr size_t kMaximumRetainedDiagnostics = 1024;

[[nodiscard]] LegacyMapScriptLoadDiagnosticSeverity toLoadSeverity(
    LegacyScriptDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case LegacyScriptDiagnosticSeverity::Info: return LegacyMapScriptLoadDiagnosticSeverity::Info;
    case LegacyScriptDiagnosticSeverity::Warning: return LegacyMapScriptLoadDiagnosticSeverity::Warning;
    case LegacyScriptDiagnosticSeverity::Error: return LegacyMapScriptLoadDiagnosticSeverity::Error;
    }
    return LegacyMapScriptLoadDiagnosticSeverity::Error;
}

[[nodiscard]] LegacyMapScriptLoadDiagnosticSeverity toLoadSeverity(
    LegacyScriptCompileDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case LegacyScriptCompileDiagnosticSeverity::Info: return LegacyMapScriptLoadDiagnosticSeverity::Info;
    case LegacyScriptCompileDiagnosticSeverity::Warning: return LegacyMapScriptLoadDiagnosticSeverity::Warning;
    case LegacyScriptCompileDiagnosticSeverity::Error: return LegacyMapScriptLoadDiagnosticSeverity::Error;
    }
    return LegacyMapScriptLoadDiagnosticSeverity::Error;
}

void appendDiagnostic(LegacyMapScriptLoadReport& report,
                      LegacyMapScriptLoadDiagnostic diagnostic) {
    ++report.diagnosticCount;
    if (diagnostic.severity == LegacyMapScriptLoadDiagnosticSeverity::Error) {
        report.hasError = true;
    }
    if (report.diagnostics.size() >= kMaximumRetainedDiagnostics) {
        ++report.suppressedDiagnosticCount;
        return;
    }
    report.diagnostics.push_back(std::move(diagnostic));
}

} // namespace

LegacyMapScriptLoader::LegacyMapScriptLoader(LegacyMapScriptParseOptions parseOptions,
                                             LegacyScriptCompileOptions compileOptions) noexcept
    : m_parseOptions(std::move(parseOptions)),
      m_compileOptions(std::move(compileOptions)) {}

LegacyMapScriptLoadResult LegacyMapScriptLoader::load(container::Span<const uint8_t> mapBytes,
                                                      container::StringView sourcePath) const {
    const LegacyMapScriptParser parser(m_parseOptions);
    LegacyMapScriptParseResult parsed = parser.parse(mapBytes);
    return loadSource(parsed.source, parsed.complete, parsed.diagnostics, sourcePath);
}

LegacyMapScriptLoadResult LegacyMapScriptLoader::loadSource(
    container::SharedPtr<const LegacyMapScriptSource> source,
    bool sourceComplete,
    container::Span<const LegacyScriptDiagnostic> parseDiagnostics,
    container::StringView sourcePath) const {
    LegacyMapScriptLoadResult result;
    result.report.sourcePath = container::String(sourcePath);

    result.source = source;
    result.report.sourceParsed = source != nullptr;
    result.report.sourceComplete = sourceComplete;
    for (const LegacyScriptDiagnostic& diagnostic : parseDiagnostics) {
        appendDiagnostic(result.report, {
            .severity = toLoadSeverity(diagnostic.severity),
            .message = diagnostic.message,
            .sourceOffset = diagnostic.offset,
            .sourceSize = 0,
            .chunkPath = diagnostic.chunkPath,
        });
    }
    if (!source) return result;

    const LegacyScriptCompiler compiler(m_compileOptions);
    LegacyScriptCompileResult compiled = compiler.compile(*source);
    result.report.sourceScriptCount = compiled.sourceScriptCount;
    result.report.runnableScriptCount = compiled.runnableScriptCount;
    result.report.blockedScriptCount = compiled.blockedScriptCount;
    for (const LegacyScriptCompileDiagnostic& diagnostic : compiled.diagnostics) {
        appendDiagnostic(result.report, {
            .severity = toLoadSeverity(diagnostic.severity),
            .message = diagnostic.message,
            .sourceOffset = diagnostic.source.offset,
            .sourceSize = diagnostic.source.size,
            .chunkPath = {},
        });
    }

    if (compiled.program) {
        result.program = std::move(compiled.program);
        result.report.programProduced = true;
    }
    return result;
}

} // namespace engine::script::legacy
