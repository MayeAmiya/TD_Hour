#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/presentation/TrackMarksRenderDescriptor.h"

#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <utility>

namespace engine {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

void appendDiagnostic(container::Vector<container::String>* diagnostics,
                      container::String message) {
    if (diagnostics) diagnostics->push_back(std::move(message));
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool parseSigned(container::StringView source, int64_t& output) noexcept {
    container::String owned{source};
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    output = static_cast<int64_t>(parsed);
    return true;
}

[[nodiscard]] bool parseBool(container::StringView source, bool& output) noexcept {
    if (equalAsciiInsensitive(source, "yes") ||
        equalAsciiInsensitive(source, "true") ||
        equalAsciiInsensitive(source, "on") || source == "1") {
        output = true;
        return true;
    }
    if (equalAsciiInsensitive(source, "no") ||
        equalAsciiInsensitive(source, "false") ||
        equalAsciiInsensitive(source, "off") || source == "0") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] uint32_t clampUnsigned(
    int64_t value, uint32_t minimum, uint32_t maximum) noexcept {
    if (value <= static_cast<int64_t>(minimum)) return minimum;
    if (value >= static_cast<int64_t>(maximum)) return maximum;
    return static_cast<uint32_t>(value);
}

[[nodiscard]] const char* lodName(TrackMarksStaticLod lod) noexcept {
    switch (lod) {
    case TrackMarksStaticLod::Low: return "LOW";
    case TrackMarksStaticLod::Medium: return "MEDIUM";
    case TrackMarksStaticLod::High: return "HIGH";
    }
    return "HIGH";
}

[[nodiscard]] bool parseLod(container::StringView authored,
                            TrackMarksStaticLod& output) noexcept {
    if (equalAsciiInsensitive(authored, "LOW")) {
        output = TrackMarksStaticLod::Low;
        return true;
    }
    if (equalAsciiInsensitive(authored, "MEDIUM")) {
        output = TrackMarksStaticLod::Medium;
        return true;
    }
    if (equalAsciiInsensitive(authored, "HIGH") ||
        equalAsciiInsensitive(authored, "VERYHIGH")) {
        output = TrackMarksStaticLod::High;
        return true;
    }
    return false;
}

void applyBudgetField(TrackMarksHistoryBudget& budget,
                      TrackMarksStaticLod lod,
                      container::StringView key,
                      container::StringView authored,
                      container::Vector<container::String>* diagnostics) {
    int64_t parsed = 0;
    if (!parseSigned(authored, parsed)) {
        appendDiagnostic(diagnostics, "StaticGameLOD " + container::String(lodName(lod)) +
            " ignored malformed " + container::String(key) + "='" +
            container::String(authored) + "'");
        return;
    }

    if (equalAsciiInsensitive(key, "MaxTankTrackEdges")) {
        const uint32_t clamped = clampUnsigned(
            parsed,
            track_marks::performance_limits::kMinimumEdgesPerStream,
            track_marks::performance_limits::kHardMaximumEdgesPerStream);
        if (parsed != static_cast<int64_t>(clamped)) {
            appendDiagnostic(diagnostics, "StaticGameLOD " + container::String(lodName(lod)) +
                " clamped MaxTankTrackEdges from " + std::to_string(parsed) +
                " to " + std::to_string(clamped));
        }
        budget.maximumEdges = clamped;
    } else if (equalAsciiInsensitive(key, "MaxTankTrackOpaqueEdges")) {
        const uint32_t clamped = clampUnsigned(
            parsed, 0,
            track_marks::performance_limits::kHardMaximumEdgesPerStream);
        if (parsed != static_cast<int64_t>(clamped)) {
            appendDiagnostic(diagnostics, "StaticGameLOD " + container::String(lodName(lod)) +
                " clamped MaxTankTrackOpaqueEdges from " + std::to_string(parsed) +
                " to " + std::to_string(clamped));
        }
        budget.opaqueEdges = clamped;
    } else if (equalAsciiInsensitive(key, "MaxTankTrackFadeDelay")) {
        const uint32_t clamped = clampUnsigned(
            parsed,
            track_marks::performance_limits::kMinimumFadeDelayMilliseconds,
            track_marks::performance_limits::kHardMaximumFadeDelayMilliseconds);
        if (parsed != static_cast<int64_t>(clamped)) {
            appendDiagnostic(diagnostics, "StaticGameLOD " + container::String(lodName(lod)) +
                " clamped MaxTankTrackFadeDelay from " + std::to_string(parsed) +
                " to " + std::to_string(clamped));
        }
        budget.fadeDelayMilliseconds = clamped;
    }
}

void normalizeBudget(TrackMarksHistoryBudget& budget,
                     TrackMarksStaticLod lod,
                     container::Vector<container::String>* diagnostics) {
    if (budget.opaqueEdges <= budget.maximumEdges) return;
    appendDiagnostic(diagnostics, "StaticGameLOD " + container::String(lodName(lod)) +
        " clamped MaxTankTrackOpaqueEdges from " +
        std::to_string(budget.opaqueEdges) + " to " +
        std::to_string(budget.maximumEdges));
    budget.opaqueEdges = budget.maximumEdges;
}

} // namespace

TrackMarksRenderDescriptor compileTrackMarksRenderDescriptor(
    const game::TrackMarksVisualDescriptor& visual,
    const TrackMarksPresentationSettings& settings) noexcept {
    const TrackMarksHistoryBudget& budget =
        settings.performance.fullDetailBudget();
    TrackMarksRenderDescriptor output;
    output.visual.enabled = settings.feature.enabled && visual.usable();
    output.visual.textureName = visual.textureName;
    output.visual.leftWidthBone = visual.leftWidthBone;
    output.visual.rightWidthBone = visual.rightWidthBone;
    output.visual.fallbackWidth = visual.fallbackWidth;
    output.visual.additionalTreadWidth = visual.additionalTreadWidth;
    output.visual.segmentLength = visual.segmentLength;
    output.performance.maximumEdges = budget.maximumEdges;
    output.performance.opaqueEdges =
        std::min(budget.opaqueEdges, budget.maximumEdges);
    output.performance.fadeDelayMilliseconds = budget.fadeDelayMilliseconds;
    output.performance.maximumTrackedObjects =
        settings.performance.maximumTrackedObjects;
    return output;
}

bool applyTrackMarksGameDataIni(
    container::StringView content,
    TrackMarksPresentationSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse GameData TrackMarks settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (!equalAsciiInsensitive(block.type, "GameData")) continue;
        for (const auto& [key, value] : block.values) {
            if (equalAsciiInsensitive(key, "MakeTrackMarks")) {
                bool parsed = settings.feature.enabled;
                if (!parseBool(value, parsed)) {
                    appendDiagnostic(diagnostics,
                        "GameData ignored malformed MakeTrackMarks='" + value + "'");
                } else {
                    settings.feature.enabled = parsed;
                }
            } else if (equalAsciiInsensitive(key, "MaxTerrainTracks")) {
                int64_t parsed = 0;
                if (!parseSigned(value, parsed)) {
                    appendDiagnostic(diagnostics,
                        "GameData ignored malformed MaxTerrainTracks='" + value + "'");
                    continue;
                }
                const uint32_t clamped = clampUnsigned(
                    parsed, 0,
                    track_marks::performance_limits::kHardMaximumStreams);
                if (parsed != static_cast<int64_t>(clamped)) {
                    appendDiagnostic(diagnostics,
                        "GameData clamped MaxTerrainTracks from " +
                        std::to_string(parsed) + " to " + std::to_string(clamped));
                }
                settings.performance.maximumTrackedObjects = clamped;
            }
        }
    }
    return true;
}

bool applyTrackMarksGameLodIni(
    container::StringView content,
    TrackMarksPresentationSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse StaticGameLOD TrackMarks settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (!equalAsciiInsensitive(block.type, "StaticGameLOD")) continue;
        TrackMarksStaticLod lod = TrackMarksStaticLod::High;
        if (!parseLod(block.name, lod)) {
            appendDiagnostic(diagnostics,
                "ignored unknown StaticGameLOD '" + block.name + "'");
            continue;
        }
        TrackMarksHistoryBudget& budget =
            settings.performance.lodBudgets[static_cast<size_t>(lod)];
        for (const auto& [key, value] : block.values) {
            if (equalAsciiInsensitive(key, "MaxTankTrackEdges") ||
                equalAsciiInsensitive(key, "MaxTankTrackOpaqueEdges") ||
                equalAsciiInsensitive(key, "MaxTankTrackFadeDelay")) {
                applyBudgetField(budget, lod, key, value, diagnostics);
            }
        }
        normalizeBudget(budget, lod, diagnostics);
    }
    return true;
}

} // namespace engine
