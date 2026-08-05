#include "core/container/hash_containers.h"
#include "game/scenario/source/LegacyMapScriptSource.h"

#include "core/compression/runtime/manager.h"
#include "VFS.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <utility>

namespace engine::script::legacy {
namespace {

constexpr size_t kCkMpChunkHeaderSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int32_t);
constexpr int32_t kLegacyCoordinateParameterType = 16; // Parameter::COORD3D in Scripts.h.
constexpr uint8_t kDictionaryBool = 0;
constexpr uint8_t kDictionaryInt = 1;
constexpr uint8_t kDictionaryReal = 2;
constexpr uint8_t kDictionaryAsciiString = 3;
constexpr uint8_t kDictionaryUnicodeString = 4;

[[nodiscard]] container::String unknownLabel(uint32_t symbolId) {
    return "#" + std::to_string(symbolId);
}

[[nodiscard]] container::String appendPath(container::StringView parent, container::StringView child) {
    if (parent.empty()) return container::String(child);
    container::String result;
    result.reserve(parent.size() + 1 + child.size());
    result.append(parent);
    result.push_back('/');
    result.append(child);
    return result;
}

class ByteCursor final {
public:
    ByteCursor(container::Span<const uint8_t> bytes, size_t begin, size_t end) noexcept
        : m_bytes(bytes), m_begin(begin), m_position(begin), m_end(end) {}

    [[nodiscard]] size_t begin() const noexcept { return m_begin; }
    [[nodiscard]] size_t position() const noexcept { return m_position; }
    [[nodiscard]] size_t end() const noexcept { return m_end; }
    [[nodiscard]] size_t remaining() const noexcept { return m_end - m_position; }
    [[nodiscard]] bool atEnd() const noexcept { return m_position == m_end; }

    [[nodiscard]] bool advance(size_t count) noexcept {
        if (count > remaining()) return false;
        m_position += count;
        return true;
    }

    [[nodiscard]] bool readU8(uint8_t& value) noexcept {
        if (remaining() < sizeof(uint8_t)) return false;
        value = m_bytes[m_position++];
        return true;
    }

    [[nodiscard]] bool readU16(uint16_t& value) noexcept {
        if (remaining() < sizeof(uint16_t)) return false;
        const uint8_t* data = m_bytes.data() + m_position;
        value = static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8U);
        m_position += sizeof(uint16_t);
        return true;
    }

    [[nodiscard]] bool readU32(uint32_t& value) noexcept {
        if (remaining() < sizeof(uint32_t)) return false;
        const uint8_t* data = m_bytes.data() + m_position;
        value = static_cast<uint32_t>(data[0]) |
                (static_cast<uint32_t>(data[1]) << 8U) |
                (static_cast<uint32_t>(data[2]) << 16U) |
                (static_cast<uint32_t>(data[3]) << 24U);
        m_position += sizeof(uint32_t);
        return true;
    }

    [[nodiscard]] bool readI32(int32_t& value) noexcept {
        uint32_t raw = 0;
        if (!readU32(raw)) return false;
        value = static_cast<int32_t>(raw);
        return true;
    }

    [[nodiscard]] bool readFloat(float& value) noexcept {
        uint32_t raw = 0;
        if (!readU32(raw)) return false;
        static_assert(sizeof(raw) == sizeof(value));
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

    [[nodiscard]] bool readAsciiString(container::String& value) {
        uint16_t length = 0;
        if (!readU16(length) || remaining() < length) return false;
        value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_position), length);
        m_position += length;
        return true;
    }

    [[nodiscard]] bool readUtf16String(container::U16String& value) {
        uint16_t length = 0;
        if (!readU16(length) || static_cast<size_t>(length) > remaining() / sizeof(uint16_t)) {
            return false;
        }
        value.resize(length);
        for (char16_t& character : value) {
            uint16_t codeUnit = 0;
            if (!readU16(codeUnit)) return false;
            character = static_cast<char16_t>(codeUnit);
        }
        return true;
    }

private:
    container::Span<const uint8_t> m_bytes;
    size_t m_begin = 0;
    size_t m_position = 0;
    size_t m_end = 0;
};

struct ChunkView final {
    uint32_t symbolId = 0;
    container::String label;
    uint16_t version = 0;
    LegacySourceRange serialized;
    LegacySourceRange payload;
};

[[nodiscard]] LegacySourceRange rangeFrom(size_t start, size_t end) noexcept {
    return {
        .offset = static_cast<uint64_t>(start),
        .size = static_cast<uint64_t>(end - start),
    };
}

[[nodiscard]] bool optionsAreValid(const LegacyMapScriptParseOptions& options) noexcept {
    return options.maxInputBytes > 0 &&
           options.maxInputBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()) &&
           options.maxSymbols > 0 && options.maxChunks > 0 && options.maxSides > 0 &&
           options.maxTeams > 0 && options.maxBuildEntries > 0 && options.maxPlayerScriptLists > 0 &&
           options.maxScriptListsPerPlayerSet > 0 && options.maxScripts > 0 &&
           options.maxGroups > 0 && options.maxConditionsPerScript > 0 &&
           options.maxActionsPerScript > 0 && options.maxParametersPerInstruction > 0 &&
           options.maxScriptPlayerNameSets > 0 && options.maxScriptTeamSets > 0;
}

} // namespace

class LegacyMapScriptSourceBuilder final {
public:
    explicit LegacyMapScriptSourceBuilder(const LegacyMapScriptParseOptions& options)
        : m_options(options), m_source(std::make_shared<LegacyMapScriptSource>()) {}

    [[nodiscard]] LegacyMapScriptParseResult parse(container::Span<const uint8_t> input) {
        if (!optionsAreValid(m_options)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid LegacyMapScriptParser limits", 0, {});
            return finish();
        }
        if (input.empty() || input.size() > m_options.maxInputBytes ||
            input.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid or oversized map byte stream", 0, {});
            return finish();
        }

        try {
            container::Vector<uint8_t> decoded;
            if (compression::manager::is_data_compressed(input.data(), static_cast<int32_t>(input.size()))) {
                const int32_t decodedSize = compression::manager::uncompressed_size(
                    input.data(), static_cast<int32_t>(input.size()));
                if (decodedSize <= 0 || static_cast<size_t>(decodedSize) > m_options.maxInputBytes) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Invalid or oversized compressed map header", 0, {});
                    return finish();
                }
                decoded.resize(static_cast<size_t>(decodedSize));
                const int32_t actualSize = compression::manager::decompress(
                    decoded.data(), decodedSize, input.data(), static_cast<int32_t>(input.size()));
                if (actualSize != decodedSize) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Failed to decompress map byte stream", 0, {});
                    return finish();
                }
            } else {
                decoded.assign(input.begin(), input.end());
            }

            m_source->m_ckMpBytes = std::move(decoded);
            if (m_source->m_ckMpBytes.empty()) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Map byte stream became empty after decompression", 0, {});
                return finish();
            }
            parseCkMp();
        } catch (const std::bad_alloc&) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Legacy script source parsing ran out of memory", 0, {});
        }
        return finish();
    }

private:
    [[nodiscard]] container::Span<const uint8_t> bytes() const noexcept {
        return m_source->m_ckMpBytes;
    }

    void addDiagnostic(LegacyScriptDiagnosticSeverity severity,
                       container::String message,
                       size_t offset,
                       container::StringView path) {
        if (severity == LegacyScriptDiagnosticSeverity::Error) m_hasErrors = true;
        m_diagnostics.push_back({
            .severity = severity,
            .message = std::move(message),
            .offset = static_cast<uint64_t>(offset),
            .chunkPath = container::String(path),
        });
    }

    [[nodiscard]] container::String labelFor(uint32_t symbolId) const {
        if (const auto found = m_symbolIndices.find(symbolId); found != m_symbolIndices.end()) {
            return m_source->m_symbols[found->second].name;
        }
        return unknownLabel(symbolId);
    }

    [[nodiscard]] LegacyNameKey decodeNameKey(uint32_t raw) const {
        LegacyNameKey result;
        result.rawValue = raw;
        result.symbolId = raw >> 8U;
        result.typeTag = static_cast<uint8_t>(raw & 0xffU);
        if (const auto found = m_symbolIndices.find(result.symbolId); found != m_symbolIndices.end()) {
            result.resolvedName = m_source->m_symbols[found->second].name;
        }
        return result;
    }

    [[nodiscard]] bool readChunk(ByteCursor& cursor, ChunkView& output, container::StringView parentPath) {
        const size_t start = cursor.position();
        if (cursor.remaining() < kCkMpChunkHeaderSize) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated CkMp nested chunk header", start, parentPath);
            return false;
        }
        if (m_chunkCount >= m_options.maxChunks) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "CkMp chunk limit exceeded", start, parentPath);
            return false;
        }

        ByteCursor probe = cursor;
        uint32_t symbolId = 0;
        uint16_t version = 0;
        int32_t payloadSize = 0;
        if (!probe.readU32(symbolId) || !probe.readU16(version) || !probe.readI32(payloadSize) ||
            payloadSize < 0 || static_cast<size_t>(payloadSize) > probe.remaining()) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid CkMp nested chunk size", start, parentPath);
            return false;
        }

        const size_t payloadStart = probe.position();
        if (!probe.advance(static_cast<size_t>(payloadSize))) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated CkMp nested chunk payload", start, parentPath);
            return false;
        }
        cursor = probe;
        ++m_chunkCount;
        output = {
            .symbolId = symbolId,
            .label = labelFor(symbolId),
            .version = version,
            .serialized = rangeFrom(start, probe.position()),
            .payload = rangeFrom(payloadStart, probe.position()),
        };
        return true;
    }

    void retainUnknownChunk(const ChunkView& chunk, container::StringView parentPath) {
        m_source->m_unknownChunks.push_back({
            .symbolId = chunk.symbolId,
            .label = chunk.label,
            .version = chunk.version,
            .serialized = chunk.serialized,
            .payload = chunk.payload,
            .parentPath = container::String(parentPath),
        });
        addDiagnostic(LegacyScriptDiagnosticSeverity::Warning,
                      "Retained unsupported CkMp chunk '" + chunk.label + "'",
                       static_cast<size_t>(chunk.serialized.offset), parentPath);
    }

    // CkMp maps contain terrain, lighting, object and editor chunks alongside
    // SidesList. They are not unknown to the game, merely outside this
    // script-source reader's ownership. Retain their byte ranges for tooling
    // without reporting every normal map chunk as a script warning.
    void retainNonScriptChunk(const ChunkView& chunk, container::StringView parentPath) {
        m_source->m_unknownChunks.push_back({
            .symbolId = chunk.symbolId,
            .label = chunk.label,
            .version = chunk.version,
            .serialized = chunk.serialized,
            .payload = chunk.payload,
            .parentPath = container::String(parentPath),
        });
    }

    [[nodiscard]] static bool isScriptRelatedTopLevelLabel(container::StringView label) noexcept {
        constexpr container::StringView prefix = "script";
        if (label.size() < prefix.size()) return false;
        for (size_t index = 0; index < prefix.size(); ++index) {
            const char value = label[index];
            const char lower = value >= 'A' && value <= 'Z'
                ? static_cast<char>(value + ('a' - 'A')) : value;
            if (lower != prefix[index]) return false;
        }
        return true;
    }

    void retainOpaqueTail(ByteCursor& cursor,
                          container::StringView parentPath,
                          container::StringView reason,
                          LegacySourceRange* output = nullptr) {
        const LegacySourceRange tail = rangeFrom(cursor.position(), cursor.end());
        if (!tail.empty()) {
            m_source->m_opaqueData.push_back({
                .serialized = tail,
                .parentPath = container::String(parentPath),
                .reason = container::String(reason),
            });
        }
        if (output) *output = tail;
        static_cast<void>(cursor.advance(cursor.remaining()));
    }

    template <typename Handler>
    bool visitChildren(ByteCursor& cursor,
                       container::StringView parentPath,
                       LegacySourceRange* malformedTail,
                       Handler&& handler) {
        while (!cursor.atEnd()) {
            ChunkView child;
            if (!readChunk(cursor, child, parentPath)) {
                retainOpaqueTail(cursor, parentPath, "Malformed nested CkMp chunk sequence", malformedTail);
                return false;
            }
            handler(child, appendPath(parentPath, child.label));
        }
        return true;
    }

    [[nodiscard]] bool readDictionary(ByteCursor& cursor,
                                      container::Vector<LegacyDictionaryEntry>& output,
                                      container::StringView path) {
        uint16_t count = 0;
        const size_t start = cursor.position();
        if (!cursor.readU16(count)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated legacy dictionary count", start, path);
            return false;
        }
        // Every dictionary record needs a key/type word and at least one byte
        // of value, regardless of its type.
        if (static_cast<size_t>(count) > cursor.remaining() / (sizeof(uint32_t) + 1U)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid legacy dictionary count", start, path);
            return false;
        }
        output.clear();
        output.reserve(count);
        for (uint16_t index = 0; index < count; ++index) {
            const size_t entryStart = cursor.position();
            uint32_t keyAndType = 0;
            if (!cursor.readU32(keyAndType)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated legacy dictionary key", entryStart, path);
                return false;
            }

            LegacyDictionaryEntry entry;
            entry.key = decodeNameKey(keyAndType);
            switch (entry.key.typeTag) {
            case kDictionaryBool: {
                uint8_t value = 0;
                if (!cursor.readU8(value)) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Truncated boolean legacy dictionary value", entryStart, path);
                    return false;
                }
                entry.value = value != 0;
                break;
            }
            case kDictionaryInt: {
                int32_t value = 0;
                if (!cursor.readI32(value)) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Truncated integer legacy dictionary value", entryStart, path);
                    return false;
                }
                entry.value = value;
                break;
            }
            case kDictionaryReal: {
                float value = 0.0f;
                if (!cursor.readFloat(value)) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Truncated real legacy dictionary value", entryStart, path);
                    return false;
                }
                entry.value = value;
                break;
            }
            case kDictionaryAsciiString: {
                container::String value;
                if (!cursor.readAsciiString(value)) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Truncated ASCII legacy dictionary value", entryStart, path);
                    return false;
                }
                entry.value = std::move(value);
                break;
            }
            case kDictionaryUnicodeString: {
                container::U16String value;
                if (!cursor.readUtf16String(value)) {
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Truncated Unicode legacy dictionary value", entryStart, path);
                    return false;
                }
                entry.value = std::move(value);
                break;
            }
            default:
                // CkMp dictionaries do not prefix unknown value types with a
                // byte length, so continuing would desynchronize SidesList.
                entry.value = std::monostate{};
                entry.serialized = rangeFrom(entryStart, cursor.position());
                output.push_back(std::move(entry));
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Unsupported legacy dictionary value type", entryStart, path);
                return false;
            }
            entry.serialized = rangeFrom(entryStart, cursor.position());
            output.push_back(std::move(entry));
        }
        return true;
    }

    [[nodiscard]] bool readBuildEntry(ByteCursor& cursor,
                                      uint16_t sidesVersion,
                                      LegacyBuildListEntrySource& output,
                                      container::StringView path) {
        const size_t start = cursor.position();
        uint8_t initiallyBuilt = 0;
        if (!cursor.readAsciiString(output.buildingName) ||
            !cursor.readAsciiString(output.templateName) ||
            !cursor.readFloat(output.position[0]) ||
            !cursor.readFloat(output.position[1]) ||
            !cursor.readFloat(output.position[2]) ||
            !cursor.readFloat(output.angle) ||
            !cursor.readU8(initiallyBuilt) ||
            !cursor.readI32(output.rebuildCount)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated SidesList build entry", start, path);
            return false;
        }
        output.initiallyBuilt = initiallyBuilt != 0;
        if (sidesVersion >= 3) {
            uint8_t whiner = 0;
            uint8_t unsellable = 0;
            uint8_t repairable = 0;
            if (!cursor.readAsciiString(output.script) || !cursor.readI32(output.health) ||
                !cursor.readU8(whiner) || !cursor.readU8(unsellable) || !cursor.readU8(repairable)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated SidesList v3 build entry", start, path);
                return false;
            }
            output.whiner = whiner != 0;
            output.unsellable = unsellable != 0;
            output.repairable = repairable != 0;
        }
        output.serialized = rangeFrom(start, cursor.position());
        return true;
    }

    [[nodiscard]] bool readParameter(ByteCursor& cursor,
                                     LegacyScriptParameter& output,
                                     container::StringView path) {
        const size_t start = cursor.position();
        if (!cursor.readI32(output.type)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated script parameter type", start, path);
            return false;
        }
        output.isCoordinate = output.type == kLegacyCoordinateParameterType;
        if (output.isCoordinate) {
            if (!cursor.readFloat(output.coordinate[0]) ||
                !cursor.readFloat(output.coordinate[1]) ||
                !cursor.readFloat(output.coordinate[2])) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated COORD3D script parameter", start, path);
                return false;
            }
        } else if (!cursor.readI32(output.integerValue) || !cursor.readFloat(output.realValue) ||
                   !cursor.readAsciiString(output.text)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated scalar script parameter", start, path);
            return false;
        }
        output.serialized = rangeFrom(start, cursor.position());
        return true;
    }

    [[nodiscard]] LegacyScriptInstructionSource parseInstruction(const ChunkView& chunk,
                                                                  bool isCondition,
                                                                  container::StringView path) {
        LegacyScriptInstructionSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        const size_t start = cursor.position();
        if (!cursor.readI32(result.opcode)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated script opcode", start, path);
            result.trailingData = rangeFrom(start, cursor.end());
            return result;
        }

        const bool hasNameKey = isCondition ? chunk.version >= 4 : chunk.version >= 2;
        if (hasNameKey) {
            uint32_t rawNameKey = 0;
            if (!cursor.readU32(rawNameKey)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated script internal NameKey", cursor.position(), path);
                result.trailingData = rangeFrom(cursor.position(), cursor.end());
                return result;
            }
            result.nameKey = decodeNameKey(rawNameKey);
            if (result.nameKey->resolvedName.empty()) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Warning,
                              "Script internal NameKey references an unknown CkMp symbol",
                              cursor.position() - sizeof(uint32_t), path);
            }
        }

        int32_t parameterCount = 0;
        if (!cursor.readI32(parameterCount) || parameterCount < 0) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid script parameter count", cursor.position(), path);
            result.trailingData = rangeFrom(cursor.position(), cursor.end());
            return result;
        }
        if (static_cast<size_t>(parameterCount) > m_options.maxParametersPerInstruction ||
            static_cast<size_t>(parameterCount) > cursor.remaining() / 14U) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Script parameter count exceeds safe bounds", cursor.position(), path);
            result.trailingData = rangeFrom(cursor.position(), cursor.end());
            return result;
        }
        result.parameters.reserve(static_cast<size_t>(parameterCount));
        for (int32_t index = 0; index < parameterCount; ++index) {
            LegacyScriptParameter parameter;
            if (!readParameter(cursor, parameter, path)) {
                result.trailingData = rangeFrom(cursor.position(), cursor.end());
                return result;
            }
            result.parameters.push_back(std::move(parameter));
        }
        if (!cursor.atEnd()) {
            result.trailingData = rangeFrom(cursor.position(), cursor.end());
            m_source->m_opaqueData.push_back({
                .serialized = result.trailingData,
                .parentPath = container::String(path),
                .reason = "Trailing data after legacy script instruction",
            });
            addDiagnostic(LegacyScriptDiagnosticSeverity::Warning,
                          "Retained trailing data after legacy script instruction",
                          cursor.position(), path);
        }
        return result;
    }

    [[nodiscard]] LegacyOrConditionSource parseOrCondition(const ChunkView& chunk,
                                                            container::StringView path,
                                                            size_t& conditionCount) {
        LegacyOrConditionSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        visitChildren(cursor, path, nullptr, [&](const ChunkView& child, const container::String& childPath) {
            if (child.label != "Condition") {
                retainUnknownChunk(child, path);
                return;
            }
            if (conditionCount >= m_options.maxConditionsPerScript) {
                retainUnknownChunk(child, path);
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Script condition limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                return;
            }
            ++conditionCount;
            result.conditions.push_back(parseInstruction(child, true, childPath));
        });
        return result;
    }

    [[nodiscard]] LegacyScriptSource parseScript(const ChunkView& chunk, container::StringView path) {
        LegacyScriptSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        const size_t start = cursor.position();
        uint8_t active = 0;
        uint8_t oneShot = 0;
        uint8_t easy = 0;
        uint8_t normal = 0;
        uint8_t hard = 0;
        uint8_t subroutine = 0;
        if (!cursor.readAsciiString(result.name) || !cursor.readAsciiString(result.comment) ||
            !cursor.readAsciiString(result.conditionComment) ||
            !cursor.readAsciiString(result.actionComment) || !cursor.readU8(active) ||
            !cursor.readU8(oneShot) || !cursor.readU8(easy) || !cursor.readU8(normal) ||
            !cursor.readU8(hard) || !cursor.readU8(subroutine)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated Script header", start, path);
            retainOpaqueTail(cursor, path, "Truncated Script header");
            return result;
        }
        result.active = active != 0;
        result.oneShot = oneShot != 0;
        result.easy = easy != 0;
        result.normal = normal != 0;
        result.hard = hard != 0;
        result.subroutine = subroutine != 0;
        if (chunk.version >= 2 && !cursor.readI32(result.delayEvaluationSeconds)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated Script v2 delay", cursor.position(), path);
            retainOpaqueTail(cursor, path, "Truncated Script v2 delay");
            return result;
        }

        size_t conditionCount = 0;
        size_t actionCount = 0;
        visitChildren(cursor, path, nullptr, [&](const ChunkView& child, const container::String& childPath) {
            if (child.label == "OrCondition") {
                result.conditions.push_back(parseOrCondition(child, childPath, conditionCount));
            } else if (child.label == "ScriptAction" || child.label == "ScriptActionFalse") {
                if (actionCount >= m_options.maxActionsPerScript) {
                    retainUnknownChunk(child, path);
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Script action limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                    return;
                }
                ++actionCount;
                LegacyScriptInstructionSource action = parseInstruction(child, false, childPath);
                if (child.label == "ScriptAction") {
                    result.actions.push_back(std::move(action));
                } else {
                    result.falseActions.push_back(std::move(action));
                }
            } else {
                retainUnknownChunk(child, path);
            }
        });
        return result;
    }

    [[nodiscard]] LegacyScriptGroupSource parseScriptGroup(const ChunkView& chunk, container::StringView path) {
        LegacyScriptGroupSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        const size_t start = cursor.position();
        uint8_t active = 0;
        uint8_t subroutine = 0;
        if (!cursor.readAsciiString(result.name) || !cursor.readU8(active) ||
            (chunk.version >= 2 && !cursor.readU8(subroutine))) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated ScriptGroup header", start, path);
            retainOpaqueTail(cursor, path, "Truncated ScriptGroup header");
            return result;
        }
        result.active = active != 0;
        result.subroutine = subroutine != 0;
        visitChildren(cursor, path, nullptr, [&](const ChunkView& child, const container::String& childPath) {
            if (child.label != "Script") {
                retainUnknownChunk(child, path);
                return;
            }
            if (m_scriptCount >= m_options.maxScripts) {
                retainUnknownChunk(child, path);
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Total Script limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                return;
            }
            ++m_scriptCount;
            result.scripts.push_back(parseScript(child, childPath));
        });
        return result;
    }

    [[nodiscard]] LegacyScriptListSource parseScriptList(const ChunkView& chunk, container::StringView path) {
        LegacyScriptListSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        visitChildren(cursor, path, nullptr, [&](const ChunkView& child, const container::String& childPath) {
            if (child.label == "Script") {
                if (m_scriptCount >= m_options.maxScripts) {
                    retainUnknownChunk(child, path);
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "Total Script limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                    return;
                }
                ++m_scriptCount;
                result.scripts.push_back(parseScript(child, childPath));
            } else if (child.label == "ScriptGroup") {
                if (m_groupCount >= m_options.maxGroups) {
                    retainUnknownChunk(child, path);
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "ScriptGroup limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                    return;
                }
                ++m_groupCount;
                result.groups.push_back(parseScriptGroup(child, childPath));
            } else {
                retainUnknownChunk(child, path);
            }
        });
        return result;
    }

    [[nodiscard]] uint32_t parsePlayerScriptsList(const ChunkView& chunk, container::StringView path) {
        LegacyPlayerScriptsListSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        visitChildren(cursor, path, nullptr, [&](const ChunkView& child, const container::String& childPath) {
            if (child.label != "ScriptList") {
                retainUnknownChunk(child, path);
                return;
            }
            if (result.playerLists.size() >= m_options.maxScriptListsPerPlayerSet) {
                retainUnknownChunk(child, path);
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "PlayerScriptsList ScriptList limit exceeded",
                              static_cast<size_t>(child.serialized.offset), path);
                return;
            }
            result.playerLists.push_back(parseScriptList(child, childPath));
        });
        const uint32_t index = static_cast<uint32_t>(m_source->m_playerScriptLists.size());
        m_source->m_playerScriptLists.push_back(std::move(result));
        return index;
    }

    // RefCode SidesList.cpp ParsePlayersDataChunk. Version 2 prefixes an
    // int that says whether one Side dictionary follows each name.
    void parseScriptsPlayers(const ChunkView& chunk, container::StringView path) {
        LegacyScriptPlayerNamesSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        int32_t readDictionaries = 0;
        if (chunk.version >= 2 && !cursor.readI32(readDictionaries)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated ScriptsPlayers dictionary flag", cursor.position(), path);
            retainOpaqueTail(cursor, path, "Truncated ScriptsPlayers dictionary flag",
                             &result.trailingData);
            m_source->m_scriptPlayerNameSets.push_back(std::move(result));
            return;
        }
        const size_t countOffset = cursor.position();
        int32_t nameCount = 0;
        if (!cursor.readI32(nameCount) || nameCount < 0 ||
            static_cast<size_t>(nameCount) > m_options.maxSides ||
            static_cast<size_t>(nameCount) > cursor.remaining() / sizeof(uint16_t)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid ScriptsPlayers name count", countOffset, path);
            retainOpaqueTail(cursor, path, "Invalid ScriptsPlayers name count", &result.trailingData);
            m_source->m_scriptPlayerNameSets.push_back(std::move(result));
            return;
        }
        result.names.reserve(static_cast<size_t>(nameCount));
        for (int32_t index = 0; index < nameCount; ++index) {
            container::String name;
            if (!cursor.readAsciiString(name)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated ScriptsPlayers name", cursor.position(), path);
                retainOpaqueTail(cursor, path, "Truncated ScriptsPlayers name", &result.trailingData);
                m_source->m_scriptPlayerNameSets.push_back(std::move(result));
                return;
            }
            result.names.push_back(std::move(name));
            if (readDictionaries == 0) continue;
            container::Vector<LegacyDictionaryEntry> properties;
            if (!readDictionary(cursor, properties, path)) {
                retainOpaqueTail(cursor, path, "Malformed ScriptsPlayers Side dictionary",
                                 &result.trailingData);
                m_source->m_scriptPlayerNameSets.push_back(std::move(result));
                return;
            }
            result.playerProperties.push_back(std::move(properties));
        }
        if (!cursor.atEnd()) {
            retainOpaqueTail(cursor, path, "Trailing data after ScriptsPlayers names",
                             &result.trailingData);
            addDiagnostic(LegacyScriptDiagnosticSeverity::Warning,
                          "Retained trailing data after ScriptsPlayers names",
                          static_cast<size_t>(result.trailingData.offset), path);
        }
        m_source->m_scriptPlayerNameSets.push_back(std::move(result));
    }

    // RefCode SidesList.cpp ParseTeamsDataChunk: a bare, uncounted sequence of
    // Team dictionaries read until the chunk ends.
    void parseScriptTeams(const ChunkView& chunk, container::StringView path) {
        LegacyScriptTeamsSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        while (!cursor.atEnd()) {
            if (result.teams.size() >= m_options.maxTeams) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "ScriptTeams team limit exceeded", cursor.position(), path);
                retainOpaqueTail(cursor, path, "ScriptTeams team limit exceeded", &result.trailingData);
                break;
            }
            LegacyTeamSource team;
            if (!readDictionary(cursor, team.properties, path)) {
                retainOpaqueTail(cursor, path, "Malformed ScriptTeams team dictionary",
                                 &result.trailingData);
                break;
            }
            result.teams.push_back(std::move(team));
        }
        m_source->m_scriptTeamSets.push_back(std::move(result));
    }

    void parseSidesList(const ChunkView& chunk, container::StringView path) {
        LegacySidesListSource result;
        result.sourceVersion = chunk.version;
        result.serialized = chunk.serialized;
        ByteCursor cursor(bytes(), static_cast<size_t>(chunk.payload.offset),
                          static_cast<size_t>(chunk.payload.offset + chunk.payload.size));
        const size_t start = cursor.position();
        int32_t sideCount = 0;
        if (!cursor.readI32(sideCount) || sideCount < 0 ||
            static_cast<size_t>(sideCount) > m_options.maxSides) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid SidesList side count", start, path);
            retainOpaqueTail(cursor, path, "Invalid SidesList side count", &result.trailingData);
            m_source->m_sidesLists.push_back(std::move(result));
            return;
        }
        result.sides.reserve(static_cast<size_t>(sideCount));
        for (int32_t sideIndex = 0; sideIndex < sideCount; ++sideIndex) {
            LegacySideSource side;
            if (!readDictionary(cursor, side.properties, path)) {
                retainOpaqueTail(cursor, path, "Malformed SidesList side dictionary", &result.trailingData);
                m_source->m_sidesLists.push_back(std::move(result));
                return;
            }
            const size_t buildCountOffset = cursor.position();
            int32_t buildCount = 0;
            if (!cursor.readI32(buildCount) || buildCount < 0 ||
                static_cast<size_t>(buildCount) > m_options.maxBuildEntries) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Invalid SidesList build entry count", buildCountOffset, path);
                retainOpaqueTail(cursor, path, "Invalid SidesList build entry count", &result.trailingData);
                m_source->m_sidesLists.push_back(std::move(result));
                return;
            }
            side.buildList.reserve(static_cast<size_t>(buildCount));
            for (int32_t buildIndex = 0; buildIndex < buildCount; ++buildIndex) {
                LegacyBuildListEntrySource entry;
                if (!readBuildEntry(cursor, chunk.version, entry, path)) {
                    retainOpaqueTail(cursor, path, "Malformed SidesList build entry", &result.trailingData);
                    m_source->m_sidesLists.push_back(std::move(result));
                    return;
                }
                side.buildList.push_back(std::move(entry));
            }
            result.sides.push_back(std::move(side));
        }

        if (chunk.version >= 2) {
            const size_t teamCountOffset = cursor.position();
            int32_t teamCount = 0;
            if (!cursor.readI32(teamCount) || teamCount < 0 ||
                static_cast<size_t>(teamCount) > m_options.maxTeams ||
                static_cast<size_t>(teamCount) > cursor.remaining() / sizeof(uint16_t)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Invalid SidesList team count", teamCountOffset, path);
                retainOpaqueTail(cursor, path, "Invalid SidesList team count", &result.trailingData);
                m_source->m_sidesLists.push_back(std::move(result));
                return;
            }
            result.teams.reserve(static_cast<size_t>(teamCount));
            for (int32_t teamIndex = 0; teamIndex < teamCount; ++teamIndex) {
                LegacyTeamSource team;
                if (!readDictionary(cursor, team.properties, path)) {
                    retainOpaqueTail(cursor, path, "Malformed SidesList team dictionary", &result.trailingData);
                    m_source->m_sidesLists.push_back(std::move(result));
                    return;
                }
                result.teams.push_back(std::move(team));
            }
        }

        visitChildren(cursor, path, &result.trailingData,
                      [&](const ChunkView& child, const container::String& childPath) {
            if (child.label != "PlayerScriptsList") {
                retainUnknownChunk(child, path);
                return;
            }
            if (m_source->m_playerScriptLists.size() >= m_options.maxPlayerScriptLists) {
                retainUnknownChunk(child, path);
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "PlayerScriptsList limit exceeded", static_cast<size_t>(child.serialized.offset), path);
                return;
            }
            result.playerScriptsListIndices.push_back(parsePlayerScriptsList(child, childPath));
        });
        m_source->m_sidesLists.push_back(std::move(result));
    }

    void parseCkMp() {
        ByteCursor cursor(bytes(), 0, bytes().size());
        const size_t start = cursor.position();
        uint8_t signature[4]{};
        for (uint8_t& byte : signature) {
            if (!cursor.readU8(byte)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated CkMp header", start, {});
                return;
            }
        }
        int32_t symbolCount = 0;
        if (std::memcmp(signature, "CkMp", sizeof(signature)) != 0 || !cursor.readI32(symbolCount) ||
            symbolCount < 0 || static_cast<size_t>(symbolCount) > m_options.maxSymbols) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Invalid CkMp header or symbol count", start, {});
            return;
        }
        if (static_cast<size_t>(symbolCount) > cursor.remaining() / (sizeof(uint32_t) + 1U)) {
            addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                          "Truncated CkMp symbol table", cursor.position(), {});
            return;
        }
        m_source->m_symbols.reserve(static_cast<size_t>(symbolCount));
        m_symbolIndices.reserve(static_cast<size_t>(symbolCount));
        for (int32_t index = 0; index < symbolCount; ++index) {
            const size_t symbolOffset = cursor.position();
            uint8_t nameLength = 0;
            uint32_t symbolId = 0;
            if (!cursor.readU8(nameLength) || cursor.remaining() < nameLength) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated CkMp symbol name", symbolOffset, {});
                return;
            }
            container::String name(reinterpret_cast<const char*>(bytes().data() + cursor.position()), nameLength);
            static_cast<void>(cursor.advance(nameLength));
            if (!cursor.readU32(symbolId)) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Truncated CkMp symbol ID", symbolOffset, {});
                return;
            }
            const size_t sourceIndex = m_source->m_symbols.size();
            if (!m_symbolIndices.emplace(symbolId, sourceIndex).second) {
                addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                              "Duplicate CkMp symbol ID", symbolOffset, {});
                return;
            }
            m_source->m_symbols.push_back({.id = symbolId, .name = std::move(name)});
        }

        visitChildren(cursor, {}, nullptr, [&](const ChunkView& chunk, const container::String& path) {
            if (chunk.label == "SidesList") {
                parseSidesList(chunk, path);
            } else if (chunk.label == "PlayerScriptsList") {
                if (m_source->m_playerScriptLists.size() >= m_options.maxPlayerScriptLists) {
                    retainUnknownChunk(chunk, {});
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "PlayerScriptsList limit exceeded", static_cast<size_t>(chunk.serialized.offset), {});
                    return;
                }
                static_cast<void>(parsePlayerScriptsList(chunk, path));
            } else if (chunk.label == "ScriptsPlayers") {
                if (m_source->m_scriptPlayerNameSets.size() >= m_options.maxScriptPlayerNameSets) {
                    retainUnknownChunk(chunk, {});
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "ScriptsPlayers limit exceeded",
                                  static_cast<size_t>(chunk.serialized.offset), {});
                    return;
                }
                parseScriptsPlayers(chunk, path);
            } else if (chunk.label == "ScriptTeams") {
                if (m_source->m_scriptTeamSets.size() >= m_options.maxScriptTeamSets) {
                    retainUnknownChunk(chunk, {});
                    addDiagnostic(LegacyScriptDiagnosticSeverity::Error,
                                  "ScriptTeams limit exceeded",
                                  static_cast<size_t>(chunk.serialized.offset), {});
                    return;
                }
                parseScriptTeams(chunk, path);
            } else if (isScriptRelatedTopLevelLabel(chunk.label)) {
                retainUnknownChunk(chunk, {});
            } else {
                retainNonScriptChunk(chunk, {});
            }
        });
    }

    [[nodiscard]] LegacyMapScriptParseResult finish() {
        LegacyMapScriptParseResult result;
        result.source = std::move(m_source);
        result.diagnostics = std::move(m_diagnostics);
        result.complete = !m_hasErrors;
        return result;
    }

    LegacyMapScriptParseOptions m_options;
    container::SharedPtr<LegacyMapScriptSource> m_source;
    container::Vector<LegacyScriptDiagnostic> m_diagnostics;
    container::HashMap<uint32_t, size_t> m_symbolIndices;
    size_t m_chunkCount = 0;
    size_t m_scriptCount = 0;
    size_t m_groupCount = 0;
    bool m_hasErrors = false;
};

container::Span<const uint8_t> LegacyMapScriptSource::ckMpBytes() const noexcept {
    return m_ckMpBytes;
}

container::Span<const uint8_t> LegacyMapScriptSource::rawBytes(LegacySourceRange range) const noexcept {
    if (range.offset > m_ckMpBytes.size() || range.size > m_ckMpBytes.size() - range.offset) {
        return {};
    }
    return container::Span<const uint8_t>(m_ckMpBytes).subspan(static_cast<size_t>(range.offset),
                                                          static_cast<size_t>(range.size));
}

container::Span<const LegacyCkMpSymbol> LegacyMapScriptSource::symbols() const noexcept {
    return m_symbols;
}

container::Span<const LegacySidesListSource> LegacyMapScriptSource::sidesLists() const noexcept {
    return m_sidesLists;
}

container::Span<const LegacyPlayerScriptsListSource> LegacyMapScriptSource::playerScriptLists() const noexcept {
    return m_playerScriptLists;
}

container::Span<const LegacyScriptPlayerNamesSource>
LegacyMapScriptSource::scriptPlayerNameSets() const noexcept {
    return m_scriptPlayerNameSets;
}

container::Span<const LegacyScriptTeamsSource>
LegacyMapScriptSource::scriptTeamSets() const noexcept {
    return m_scriptTeamSets;
}

container::Span<const LegacyCkMpChunk> LegacyMapScriptSource::unknownChunks() const noexcept {
    return m_unknownChunks;
}

container::Span<const LegacyOpaqueData> LegacyMapScriptSource::opaqueData() const noexcept {
    return m_opaqueData;
}

std::optional<container::StringView> LegacyMapScriptSource::symbolName(uint32_t symbolId) const noexcept {
    const auto found = std::find_if(m_symbols.begin(), m_symbols.end(),
                                    [symbolId](const LegacyCkMpSymbol& symbol) {
                                        return symbol.id == symbolId;
                                    });
    if (found == m_symbols.end()) return std::nullopt;
    return found->name;
}

bool LegacyMapScriptParseResult::hasErrors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const LegacyScriptDiagnostic& diagnostic) {
        return diagnostic.severity == LegacyScriptDiagnosticSeverity::Error;
    });
}

LegacyMapScriptParser::LegacyMapScriptParser(LegacyMapScriptParseOptions options) noexcept
    : m_options(options) {}

LegacyMapScriptParseResult LegacyMapScriptParser::parse(container::Span<const uint8_t> bytes) const {
    LegacyMapScriptSourceBuilder builder(m_options);
    return builder.parse(bytes);
}

LegacyMapScriptParseResult LegacyMapScriptParser::parseFile(container::StringView path) const {
    container::Vector<uint8_t> bytes;
    try {
        if (io::VFS::instance().readToBuffer(path, bytes)) {
            return parse(bytes);
        }

        std::ifstream file(container::String(path), std::ios::binary | std::ios::ate);
        if (!file) {
            LegacyMapScriptParseResult result;
            result.diagnostics.push_back({
                .severity = LegacyScriptDiagnosticSeverity::Error,
                .message = "Failed to read legacy map script source: " + container::String(path),
                .offset = 0,
                .chunkPath = {},
            });
            return result;
        }
        const std::streamoff length = file.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > m_options.maxInputBytes) {
            LegacyMapScriptParseResult result;
            result.diagnostics.push_back({
                .severity = LegacyScriptDiagnosticSeverity::Error,
                .message = "Legacy map script source has an invalid file size",
                .offset = 0,
                .chunkPath = {},
            });
            return result;
        }
        bytes.resize(static_cast<size_t>(length));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
            LegacyMapScriptParseResult result;
            result.diagnostics.push_back({
                .severity = LegacyScriptDiagnosticSeverity::Error,
                .message = "Failed while reading legacy map script source: " + container::String(path),
                .offset = 0,
                .chunkPath = {},
            });
            return result;
        }
        return parse(bytes);
    } catch (const std::bad_alloc&) {
        LegacyMapScriptParseResult result;
        result.diagnostics.push_back({
            .severity = LegacyScriptDiagnosticSeverity::Error,
            .message = "Legacy map script source loading ran out of memory",
            .offset = 0,
            .chunkPath = {},
        });
        return result;
    }
}

} // namespace engine::script::legacy
