#pragma once

#include "core/container/container_types.h"

#include "game/scenario/source/LegacyMapScriptSource.h"
#include "game/script/runtime/ScriptProgram.h"

#include <cstddef>
#include <cstdint>
namespace engine::script::legacy {

enum class LegacyScriptCompileDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error,
};

struct LegacyScriptCompileDiagnostic final {
    LegacyScriptCompileDiagnosticSeverity severity = LegacyScriptCompileDiagnosticSeverity::Info;
    container::String message;
    LegacySourceRange source;
};

// The original engine measures the script frame timer in LOGICFRAMES_PER_SECOND.
// GameSession supplies its fixed simulation rate so a second-based map field
// is compiled to the same confirmed-tick domain as ScriptRuntime.
struct LegacyScriptCompileOptions final {
    uint32_t logicFramesPerSecond = 30;
    // Production follows the content-degradation contract: one malformed or
    // unknown Action becomes a diagnosed NO_OP and one Condition becomes a
    // diagnosed false value, while later instructions in the same Script
    // remain executable. Tools may opt into whole-Script blocking when they
    // need a strict coverage audit. The raw source is retained either way.
    bool blockScriptsWithUnsupportedInstructions = false;
};

struct LegacyScriptCompileResult final {
    container::SharedPtr<const ScriptProgram> program;
    container::Vector<LegacyScriptCompileDiagnostic> diagnostics;
    size_t sourceScriptCount = 0;
    size_t runnableScriptCount = 0;
    size_t blockedScriptCount = 0;

    [[nodiscard]] bool hasErrors() const noexcept;
    // A non-null Program has passed ScriptProgramBuilder validation and is
    // safe to install. Error diagnostics may describe isolated source data
    // that was omitted; they do not invalidate the verified remainder.
    [[nodiscard]] explicit operator bool() const noexcept {
        return program != nullptr;
    }
};

// Converts the loss-aware legacy source representation to the small, fully
// executable ScriptProgram subset. It preserves original list/group/script
// order and uses serialized NameKeys before numeric opcode fallbacks. The
// Unsupported source is never silent: production degrades at instruction
// granularity, while strict audit callers may request whole-Script blocking.
class LegacyScriptCompiler final {
public:
    explicit LegacyScriptCompiler(LegacyScriptCompileOptions options = {}) noexcept;

    [[nodiscard]] LegacyScriptCompileResult compile(const LegacyMapScriptSource& source) const;
    [[nodiscard]] const LegacyScriptCompileOptions& options() const noexcept { return m_options; }

private:
    LegacyScriptCompileOptions m_options;
};

} // namespace engine::script::legacy
