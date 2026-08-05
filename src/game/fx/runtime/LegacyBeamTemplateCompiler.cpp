#include "presentation/fx/runtime/LegacyBeamTemplate.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>

namespace engine::fx {
namespace {

constexpr auto asciiEqual = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* moduleValue(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (asciiEqual(it->first, key)) return &it->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqual(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] float parseFloat(const container::String* source,
                               float fallback) noexcept {
    if (!source) return fallback;
    return game::parseContentFloatOr(*source, {
        .source = __FILE__, .block = "Object", .module = "LaserDraw",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] uint32_t parseUnsigned(const container::String* source,
                                     uint32_t fallback) noexcept {
    if (!source) return fallback;
    uint32_t parsed = 0;
    const auto result = std::from_chars(
        source->data(), source->data() + source->size(), parsed);
    return result.ec == std::errc{} ? parsed : fallback;
}

[[nodiscard]] uint32_t millisecondsToAuthoredFrames(
    uint32_t milliseconds) noexcept {
    if (milliseconds == 0) return 0;
    constexpr uint64_t framesPerSecond = 30;
    return static_cast<uint32_t>(std::min<uint64_t>(
        (static_cast<uint64_t>(milliseconds) * framesPerSecond + 999u) /
            1000u,
        std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] bool parseBool(const container::String* source,
                             bool fallback = false) noexcept {
    if (!source) return fallback;
    if (asciiEqual(*source, "yes") || asciiEqual(*source, "true") ||
        asciiEqual(*source, "1")) return true;
    if (asciiEqual(*source, "no") || asciiEqual(*source, "false") ||
        asciiEqual(*source, "0")) return false;
    return fallback;
}

[[nodiscard]] LegacyBeamColor parseColor(const container::String* source,
                                         LegacyBeamColor fallback = {}) {
    if (!source) return fallback;
    LegacyBeamColor result = fallback;
    size_t cursor = 0;
    while (cursor < source->size()) {
        while (cursor < source->size() &&
               ((*source)[cursor] == ' ' || (*source)[cursor] == '\t')) ++cursor;
        const size_t separator = source->find(':', cursor);
        if (separator == container::String::npos || separator == cursor) break;
        const char channel = static_cast<char>(std::toupper(
            static_cast<unsigned char>((*source)[cursor])));
        size_t end = separator + 1u;
        while (end < source->size() && (*source)[end] != ' ' &&
               (*source)[end] != '\t') ++end;
        float value = fallback.alpha * 255.0f;
        const container::String token = source->substr(separator + 1u,
                                                        end - separator - 1u);
        const std::optional<float> parsed = game::parseContentFloat(token, {
            .source = __FILE__, .block = "Object", .module = "LaserDraw",
            .field = "ColorChannel", .fallback = value});
        if (parsed) {
            value = *parsed;
            value = std::clamp(value / 255.0f, 0.0f, 1.0f);
            if (channel == 'R') result.red = value;
            else if (channel == 'G') result.green = value;
            else if (channel == 'B') result.blue = value;
            else if (channel == 'A') result.alpha = value;
        }
        cursor = end;
    }
    return result;
}

[[nodiscard]] float millisecondsToSeconds(const container::String* source,
                                           float fallback = 0.0f) noexcept {
    return std::max(0.0f, parseFloat(source, fallback * 1000.0f)) * 0.001f;
}

[[nodiscard]] uint64_t mix64(uint64_t value) noexcept {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

} // namespace

std::optional<LegacyBeamTemplate> compileLegacyBeamTemplate(
    const game::ThingTemplate& source, container::String* diagnostic) {
    if (diagnostic) diagnostic->clear();
    const game::ModuleData* laserDraw = nullptr;
    const game::ModuleData* laserUpdate = nullptr;
    const game::ModuleData* lifetime = nullptr;
    bool tracer = false;
    bool rope = false;
    for (const game::ModuleData& module : source.modules) {
        if (asciiEqual(module.moduleClass, "W3DLaserDraw")) laserDraw = &module;
        else if (asciiEqual(module.moduleClass, "LaserUpdate")) laserUpdate = &module;
        else if (asciiEqual(module.moduleClass, "W3DTracerDraw")) tracer = true;
        else if (asciiEqual(module.moduleClass, "W3DRopeDraw")) rope = true;
        else if (asciiEqual(module.moduleClass, "LifetimeUpdate") ||
                 asciiEqual(module.moduleClass, "DeletionUpdate")) lifetime = &module;
    }

    LegacyBeamTemplate output;
    output.objectTemplate = source.name;
    if (laserDraw) {
        output.kind = LegacyBeamTemplateKind::Laser;
        LegacyLaserTemplate& value = output.laser;
        if (const container::String* texture = moduleValue(*laserDraw, "Texture")) {
            value.textureName = *texture;
        }
        value.numberOfBeams = std::max(1u, parseUnsigned(
            moduleValue(*laserDraw, "NumBeams"), 1u));
        value.innerBeamWidth = std::max(0.0f, parseFloat(
            moduleValue(*laserDraw, "InnerBeamWidth"), 0.0f));
        value.outerBeamWidth = std::max(0.0f, parseFloat(
            moduleValue(*laserDraw, "OuterBeamWidth"), 1.0f));
        value.outerBeamWidthFixed = math::q32_32{value.outerBeamWidth};
        value.innerColor = parseColor(moduleValue(*laserDraw, "InnerColor"));
        value.outerColor = parseColor(moduleValue(*laserDraw, "OuterColor"));
        value.scrollRate = parseFloat(moduleValue(*laserDraw, "ScrollRate"), 0.0f);
        value.tileTexture = parseBool(moduleValue(*laserDraw, "Tile"));
        value.segments = std::max(1u, parseUnsigned(
            moduleValue(*laserDraw, "Segments"), 1u));
        value.arcHeight = std::max(0.0f, parseFloat(
            moduleValue(*laserDraw, "ArcHeight"), 0.0f));
        value.segmentOverlapRatio = parseFloat(
            moduleValue(*laserDraw, "SegmentOverlapRatio"), 0.0f);
        value.tilingScalar = parseFloat(
            moduleValue(*laserDraw, "TilingScalar"), 1.0f);
        value.maximumIntensityFrames = millisecondsToAuthoredFrames(
            parseUnsigned(moduleValue(*laserDraw, "MaxIntensityLifetime"),
                          0u));
        value.fadeFrames = millisecondsToAuthoredFrames(
            parseUnsigned(moduleValue(*laserDraw, "FadeLifetime"), 0u));
        if (laserUpdate) {
            if (const container::String* muzzle =
                    moduleValue(*laserUpdate, "MuzzleParticleSystem")) {
                value.muzzleParticleSystem = *muzzle;
            }
            if (const container::String* target =
                    moduleValue(*laserUpdate, "TargetParticleSystem")) {
                value.targetParticleSystem = *target;
            }
            value.punchThroughScalar = std::max(0.0f, parseFloat(
                moduleValue(*laserUpdate, "PunchThroughScalar"), 0.0f));
        } else if (diagnostic) {
            *diagnostic = "W3DLaserDraw template has no LaserUpdate";
        }
        if (lifetime) {
            value.minimumLifetimeSeconds = millisecondsToSeconds(
                moduleValue(*lifetime, "MinLifetime"));
            value.maximumLifetimeSeconds = millisecondsToSeconds(
                moduleValue(*lifetime, "MaxLifetime"),
                value.minimumLifetimeSeconds);
            if (value.maximumLifetimeSeconds < value.minimumLifetimeSeconds) {
                std::swap(value.maximumLifetimeSeconds,
                          value.minimumLifetimeSeconds);
            }
        }
        // A beam without an authored LifetimeUpdate is one confirmed visual
        // frame, not an immortal renderer object. Persistent special-power
        // lasers refresh through their producer's lifecycle events.
        if (!(value.maximumLifetimeSeconds > 0.0f)) {
            value.minimumLifetimeSeconds = 1.0f / 30.0f;
            value.maximumLifetimeSeconds = 1.0f / 30.0f;
        }
        return output;
    }
    if (tracer) {
        output.kind = LegacyBeamTemplateKind::Tracer;
        return output;
    }
    if (rope) {
        output.kind = LegacyBeamTemplateKind::Rope;
        return output;
    }

    // RayEffect may name an ordinary model template. Preserve the first
    // resolved channel's model identity instead of silently manufacturing a
    // generic ribbon; stock ZH does not author this path, but Mods do.
    for (const game::ModelDrawVisualChannel& channel : source.drawVisualChannels) {
        if (!channel.defaultModel.empty()) {
            output.kind = LegacyBeamTemplateKind::ModelRay;
            output.modelAsset = channel.defaultModel;
            output.modelRay.assetScale = source.assetScale.to_float();
            output.modelRay.castsDirectionalShadow =
                source.shadow.castsDirectionalShadow();
            if (lifetime) {
                output.modelRay.lifetimeAuthored = true;
                output.modelRay.minimumLifetimeMilliseconds = parseUnsigned(
                    moduleValue(*lifetime, "MinLifetime"), 0u);
                output.modelRay.maximumLifetimeMilliseconds = parseUnsigned(
                    moduleValue(*lifetime, "MaxLifetime"), 0u);
            }
            return output;
        }
        // A newly-created legacy Drawable begins with an empty condition
        // mask.  `defaultModel` is the compiler's exact projection of that
        // DefaultConditionState.  Picking the first arbitrary conditioned
        // rule here would make a dormant/upgrade/damaged-only model visible
        // even though the original RayEffect Drawable emitted no mesh.
    }
    return std::nullopt;
}

} // namespace engine::fx
