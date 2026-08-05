#pragma once

#include "core/container/container_types.h"

// Scenario-owned detached, loss-aware source representation for Script data embedded in
// legacy Generals/Zero Hour CkMp map files.  This layer intentionally does
// not know about live entities, commands, or script opcode semantics.  The
// compiler/runtime layer owns those decisions after it has an authoritative
// ECS and command boundary.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
namespace engine::script::legacy {

// A byte range in LegacyMapScriptSource::ckMpBytes().  Ranges point at the
// *uncompressed* CkMp representation, which remains owned by the immutable
// source object for lossless inspection of unknown data.
struct LegacySourceRange final {
    uint64_t offset = 0;
    uint64_t size = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return size == 0; }
};

enum class LegacyScriptDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error,
};

struct LegacyScriptDiagnostic final {
    LegacyScriptDiagnosticSeverity severity = LegacyScriptDiagnosticSeverity::Info;
    container::String message;
    uint64_t offset = 0;
    // Slash-separated labels, for example
    // "SidesList/PlayerScriptsList/ScriptList/Script".
    container::String chunkPath;
};

// The CkMp symbol table maps compact on-disk IDs to chunk labels and NameKey
// internal names. IDs are map-local; never compare them across maps.
struct LegacyCkMpSymbol final {
    uint32_t id = 0;
    container::String name;
};

// RefCode serializes NameKey as `(symbolId << 8) | dictionaryType`.  Scripts
// use the same storage for their internal action/condition names.  Preserve
// every component so a later compiler can safely prefer the internal name to
// an obsolete/reordered numeric opcode.
struct LegacyNameKey final {
    uint32_t rawValue = 0;
    uint32_t symbolId = 0;
    uint8_t typeTag = 0;
    container::String resolvedName;
};

using LegacyDictionaryValue = std::variant<std::monostate, bool, int32_t, float,
                                           container::String, container::U16String>;

struct LegacyDictionaryEntry final {
    LegacyNameKey key;
    LegacyDictionaryValue value;
    LegacySourceRange serialized;
};

// A whole CkMp data chunk retained verbatim by range.  The parser retains
// chunks it does not interpret, including unknown chunks nested under a
// known script container.  `serialized` includes the 10-byte chunk header;
// `payload` starts immediately after it.
struct LegacyCkMpChunk final {
    uint32_t symbolId = 0;
    container::String label;
    uint16_t version = 0;
    LegacySourceRange serialized;
    LegacySourceRange payload;
    container::String parentPath;
};

// Bytes within a known parent that cannot be represented as a valid nested
// CkMp chunk (for example a malformed/truncated tail).  Their ranges remain
// available through rawBytes(), so recovery tooling does not lose evidence.
struct LegacyOpaqueData final {
    LegacySourceRange serialized;
    container::String parentPath;
    container::String reason;
};

// `Parameter::WriteParameter` has two wire shapes.  COORD3D (type 16 in the
// original enum) is three floats; all other parameter types are
// int32 + float + length-prefixed ASCII text.  Keep both values and the raw
// range instead of attempting any semantic conversion in the source parser.
struct LegacyScriptParameter final {
    int32_t type = 0;
    bool isCoordinate = false;
    int32_t integerValue = 0;
    float realValue = 0.0f;
    container::String text;
    container::Array<float, 3> coordinate{};
    LegacySourceRange serialized;
};

// Shared raw representation for Condition, ScriptAction, and
// ScriptActionFalse.  `opcode` is exactly the serialized signed 32-bit enum
// value.  It is deliberately not translated to a modern enum here.
struct LegacyScriptInstructionSource final {
    int32_t opcode = 0;
    std::optional<LegacyNameKey> nameKey;
    container::Vector<LegacyScriptParameter> parameters;
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    // Non-empty when the opcode parameter list could not be fully decoded.
    LegacySourceRange trailingData;
};

struct LegacyOrConditionSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<LegacyScriptInstructionSource> conditions;
};

struct LegacyScriptSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::String name;
    container::String comment;
    container::String conditionComment;
    container::String actionComment;
    bool active = true;
    bool oneShot = false;
    bool easy = true;
    bool normal = true;
    bool hard = true;
    bool subroutine = false;
    int32_t delayEvaluationSeconds = 0;
    // Original semantics: OR across clauses, AND within each clause.
    container::Vector<LegacyOrConditionSource> conditions;
    container::Vector<LegacyScriptInstructionSource> actions;
    container::Vector<LegacyScriptInstructionSource> falseActions;
};

struct LegacyScriptGroupSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::String name;
    bool active = true;
    bool subroutine = false;
    container::Vector<LegacyScriptSource> scripts;
};

struct LegacyScriptListSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<LegacyScriptSource> scripts;
    container::Vector<LegacyScriptGroupSource> groups;
};

struct LegacyPlayerScriptsListSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<LegacyScriptListSource> playerLists;
};

struct LegacyBuildListEntrySource final {
    container::String buildingName;
    container::String templateName;
    container::Array<float, 3> position{};
    float angle = 0.0f;
    bool initiallyBuilt = false;
    int32_t rebuildCount = 0;
    // SidesList v3 additions.
    container::String script;
    int32_t health = 0;
    bool whiner = false;
    bool unsellable = false;
    bool repairable = false;
    LegacySourceRange serialized;
};

struct LegacySideSource final {
    container::Vector<LegacyDictionaryEntry> properties;
    container::Vector<LegacyBuildListEntrySource> buildList;
};

struct LegacyTeamSource final {
    container::Vector<LegacyDictionaryEntry> properties;
};

// A standalone compiled script file (`.scb`, for example
// `Data\Scripts\SkirmishScripts.scb`) is the *same* CkMp chunk stream a `.map`
// uses for its script data and is stored uncompressed.  The only difference is
// its top level: instead of one `SidesList` wrapper it carries
// `PlayerScriptsList`, `ScriptsPlayers` and `ScriptTeams` as siblings.
// `ScriptsPlayers` names the owning player of each `ScriptList` inside the
// sibling `PlayerScriptsList` positionally, so the graft matches by name and
// never by index -- a skirmish map orders its Sides differently than the
// shipped `.scb` orders its names.
struct LegacyScriptPlayerNamesSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<container::String> names;
    // Version 2 optionally interleaves one Side dictionary per name.  RefCode
    // reads and discards them; retain them so the stream stays lossless.
    container::Vector<container::Vector<LegacyDictionaryEntry>> playerProperties;
    LegacySourceRange trailingData;
};

// A standalone `ScriptTeams` chunk is a bare sequence of Team dictionaries,
// with no count prefix: the reader consumes dictionaries until the chunk ends.
struct LegacyScriptTeamsSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<LegacyTeamSource> teams;
    LegacySourceRange trailingData;
};

struct LegacySidesListSource final {
    uint16_t sourceVersion = 0;
    LegacySourceRange serialized;
    container::Vector<LegacySideSource> sides;
    container::Vector<LegacyTeamSource> teams;
    // Indices into LegacyMapScriptSource::playerScriptLists(). One legacy
    // SidesList normally has exactly one nested PlayerScriptsList.
    container::Vector<uint32_t> playerScriptsListIndices;
    LegacySourceRange trailingData;
};

class LegacyMapScriptSourceBuilder;
class LegacySkirmishSourceBuilder;

// Immutable owning source object.  LegacyMapScriptParser only exposes it as
// shared_ptr<const LegacyMapScriptSource>; no live game state or parser state
// is retained here.
class LegacyMapScriptSource final {
public:
    [[nodiscard]] container::Span<const uint8_t> ckMpBytes() const noexcept;
    [[nodiscard]] container::Span<const uint8_t> rawBytes(LegacySourceRange range) const noexcept;
    [[nodiscard]] container::Span<const LegacyCkMpSymbol> symbols() const noexcept;
    [[nodiscard]] container::Span<const LegacySidesListSource> sidesLists() const noexcept;
    [[nodiscard]] container::Span<const LegacyPlayerScriptsListSource> playerScriptLists() const noexcept;
    // Non-empty only for a standalone `.scb` stream. A `.map` keeps its Side
    // names and Team records inside SidesList instead.
    [[nodiscard]] container::Span<const LegacyScriptPlayerNamesSource> scriptPlayerNameSets() const noexcept;
    [[nodiscard]] container::Span<const LegacyScriptTeamsSource> scriptTeamSets() const noexcept;
    [[nodiscard]] container::Span<const LegacyCkMpChunk> unknownChunks() const noexcept;
    [[nodiscard]] container::Span<const LegacyOpaqueData> opaqueData() const noexcept;

    [[nodiscard]] std::optional<container::StringView> symbolName(uint32_t symbolId) const noexcept;

private:
    container::Vector<uint8_t> m_ckMpBytes;
    container::Vector<LegacyCkMpSymbol> m_symbols;
    container::Vector<LegacySidesListSource> m_sidesLists;
    container::Vector<LegacyPlayerScriptsListSource> m_playerScriptLists;
    container::Vector<LegacyScriptPlayerNamesSource> m_scriptPlayerNameSets;
    container::Vector<LegacyScriptTeamsSource> m_scriptTeamSets;
    container::Vector<LegacyCkMpChunk> m_unknownChunks;
    container::Vector<LegacyOpaqueData> m_opaqueData;

    friend class LegacyMapScriptSourceBuilder;
    // Composes a map source and a standalone `.scb` source into one immutable
    // source; see LegacySkirmishScriptSource.h.
    friend class LegacySkirmishSourceBuilder;
};

// Conservative limits for untrusted map data.  Callers may lower them for
// tools; values of zero are treated as invalid parser configuration.
struct LegacyMapScriptParseOptions final {
    size_t maxInputBytes = 256ull * 1024ull * 1024ull;
    size_t maxSymbols = 65536;
    size_t maxChunks = 131072;
    size_t maxSides = 64;
    // Script teams are not multiplayer sides. Campaign maps commonly have
    // dozens of them, so do not apply maxSides here.
    size_t maxTeams = 250000;
    size_t maxBuildEntries = 250000;
    size_t maxPlayerScriptLists = 64;
    size_t maxScriptListsPerPlayerSet = 64;
    // A standalone `.scb` names one owning player per ScriptList. RefCode
    // stops at MAX_PLAYER_COUNT; keep the same order of magnitude.
    size_t maxScriptPlayerNameSets = 8;
    size_t maxScriptTeamSets = 8;
    size_t maxScripts = 200000;
    size_t maxGroups = 100000;
    size_t maxConditionsPerScript = 4096;
    size_t maxActionsPerScript = 4096;
    size_t maxParametersPerInstruction = 256;
};

struct LegacyMapScriptParseResult final {
    container::SharedPtr<const LegacyMapScriptSource> source;
    container::Vector<LegacyScriptDiagnostic> diagnostics;
    // `complete` is true only when the CkMp stream was parsed without an
    // error-level diagnostic. Warnings (such as retained unknown chunks) do
    // not make a source incomplete.
    bool complete = false;

    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return source != nullptr && complete; }
};

class LegacyMapScriptParser final {
public:
    explicit LegacyMapScriptParser(LegacyMapScriptParseOptions options = {}) noexcept;

    // Accepts either an uncompressed CkMp byte stream or the original
    // EA-compressed map representation used by shipped .map files.
    [[nodiscard]] LegacyMapScriptParseResult parse(container::Span<const uint8_t> bytes) const;

    // First resolves the path through the project's VFS, then falls back to a
    // direct local file read. This makes both game assets and standalone map
    // tooling use the same source parser.
    [[nodiscard]] LegacyMapScriptParseResult parseFile(container::StringView path) const;

    [[nodiscard]] const LegacyMapScriptParseOptions& options() const noexcept { return m_options; }

private:
    LegacyMapScriptParseOptions m_options;
};

} // namespace engine::script::legacy
