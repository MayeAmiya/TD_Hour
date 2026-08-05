#include "LegacyScriptCompiler.h"
#include "LegacyScriptCompilerInternal.h"

#include <algorithm>

namespace engine::script::legacy
{

bool LegacyScriptCompileResult::hasErrors() const noexcept
{
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const LegacyScriptCompileDiagnostic& diagnostic)
        {
            return diagnostic.severity ==
                LegacyScriptCompileDiagnosticSeverity::Error;
        });
}

LegacyScriptCompiler::LegacyScriptCompiler(
    LegacyScriptCompileOptions options) noexcept
    : m_options(options)
{
    if (m_options.logicFramesPerSecond == 0)
        m_options.logicFramesPerSecond = 1;
}

LegacyScriptCompileResult LegacyScriptCompiler::compile(
    const LegacyMapScriptSource& source) const
{
    return detail::compileLegacyScript(source, m_options);
}

} // namespace engine::script::legacy
