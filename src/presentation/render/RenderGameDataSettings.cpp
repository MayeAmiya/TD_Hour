#include "RenderGameDataSettings.h"
#include "RenderQualitySettingsManager.h"

#include "core/config/GraphPreferences.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "presentation/render/WaterSurfacePerformanceSettings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace engine {
namespace {

constexpr uint32_t kMaximumAutoBodyParticleBones = 16u;

container::String lowerAscii(container::StringView value) {
    container::String result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

void diagnostic(container::Vector<container::String>* output,
                container::String message) {
    if (output) output->push_back(std::move(message));
}

void setError(container::String* output, container::String message) {
    if (output) *output = std::move(message);
}

bool parseBool(container::StringView value, bool& output) {
    const container::String lower = lowerAscii(value);
    if (lower == "yes" || lower == "true" || lower == "1") {
        output = true;
        return true;
    }
    if (lower == "no" || lower == "false" || lower == "0") {
        output = false;
        return true;
    }
    return false;
}

bool parseFloat(container::StringView value, float& output) {
    const container::String owned(value);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(owned.c_str(), &end);
    if (end == owned.c_str() || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end == 'f' || *end == 'F') ++end;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    output = parsed;
    return true;
}

bool parseDurationMilliseconds(container::StringView value, uint32_t& output) {
    const container::String owned = lowerAscii(value);
    const char* begin = owned.c_str();
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || errno == ERANGE || !std::isfinite(parsed) ||
        parsed < 0.0) {
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    double milliseconds = parsed;
    if (*end != '\0') {
        const container::StringView suffix(end);
        if (suffix == "s" || suffix == "sec" || suffix == "second" ||
            suffix == "seconds") {
            milliseconds *= 1000.0;
        } else if (suffix != "ms" && suffix != "msec" &&
                   suffix != "millisecond" && suffix != "milliseconds") {
            return false;
        }
    }
    output = static_cast<uint32_t>(std::min<double>(
        std::round(milliseconds),
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
    return true;
}

bool parseSigned(container::StringView value, int64_t& output) {
    const container::String owned(value);
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    output = static_cast<int64_t>(parsed);
    return true;
}

bool parseUnsigned32(container::StringView value, uint32_t& output) {
    int64_t parsed = 0;
    if (!parseSigned(value, parsed) || parsed < 0 ||
        parsed > static_cast<int64_t>(
            std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    output = static_cast<uint32_t>(parsed);
    return true;
}

uint32_t boundedUnsigned(int64_t value, uint32_t maximum) noexcept {
    if (value <= 0) return 0;
    return static_cast<uint32_t>(std::min<int64_t>(value, maximum));
}

uint8_t boundedByte(int64_t value) noexcept {
    return static_cast<uint8_t>(std::clamp<int64_t>(value, 0, 255));
}

uint32_t highestPowerOfTwo(uint32_t value) noexcept {
    if (value == 0) return 0;
    uint32_t result = 1;
    while (result <= value / 2u) result *= 2u;
    return result;
}

std::optional<RenderStaticLod> parseLod(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "low") return RenderStaticLod::Low;
    if (lower == "medium") return RenderStaticLod::Medium;
    if (lower == "high") return RenderStaticLod::High;
    if (lower == "veryhigh" || lower == "very_high")
        return RenderStaticLod::VeryHigh;
    if (lower == "custom") return RenderStaticLod::Custom;
    return std::nullopt;
}

std::optional<RenderDynamicLod> parseDynamicLod(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "low") return RenderDynamicLod::Low;
    if (lower == "medium") return RenderDynamicLod::Medium;
    if (lower == "high") return RenderDynamicLod::High;
    if (lower == "veryhigh" || lower == "very_high")
        return RenderDynamicLod::VeryHigh;
    return std::nullopt;
}

std::optional<RenderParticlePriority> parseParticlePriority(
    container::StringView value) {
    static constexpr container::Array<container::StringView, 14> names{
        "none", "weapon_explosion", "scorchmark", "dust_trail", "buildup",
        "debris_trail", "unit_damage_fx", "death_explosion", "semi_constant",
        "constant", "weapon_trail", "area_effect", "critical", "always_render"};
    const container::String lower = lowerAscii(value);
    for (size_t index = 0; index < names.size(); ++index) {
        if (lower == names[index])
            return static_cast<RenderParticlePriority>(index);
    }
    return std::nullopt;
}

std::optional<RenderTerrainLod> parseTerrainLod(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "automatic" || lower == "auto")
        return RenderTerrainLod::Automatic;
    if (lower == "low") return RenderTerrainLod::Low;
    if (lower == "medium") return RenderTerrainLod::Medium;
    if (lower == "high") return RenderTerrainLod::High;
    if (lower == "veryhigh" || lower == "very_high")
        return RenderTerrainLod::VeryHigh;
    // Legacy DISABLE turns off adaptive terrain degradation. The modern
    // renderer expresses that as a fixed highest-quality terrain setting.
    if (lower == "disable") return RenderTerrainLod::VeryHigh;
    return std::nullopt;
}

std::optional<uint32_t> parseTextureFilter(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "none") return 0;
    if (lower == "point") return 1;
    if (lower == "bilinear") return 2;
    if (lower == "trilinear") return 3;
    if (lower == "anisotropic") return 4;
    int64_t numeric = 0;
    if (parseSigned(value, numeric) && numeric >= 0 && numeric <= 4)
        return static_cast<uint32_t>(numeric);
    return std::nullopt;
}

float displayGammaFromOption(uint32_t value) noexcept {
    if (value < 50)
        return value == 0
            ? 0.6f
            : 1.0f - 0.4f * static_cast<float>(50u - value) / 50.0f;
    if (value > 50)
        return 1.0f + static_cast<float>(value - 50u) / 50.0f;
    return 1.0f;
}

container::Vector<float> parseScalarList(container::StringView value) {
    container::Vector<float> result;
    const container::String owned(value);
    const char* cursor = owned.c_str();
    while (*cursor != '\0') {
        if (!std::isdigit(static_cast<unsigned char>(*cursor)) &&
            *cursor != '-' && *cursor != '+' && *cursor != '.') {
            ++cursor;
            continue;
        }
        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(cursor, &end);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        if (errno != ERANGE && std::isfinite(parsed)) result.push_back(parsed);
        cursor = end;
    }
    return result;
}

bool parseRgb(container::StringView value, RenderRgbColor& output) {
    const container::Vector<float> scalars = parseScalarList(value);
    if (scalars.size() != 3) return false;
    output = {
        boundedByte(static_cast<int64_t>(scalars[0])),
        boundedByte(static_cast<int64_t>(scalars[1])),
        boundedByte(static_cast<int64_t>(scalars[2])),
    };
    return true;
}

bool parseArgb(container::StringView value, uint32_t& output) {
    const container::Vector<float> scalars = parseScalarList(value);
    if (scalars.size() != 3 && scalars.size() != 4) return false;
    if (std::any_of(scalars.begin(), scalars.end(), [](float component) {
            return component < 0.0f || component > 255.0f ||
                std::floor(component) != component;
        })) {
        return false;
    }
    const uint8_t red = boundedByte(static_cast<int64_t>(scalars[0]));
    const uint8_t green = boundedByte(static_cast<int64_t>(scalars[1]));
    const uint8_t blue = boundedByte(static_cast<int64_t>(scalars[2]));
    const uint8_t alpha = scalars.size() == 4
        ? boundedByte(static_cast<int64_t>(scalars[3])) : 255u;
    output = (static_cast<uint32_t>(alpha) << 24u) |
        (static_cast<uint32_t>(red) << 16u) |
        (static_cast<uint32_t>(green) << 8u) |
        static_cast<uint32_t>(blue);
    return true;
}

container::String trimAndUnquote(container::StringView value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return container::String(value);
}

bool parseFontDescriptor(container::StringView value,
                         container::String& name,
                         int32_t& pointSize, bool& bold) {
    // Language.ini stores `font-name point-size bold`.  Work backwards so
    // an unquoted multi-word family remains intact just like legacy FieldParse.
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    const size_t boldSeparator = value.find_last_of(" \t");
    if (boldSeparator == container::StringView::npos) return false;
    container::StringView boldValue = value.substr(boldSeparator + 1u);
    value = value.substr(0, boldSeparator);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    const size_t sizeSeparator = value.find_last_of(" \t");
    if (sizeSeparator == container::StringView::npos) return false;
    int64_t parsedSize = 0;
    bool parsedBold = false;
    container::String parsedName = trimAndUnquote(
        value.substr(0, sizeSeparator));
    if (parsedName.empty() ||
        !parseSigned(value.substr(sizeSeparator + 1u), parsedSize) ||
        parsedSize <= 0 || parsedSize > 256 ||
        !parseBool(boldValue, parsedBold)) {
        return false;
    }
    name = std::move(parsedName);
    pointSize = static_cast<int32_t>(parsedSize);
    bold = parsedBold;
    return true;
}

void applyInGameUiField(
    container::StringView authoredKey, container::StringView value,
    RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics) {
    const container::String key = lowerAscii(authoredKey);
    auto& feedback = settings.visual.objectFeedback;
    if (key == "drawablecaptionfont") {
        const container::String parsed = trimAndUnquote(value);
        if (!parsed.empty()) feedback.drawableCaptionFont = parsed;
        else diagnostic(diagnostics, "ignored empty DrawableCaptionFont");
    } else if (key == "drawablecaptionpointsize") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed) && parsed > 0 && parsed <= 256) {
            feedback.drawableCaptionPointSize = static_cast<int32_t>(parsed);
        } else {
            diagnostic(diagnostics,
                "ignored malformed DrawableCaptionPointSize");
        }
    } else if (key == "drawablecaptionbold") {
        bool parsed = feedback.drawableCaptionBold;
        if (parseBool(value, parsed)) feedback.drawableCaptionBold = parsed;
        else diagnostic(diagnostics,
            "ignored malformed DrawableCaptionBold");
    } else if (key == "drawablecaptioncolor") {
        uint32_t parsed = feedback.drawableCaptionColor;
        if (parseArgb(value, parsed)) feedback.drawableCaptionColor = parsed;
        else diagnostic(diagnostics,
            "ignored malformed DrawableCaptionColor");
    } else if (key == "floatingtexttimeout") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed) && parsed >= 0 &&
            parsed <= std::numeric_limits<uint32_t>::max()) {
            feedback.floatingTextTimeoutMilliseconds =
                static_cast<uint32_t>(parsed);
        } else {
            diagnostic(diagnostics, "ignored malformed FloatingTextTimeOut");
        }
    } else if (key == "floatingtextmoveupspeed") {
        float parsed = feedback.floatingTextMoveUpPerSecond;
        if (parseFloat(value, parsed)) {
            feedback.floatingTextMoveUpPerSecond = parsed;
        } else {
            diagnostic(diagnostics,
                "ignored malformed FloatingTextMoveUpSpeed");
        }
    } else if (key == "floatingtextvanishrate") {
        float parsed = feedback.floatingTextVanishPerSecond;
        if (parseFloat(value, parsed)) {
            feedback.floatingTextVanishPerSecond = parsed;
        } else {
            diagnostic(diagnostics,
                "ignored malformed FloatingTextVanishRate");
        }
    }
}

bool parseVector3(container::StringView value, RenderVector3& output) {
    const container::Vector<float> scalars = parseScalarList(value);
    if (scalars.size() != 3) return false;
    output = {scalars[0], scalars[1], scalars[2]};
    return true;
}

bool parseResolution(container::StringView value, uint32_t& width,
                     uint32_t& height) {
    const size_t separator = value.find_first_of("xX, ");
    if (separator == container::StringView::npos) return false;
    int64_t parsedWidth = 0;
    int64_t parsedHeight = 0;
    size_t heightBegin = value.find_first_not_of("xX, \t", separator);
    if (!parseSigned(value.substr(0, separator), parsedWidth) ||
        heightBegin == container::StringView::npos ||
        !parseSigned(value.substr(heightBegin), parsedHeight) ||
        parsedWidth <= 0 || parsedHeight <= 0) {
        return false;
    }
    width = boundedUnsigned(
        parsedWidth, render_game_data_limits::kMaximumRenderDimension);
    height = boundedUnsigned(
        parsedHeight, render_game_data_limits::kMaximumRenderDimension);
    return width != 0 && height != 0;
}

template <typename Value, typename Parser>
void applyParsedField(container::StringView key, container::StringView source,
                      Value& destination, Parser&& parser,
                      container::Vector<container::String>* diagnostics) {
    Value parsed = destination;
    if (!parser(source, parsed)) {
        diagnostic(diagnostics, "ignored malformed " + container::String(key) +
            "='" + container::String(source) + "'");
        return;
    }
    destination = parsed;
}

void applyUnsignedField(container::StringView key, container::StringView source,
                        uint32_t maximum, uint32_t& destination,
                        container::Vector<container::String>* diagnostics) {
    int64_t parsed = 0;
    if (!parseSigned(source, parsed)) {
        diagnostic(diagnostics, "ignored malformed " + container::String(key) +
            "='" + container::String(source) + "'");
        return;
    }
    const uint32_t bounded = boundedUnsigned(parsed, maximum);
    if (parsed != static_cast<int64_t>(bounded)) {
        diagnostic(diagnostics, "clamped " + container::String(key) + " from " +
            std::to_string(parsed) + " to " + std::to_string(bounded));
    }
    destination = bounded;
}

bool applyBodyParticleField(
    container::StringView key, container::StringView authoredKey,
    container::StringView value, RenderBodyParticleGameData& bodyParticles,
    container::Vector<container::String>* diagnostics) {
    const auto applyChannel = [&](container::StringView prefixKey,
                                  container::StringView systemKey,
                                  container::StringView maximumKey,
                                  RenderBodyParticleChannel& channel) {
        if (key == prefixKey) {
            channel.prefix = trimAndUnquote(value);
            return true;
        }
        if (key == systemKey) {
            channel.particleSystem = trimAndUnquote(value);
            return true;
        }
        if (key == maximumKey) {
            applyUnsignedField(authoredKey, value,
                kMaximumAutoBodyParticleBones, channel.maximumSystems,
                diagnostics);
            return true;
        }
        return false;
    };

    return applyChannel(
               "autofireparticlesmallprefix",
               "autofireparticlesmallsystem",
               "autofireparticlesmallmax", bodyParticles.fireSmall) ||
        applyChannel(
               "autofireparticlemediumprefix",
               "autofireparticlemediumsystem",
               "autofireparticlemediummax", bodyParticles.fireMedium) ||
        applyChannel(
               "autofireparticlelargeprefix",
               "autofireparticlelargesystem",
               "autofireparticlelargemax", bodyParticles.fireLarge) ||
        applyChannel(
               "autosmokeparticlesmallprefix",
               "autosmokeparticlesmallsystem",
               "autosmokeparticlesmallmax", bodyParticles.smokeSmall) ||
        applyChannel(
               "autosmokeparticlemediumprefix",
               "autosmokeparticlemediumsystem",
               "autosmokeparticlemediummax", bodyParticles.smokeMedium) ||
        applyChannel(
               "autosmokeparticlelargeprefix",
               "autosmokeparticlelargesystem",
               "autosmokeparticlelargemax", bodyParticles.smokeLarge) ||
        applyChannel(
               "autoaflameparticleprefix",
               "autoaflameparticlesystem",
               "autoaflameparticlemax", bodyParticles.aflame);
}

void applyByteField(container::StringView key, container::StringView source,
                    uint8_t& destination,
                    container::Vector<container::String>* diagnostics) {
    int64_t parsed = 0;
    if (!parseSigned(source, parsed)) {
        diagnostic(diagnostics, "ignored malformed " + container::String(key));
        return;
    }
    destination = boundedByte(parsed);
}

bool applyLightingField(container::StringView authoredKey,
                        container::StringView value,
                        RenderGameDataSettings& settings,
                        container::Vector<container::String>* diagnostics) {
    container::String key = lowerAscii(authoredKey);
    bool objectLighting = false;
    constexpr container::StringView terrainPrefix = "terrainlighting";
    constexpr container::StringView objectPrefix = "terrainobjectslighting";
    if (key.starts_with(objectPrefix)) {
        objectLighting = true;
        key.erase(0, objectPrefix.size());
    } else if (key.starts_with(terrainPrefix)) {
        key.erase(0, terrainPrefix.size());
    } else {
        return false;
    }

    static constexpr container::Array<container::StringView, 4> timeNames{
        "morning", "afternoon", "evening", "night"};
    size_t timeIndex = timeNames.size();
    for (size_t index = 0; index < timeNames.size(); ++index) {
        if (key.starts_with(timeNames[index])) {
            timeIndex = index;
            key.erase(0, timeNames[index].size());
            break;
        }
    }
    if (timeIndex == timeNames.size()) return false;

    size_t lightIndex = 0;
    if (!key.empty() && (key.back() == '2' || key.back() == '3')) {
        lightIndex = static_cast<size_t>(key.back() - '1');
        key.pop_back();
    }
    auto& table = objectLighting ? settings.visual.lighting.objects
                                 : settings.visual.lighting.terrain;
    RenderLightDescriptor& light = table[timeIndex][lightIndex];
    if (key == "ambient") {
        applyParsedField(authoredKey, value, light.ambient, parseRgb,
                         diagnostics);
        return true;
    }
    if (key == "diffuse") {
        applyParsedField(authoredKey, value, light.diffuse, parseRgb,
                         diagnostics);
        return true;
    }
    if (key == "lightpos") {
        applyParsedField(authoredKey, value, light.position, parseVector3,
                         diagnostics);
        return true;
    }
    return false;
}

void applyGameDataField(container::StringView authoredKey,
                        container::StringView value,
                        RenderGameDataSettings& settings,
                        container::Vector<container::String>* diagnostics) {
    const container::String key = lowerAscii(authoredKey);
    auto& visual = settings.visual;
    auto& budget = settings.operational;
    auto& compatibility = settings.compatibility;
    if (applyBodyParticleField(
            key, authoredKey, value, visual.bodyParticles, diagnostics)) {
        return;
    }
    if (applyLightingField(authoredKey, value, settings, diagnostics)) return;
    if (key == "camerapitch")
        applyParsedField(authoredKey, value, visual.camera.pitchDegrees,
                         parseFloat, diagnostics);
    else if (key == "camerayaw")
        applyParsedField(authoredKey, value, visual.camera.yawDegrees,
                         parseFloat, diagnostics);
    else if (key == "cameraheight")
        applyParsedField(authoredKey, value, visual.camera.initialHeight,
                         parseFloat, diagnostics);
    else if (key == "mincameraheight")
        applyParsedField(authoredKey, value, visual.camera.minimumHeight,
                         parseFloat, diagnostics);
    else if (key == "maxcameraheight")
        applyParsedField(authoredKey, value, visual.camera.maximumHeight,
                         parseFloat, diagnostics);
    else if (key == "cameraadjustspeed")
        applyParsedField(authoredKey, value, visual.camera.adjustSpeed,
                         parseFloat, diagnostics);
    else if (key == "scrollamountcutoff")
        applyParsedField(authoredKey, value, visual.camera.scrollAmountCutoff,
                         parseFloat, diagnostics);
    else if (key == "horizontalscrollspeedfactor")
        applyParsedField(authoredKey, value,
                         visual.camera.horizontalScrollSpeedFactor,
                         parseFloat, diagnostics);
    else if (key == "verticalscrollspeedfactor")
        applyParsedField(authoredKey, value,
                         visual.camera.verticalScrollSpeedFactor,
                         parseFloat, diagnostics);
    else if (key == "keyboardscrollspeedfactor")
        applyParsedField(authoredKey, value,
                         visual.camera.keyboardScrollSpeedFactor,
                         parseFloat, diagnostics);
    else if (key == "keyboarddefaultscrollspeedfactor")
        applyParsedField(authoredKey, value,
                         visual.camera.keyboardDefaultScrollSpeedFactor,
                         parseFloat, diagnostics);
    else if (key == "keyboardcamerarotatespeed")
        applyParsedField(authoredKey, value,
                         visual.camera.keyboardRotateSpeed,
                         parseFloat, diagnostics);
    else if (key == "viewportheightscale")
        applyParsedField(authoredKey, value,
                         visual.camera.viewportHeightScale,
                         parseFloat, diagnostics);
    else if (key == "enforcemaxcameraheight")
        applyParsedField(authoredKey, value,
                         visual.camera.enforceMaximumHeight,
                         parseBool, diagnostics);
    else if (key == "rightmousealwaysscrolls")
        applyParsedField(authoredKey, value,
                         visual.input.rightMouseAlwaysScrolls,
                         parseBool, diagnostics);
    else if (key == "drawskybox")
        applyParsedField(authoredKey, value, visual.water.drawSkyBox,
                         parseBool, diagnostics);
    else if (key == "skyboxpositionz")
        applyParsedField(authoredKey, value, visual.water.skyBoxPositionZ,
                         parseFloat, diagnostics);
    else if (key == "skyboxscale")
        applyParsedField(authoredKey, value, visual.water.skyBoxScale,
                         parseFloat, diagnostics);
    else if (key == "waterpositionx")
        applyParsedField(authoredKey, value, visual.water.positionX,
                         parseFloat, diagnostics);
    else if (key == "waterpositiony")
        applyParsedField(authoredKey, value, visual.water.positionY,
                         parseFloat, diagnostics);
    else if (key == "waterpositionz")
        applyParsedField(authoredKey, value, visual.water.positionZ,
                         parseFloat, diagnostics);
    else if (key == "waterextentx")
        applyParsedField(authoredKey, value, visual.water.extentX,
                         parseFloat, diagnostics);
    else if (key == "waterextenty")
        applyParsedField(authoredKey, value, visual.water.extentY,
                         parseFloat, diagnostics);
    else if (key == "showsoftwateredge")
        applyParsedField(authoredKey, value, visual.water.showSoftEdge,
                         parseBool, diagnostics);
    else if (key == "adjustclifftextures")
        applyParsedField(authoredKey, value, visual.terrain.adjustCliffTextures,
                         parseBool, diagnostics);
    else if (key == "bilinearterraintex")
        applyParsedField(authoredKey, value, visual.terrain.bilinearTextures,
                         parseBool, diagnostics);
    else if (key == "trilinearterraintex")
        applyParsedField(authoredKey, value, visual.terrain.trilinearTextures,
                         parseBool, diagnostics);
    else if (key == "usecloudmap")
        applyParsedField(authoredKey, value, visual.terrain.useCloudMap,
                         parseBool, diagnostics);
    else if (key == "uselightmap")
        applyParsedField(authoredKey, value, visual.terrain.useLightMap,
                         parseBool, diagnostics);
    else if (key == "usetrees" || key == "showtrees")
        applyParsedField(authoredKey, value, visual.terrain.showTrees,
                         parseBool, diagnostics);
    else if (key == "usetreesway")
        applyParsedField(authoredKey, value, visual.terrain.useTreeSway,
                         parseBool, diagnostics);
    else if (key == "usebuildupscaffolds")
        applyParsedField(authoredKey, value,
                         visual.terrain.useBuildupScaffolds,
                         parseBool, diagnostics);
    else if (key == "extraanimations")
        applyParsedField(authoredKey, value, visual.terrain.extraAnimations,
                         parseBool, diagnostics);
    else if (key == "useheateffects")
        applyParsedField(authoredKey, value, visual.terrain.useHeatEffects,
                         parseBool, diagnostics);
    else if (key == "multipassterrain")
        applyParsedField(authoredKey, value, visual.terrain.multiPass,
                         parseBool, diagnostics);
    else if (key == "use3wayterrainblends") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed))
            visual.terrain.threeWayBlend = parsed != 0;
        else
            diagnostic(diagnostics, "ignored malformed " +
                container::String(authoredKey));
    } else if (key == "stretchterrain")
        applyParsedField(authoredKey, value, visual.terrain.stretch,
                         parseBool, diagnostics);
    else if (key == "usehalfheightmap")
        applyParsedField(authoredKey, value,
                         visual.terrain.useHalfHeightMap, parseBool,
                         diagnostics);
    else if (key == "drawentireterrain")
        applyParsedField(authoredKey, value,
                         visual.terrain.drawEntireTerrain, parseBool,
                         diagnostics);
    else if (key == "terrainlod") {
        if (const auto parsed = parseTerrainLod(value))
            visual.terrain.lod = *parsed;
        else
            diagnostic(diagnostics, "ignored malformed TerrainLOD='" +
                container::String(value) + "'");
    }
    else if (key == "usewaterplane")
        applyParsedField(authoredKey, value, visual.water.useWaterPlane,
                         parseBool, diagnostics);
    else if (key == "usecloudplane")
        applyParsedField(authoredKey, value, visual.water.useCloudPlane,
                         parseBool, diagnostics);
    else if (key == "watertype") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed))
            visual.water.waterType = static_cast<int32_t>(std::clamp<int64_t>(
                parsed, std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::max()));
        else
            diagnostic(diagnostics, "ignored malformed WaterType");
    }
    else if (key == "useshadowvolumes")
        applyParsedField(authoredKey, value, visual.shadows.useVolumes,
                         parseBool, diagnostics);
    else if (key == "useshadowdecals")
        applyParsedField(authoredKey, value, visual.shadows.useDecals,
                         parseBool, diagnostics);
    else if (key == "showobjecthealth")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.showObjectHealth,
                         parseBool, diagnostics);
    else if (key == "powerbarbase") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed)) {
            visual.controlBarPower.logarithmicBase = static_cast<int32_t>(
                std::clamp<int64_t>(
                    parsed, 2, std::numeric_limits<int32_t>::max()));
        } else {
            diagnostic(diagnostics, "ignored malformed PowerBarBase");
        }
    }
    else if (key == "powerbarintervals")
        applyParsedField(authoredKey, value,
                         visual.controlBarPower.intervals,
                         parseFloat, diagnostics);
    else if (key == "powerbaryellowrange") {
        int64_t parsed = 0;
        if (parseSigned(value, parsed)) {
            visual.controlBarPower.yellowRange = static_cast<int32_t>(
                std::clamp<int64_t>(
                    parsed, 0, std::numeric_limits<int32_t>::max()));
        } else {
            diagnostic(diagnostics, "ignored malformed PowerBarYellowRange");
        }
    }
    else if (key == "selectionflashsaturationfactor")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.selectionFlashSaturationFactor,
                         parseFloat, diagnostics);
    else if (key == "selectionflashhousecolor")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.selectionFlashHouseColor,
                         parseBool, diagnostics);
    else if (key == "objectplacementopacity")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.objectPlacementOpacity,
                         parseFloat, diagnostics);
    else if (key == "objectplacementshadows")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.objectPlacementShadows,
                         parseBool, diagnostics);
    else if (key == "levelgainanimationname")
        visual.objectFeedback.levelGainAnimationName =
            trimAndUnquote(value);
    else if (key == "levelgainanimationtime")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.levelGainAnimationDisplaySeconds,
                         parseFloat, diagnostics);
    else if (key == "levelgainanimationzrise")
        applyParsedField(authoredKey, value,
                         visual.objectFeedback.levelGainAnimationZRisePerSecond,
                         parseFloat, diagnostics);
    else if (key == "usebehindbuildingmarker" ||
             key == "enablebehindbuildingmarkers")
        applyParsedField(authoredKey, value,
                         visual.visibility.behindBuildingMarkers,
                         parseBool, diagnostics);
    else if (key == "occludedcolorluminancescale")
        applyParsedField(authoredKey, value,
                         visual.visibility.occludedLuminanceScale,
                         parseFloat, diagnostics);
    else if (key == "clearalpha")
        applyByteField(authoredKey, value, visual.visibility.clearAlpha,
                       diagnostics);
    else if (key == "fogalpha")
        applyByteField(authoredKey, value, visual.visibility.fogAlpha,
                       diagnostics);
    else if (key == "shroudalpha")
        applyByteField(authoredKey, value, visual.visibility.shroudAlpha,
                       diagnostics);
    else if (key == "shroudcolor")
        applyParsedField(authoredKey, value, visual.visibility.shroudColor,
                         parseRgb, diagnostics);
    else if (key == "numbergloballights")
        applyUnsignedField(authoredKey, value,
            RenderLightingGameData::kGlobalLightCount,
            visual.lighting.globalLightCount, diagnostics);
    else if (key == "infantrylightmorningscale")
        applyParsedField(authoredKey, value,
            visual.lighting.infantryScale[0], parseFloat, diagnostics);
    else if (key == "infantrylightafternoonscale")
        applyParsedField(authoredKey, value,
            visual.lighting.infantryScale[1], parseFloat, diagnostics);
    else if (key == "infantrylighteveningscale")
        applyParsedField(authoredKey, value,
            visual.lighting.infantryScale[2], parseFloat, diagnostics);
    else if (key == "infantrylightnightscale")
        applyParsedField(authoredKey, value,
            visual.lighting.infantryScale[3], parseFloat, diagnostics);
    else if (key == "unlookpersistduration")
        applyParsedField(authoredKey, value,
            visual.visibility.unlookPersistMilliseconds,
            parseDurationMilliseconds, diagnostics);
    else if (key == "defaultocclusiondelay")
        applyParsedField(authoredKey, value,
            visual.visibility.defaultOcclusionDelayMilliseconds,
            parseDurationMilliseconds, diagnostics);
    else if (key == "particlescale")
        applyParsedField(authoredKey, value, visual.particleScale,
                         parseFloat, diagnostics);
    else if (key == "maxparticlecount")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumParticles,
            budget.maximumParticles, diagnostics);
    else if (key == "maxfieldparticlecount")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumFieldParticles,
            budget.maximumFieldParticles, diagnostics);
    else if (key == "maxterraintracks")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumTerrainTracks,
            budget.maximumTerrainTracks, diagnostics);
    else if (key == "terrainlodtargettimems")
        applyUnsignedField(authoredKey, value, 60000,
            budget.terrainLodTargetMilliseconds, diagnostics);
    else if (key == "maxroadsegments")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumRoadSegments,
            compatibility.maximumRoadSegments, diagnostics);
    else if (key == "maxroadvertex")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumRoadVertices,
            compatibility.maximumRoadVertices, diagnostics);
    else if (key == "maxroadindex")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumRoadIndices,
            compatibility.maximumRoadIndices, diagnostics);
    else if (key == "maxroadtypes")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumRoadTypes,
            compatibility.maximumRoadTypes, diagnostics);
    else if (key == "maxtranslucentobjects" ||
             key == "maxvisibletranslucentobjects")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumVisibleObjectsPerClass,
            compatibility.maximumVisibleTranslucentObjects, diagnostics);
    else if (key == "maxvisibleoccluderobjects")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumVisibleObjectsPerClass,
            compatibility.maximumVisibleOccluderObjects, diagnostics);
    else if (key == "maxvisibleoccludeeobjects")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumVisibleObjectsPerClass,
            compatibility.maximumVisibleOccludeeObjects, diagnostics);
    else if (key == "maxvisiblenonoccluderoroccludeeobjects")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumVisibleObjectsPerClass,
            compatibility.maximumVisibleOtherObjects, diagnostics);
    else if (key == "texturereductionfactor")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumTextureReductionFactor,
            visual.device.textureReductionFactor, diagnostics);
    else if (key == "timeofday") {
        const container::String lower = lowerAscii(value);
        if (lower == "morning") visual.defaultTimeOfDay = RenderTimeOfDay::Morning;
        else if (lower == "afternoon") visual.defaultTimeOfDay = RenderTimeOfDay::Afternoon;
        else if (lower == "evening") visual.defaultTimeOfDay = RenderTimeOfDay::Evening;
        else if (lower == "night") visual.defaultTimeOfDay = RenderTimeOfDay::Night;
        else diagnostic(diagnostics, "ignored malformed TimeOfDay='" +
            container::String(value) + "'");
    } else if (key == "weather") {
        const container::String lower = lowerAscii(value);
        if (lower == "normal") visual.defaultWeather = RenderWeather::Normal;
        else if (lower == "snowy") visual.defaultWeather = RenderWeather::Snowy;
        else diagnostic(diagnostics, "ignored malformed Weather='" +
            container::String(value) + "'");
    } else if (key == "forcemodelstofollowtimeofday") {
        applyParsedField(authoredKey, value,
            visual.forceModelsToFollowTimeOfDay, parseBool, diagnostics);
    } else if (key == "forcemodelstofollowweather") {
        applyParsedField(authoredKey, value,
            visual.forceModelsToFollowWeather, parseBool, diagnostics);
    } else if (key.starts_with("vertexwater")) {
        if (key.empty() || key.back() < '1' || key.back() > '4') return;
        const size_t index = static_cast<size_t>(key.back() - '1');
        const container::String field = key.substr(0, key.size() - 1u);
        RenderVertexWaterGameData& water = visual.water.vertexWater[index];
        if (field == "vertexwateravailablemaps") water.availableMaps = value;
        else if (field == "vertexwaterheightclamplow")
            applyParsedField(authoredKey, value, water.heightClampLow,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterheightclamphi")
            applyParsedField(authoredKey, value, water.heightClampHigh,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterangle") {
            float degrees = 0.0f;
            if (parseFloat(value, degrees)) {
                water.angleRadians = degrees * std::numbers::pi_v<float> / 180.0f;
            } else diagnostic(diagnostics, "ignored malformed " +
                container::String(authoredKey));
        } else if (field == "vertexwaterxposition")
            applyParsedField(authoredKey, value, water.positionX,
                             parseFloat, diagnostics);
        else if (field == "vertexwateryposition")
            applyParsedField(authoredKey, value, water.positionY,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterzposition")
            applyParsedField(authoredKey, value, water.positionZ,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterxgridcells")
            applyUnsignedField(authoredKey, value,
                               water_surface::performance_limits::
                                   kMaximumVertexWaterGridCellsPerAxis,
                               water.gridCellsX, diagnostics);
        else if (field == "vertexwaterygridcells")
            applyUnsignedField(authoredKey, value,
                               water_surface::performance_limits::
                                   kMaximumVertexWaterGridCellsPerAxis,
                               water.gridCellsY, diagnostics);
        else if (field == "vertexwatergridsize")
            applyParsedField(authoredKey, value, water.gridSize,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterattenuationa")
            applyParsedField(authoredKey, value, water.attenuationA,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterattenuationb")
            applyParsedField(authoredKey, value, water.attenuationB,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterattenuationc")
            applyParsedField(authoredKey, value, water.attenuationC,
                             parseFloat, diagnostics);
        else if (field == "vertexwaterattenuationrange")
            applyParsedField(authoredKey, value, water.attenuationRange,
                             parseFloat, diagnostics);
    }
}

void applyLodProfile(RenderGameDataSettings& settings) {
    const RenderGameLodProfile& profile =
        settings.lodProfiles[static_cast<size_t>(settings.selectedLod)];
    settings.operational.maximumParticles = profile.maximumParticles;
    settings.visual.shadows.useVolumes = profile.useShadowVolumes;
    settings.visual.shadows.useDecals = profile.useShadowDecals;
    settings.visual.terrain.useCloudMap = profile.useCloudMap;
    settings.visual.terrain.useLightMap = profile.useLightMap;
    settings.visual.water.showSoftEdge = profile.showSoftWaterEdge;
    settings.visual.terrain.useBuildupScaffolds = profile.useBuildupScaffolds;
    settings.visual.terrain.extraAnimations = profile.useBuildupScaffolds;
    settings.visual.terrain.useDrawModuleLod = !profile.useBuildupScaffolds;
    settings.visual.terrain.useTreeSway = profile.useTreeSway;
    settings.visual.terrain.useHeatEffects = profile.useHeatEffects;
    settings.visual.terrain.showTrees = profile.showTrees;
    settings.visual.device.dynamicLodEnabled = profile.dynamicLodEnabled;
    settings.visual.device.fpsLimitEnabled = profile.fpsLimitEnabled;
    settings.visual.device.textureReductionFactor =
        profile.textureReductionFactor;
}

void refreshCustomLodProfile(RenderGameDataSettings& settings) {
    RenderGameLodProfile& custom =
        settings.lodProfiles[static_cast<size_t>(RenderStaticLod::Custom)];
    custom.maximumParticles = settings.operational.maximumParticles;
    custom.useShadowVolumes = settings.visual.shadows.useVolumes;
    custom.useShadowDecals = settings.visual.shadows.useDecals;
    custom.useCloudMap = settings.visual.terrain.useCloudMap;
    custom.useLightMap = settings.visual.terrain.useLightMap;
    custom.showSoftWaterEdge = settings.visual.water.showSoftEdge;
    custom.useBuildupScaffolds =
        settings.visual.terrain.useBuildupScaffolds;
    custom.useTreeSway = settings.visual.terrain.useTreeSway;
    custom.useHeatEffects = settings.visual.terrain.useHeatEffects;
    custom.dynamicLodEnabled = settings.visual.device.dynamicLodEnabled;
    custom.fpsLimitEnabled = settings.visual.device.fpsLimitEnabled;
    custom.showTrees = settings.visual.terrain.showTrees;
    custom.textureReductionFactor =
        settings.visual.device.textureReductionFactor;
}

void applyLodField(container::StringView authoredKey,
                   container::StringView value, RenderGameLodProfile& profile,
                   container::Vector<container::String>* diagnostics) {
    const container::String key = lowerAscii(authoredKey);
    if (key == "maxparticlecount")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumParticles,
            profile.maximumParticles, diagnostics);
    else if (key == "useshadowvolumes")
        applyParsedField(authoredKey, value, profile.useShadowVolumes,
                         parseBool, diagnostics);
    else if (key == "useshadowdecals")
        applyParsedField(authoredKey, value, profile.useShadowDecals,
                         parseBool, diagnostics);
    else if (key == "usecloudmap")
        applyParsedField(authoredKey, value, profile.useCloudMap,
                         parseBool, diagnostics);
    else if (key == "uselightmap")
        applyParsedField(authoredKey, value, profile.useLightMap,
                         parseBool, diagnostics);
    else if (key == "showsoftwateredge")
        applyParsedField(authoredKey, value, profile.showSoftWaterEdge,
                         parseBool, diagnostics);
    else if (key == "usebuildupscaffolds")
        applyParsedField(authoredKey, value, profile.useBuildupScaffolds,
                         parseBool, diagnostics);
    else if (key == "usetreesway")
        applyParsedField(authoredKey, value, profile.useTreeSway,
                         parseBool, diagnostics);
    else if (key == "useheateffects")
        applyParsedField(authoredKey, value, profile.useHeatEffects,
                         parseBool, diagnostics);
    else if (key == "dynamiclod")
        applyParsedField(authoredKey, value, profile.dynamicLodEnabled,
                         parseBool, diagnostics);
    else if (key == "fpslimit")
        applyParsedField(authoredKey, value, profile.fpsLimitEnabled,
                         parseBool, diagnostics);
    else if (key == "showtrees")
        applyParsedField(authoredKey, value, profile.showTrees,
                         parseBool, diagnostics);
    else if (key == "texturereductionfactor")
        applyUnsignedField(authoredKey, value,
            render_game_data_limits::kMaximumTextureReductionFactor,
            profile.textureReductionFactor, diagnostics);
}

void applyDynamicLodField(
    container::StringView authoredKey, container::StringView value,
    RenderDynamicLodProfile& profile,
    container::Vector<container::String>* diagnostics) {
    const container::String key = lowerAscii(authoredKey);
    if (key == "minimumfps")
        applyUnsignedField(authoredKey, value, 1000,
            profile.minimumFramesPerSecond, diagnostics);
    else if (key == "particleskipmask")
        applyUnsignedField(authoredKey, value,
            std::numeric_limits<uint32_t>::max(), profile.particleSkipMask,
            diagnostics);
    else if (key == "debrisskipmask")
        applyUnsignedField(authoredKey, value,
            std::numeric_limits<uint32_t>::max(), profile.debrisSkipMask,
            diagnostics);
    else if (key == "slowdeathscale")
        applyParsedField(authoredKey, value, profile.slowDeathScale,
            parseFloat, diagnostics);
    else if (key == "minparticlepriority") {
        if (const auto parsed = parseParticlePriority(value))
            profile.minimumParticlePriority = *parsed;
        else
            diagnostic(diagnostics, "ignored malformed MinParticlePriority='" +
                container::String(value) + "'");
    } else if (key == "minparticleskippriority") {
        if (const auto parsed = parseParticlePriority(value))
            profile.minimumParticleSkipPriority = *parsed;
        else
            diagnostic(diagnostics,
                "ignored malformed MinParticleSkipPriority='" +
                container::String(value) + "'");
    }
}

} // namespace

RenderGameDataSettings::RenderGameDataSettings() noexcept {
    RenderGameLodProfile low;
    low.maximumParticles = 500;
    low.useShadowVolumes = false;
    low.useShadowDecals = false;
    low.useCloudMap = false;
    low.useLightMap = false;
    low.showSoftWaterEdge = false;
    low.useBuildupScaffolds = false;
    low.useTreeSway = false;
    low.useHeatEffects = false;
    low.textureReductionFactor = 1;
    lodProfiles[static_cast<size_t>(RenderStaticLod::Low)] = low;

    RenderGameLodProfile medium;
    medium.maximumParticles = 1500;
    medium.useShadowVolumes = false;
    medium.useShadowDecals = true;
    lodProfiles[static_cast<size_t>(RenderStaticLod::Medium)] = medium;

    const RenderGameLodProfile high{};
    lodProfiles[static_cast<size_t>(RenderStaticLod::High)] = high;
    lodProfiles[static_cast<size_t>(RenderStaticLod::VeryHigh)] = high;
    lodProfiles[static_cast<size_t>(RenderStaticLod::Custom)] = high;
}

bool applyRenderGameDataIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse GameData render settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (lowerAscii(block.type) != "gamedata") continue;
        for (const auto& [key, value] : block.values) {
            applyGameDataField(key, value, settings, diagnostics);
        }
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    refreshCustomLodProfile(settings);
    return true;
}

bool applyRenderMouseIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse Mouse input settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (lowerAscii(block.type) != "mouse") continue;
        for (const auto& [authoredKey, value] : block.values) {
            const container::String key = lowerAscii(authoredKey);
            if (key == "dragtolerance") {
                applyParsedField(
                    authoredKey, value,
                    settings.visual.input.dragTolerancePixels,
                    parseUnsigned32, diagnostics);
            } else if (key == "dragtolerance3d") {
                applyParsedField(
                    authoredKey, value,
                    settings.visual.input.dragToleranceWorldUnits,
                    parseUnsigned32, diagnostics);
            } else if (key == "dragtolerancems") {
                applyParsedField(
                    authoredKey, value,
                    settings.visual.input.dragToleranceMilliseconds,
                    parseUnsigned32, diagnostics);
            }
        }
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    return true;
}

bool applyRenderInGameUiIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse InGameUI caption settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (lowerAscii(block.type) != "ingameui") continue;
        for (const auto& [key, value] : block.values)
            applyInGameUiField(key, value, settings, diagnostics);
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    return true;
}

bool applyPresentationMiscAudioIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse MiscAudio presentation settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (lowerAscii(block.type) != "miscaudio") continue;
        for (const auto& [authoredKey, value] : block.values) {
            const container::String key = lowerAscii(authoredKey);
            container::String* destination = nullptr;
            if (key == "unitpromoted") {
                destination =
                    &settings.visual.objectFeedback.unitPromotedAudioEvent;
            } else if (key == "stealthdiscoveredsound") {
                destination = &settings.visual.objectFeedback.
                    stealthDiscoveredAudioEvent;
            } else if (key == "stealthneutralizedsound") {
                destination = &settings.visual.objectFeedback.
                    stealthNeutralizedAudioEvent;
            } else if (key == "radarnotifyinfiltrationsound") {
                destination = &settings.visual.objectFeedback.
                    radarInfiltrationAudioEvent;
            } else if (key == "radarnotifyunitunderattacksound") {
                destination = &settings.visual.objectFeedback.
                    radarUnitUnderAttackAudioEvent;
            } else if (key == "radarnotifyharvesterunderattacksound") {
                destination = &settings.visual.objectFeedback.
                    radarHarvesterUnderAttackAudioEvent;
            } else if (key == "radarnotifystructureunderattacksound") {
                destination = &settings.visual.objectFeedback.
                    radarStructureUnderAttackAudioEvent;
            } else if (key == "radarnotifyunderattacksound") {
                destination = &settings.visual.objectFeedback.
                    radarUnderAttackAudioEvent;
            } else if (key == "moneywithdrawsound") {
                destination = &settings.visual.objectFeedback.
                    moneyWithdrawAudioEvent;
            } else if (key == "sabotageshutdownbuilding") {
                destination = &settings.visual.objectFeedback.
                    sabotageShutdownAudioEvent;
            } else if (key == "sabotageresettimebuilding") {
                destination = &settings.visual.objectFeedback.
                    sabotageResetTimerAudioEvent;
            } else if (key == "defectortimerticksound") {
                destination = &settings.visual.objectFeedback.
                    defectorTimerTickAudioEvent;
            }
            if (!destination) continue;
            container::String parsed = trimAndUnquote(value);
            if (parsed.empty()) {
                diagnostic(diagnostics,
                    "ignored empty MiscAudio." + authoredKey);
            } else {
                *destination = std::move(parsed);
            }
        }
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    return true;
}

bool applyRenderLanguageIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse Language caption settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (lowerAscii(block.type) != "language") continue;
        for (const auto& [authoredKey, value] : block.values) {
            if (lowerAscii(authoredKey) != "drawablecaptionfont") continue;
            auto& feedback = settings.visual.objectFeedback;
            container::String name = feedback.drawableCaptionFont;
            int32_t pointSize = feedback.drawableCaptionPointSize;
            bool bold = feedback.drawableCaptionBold;
            if (!parseFontDescriptor(value, name, pointSize, bold)) {
                diagnostic(diagnostics,
                    "ignored malformed Language.DrawableCaptionFont");
                continue;
            }
            feedback.drawableCaptionFont = std::move(name);
            feedback.drawableCaptionPointSize = pointSize;
            feedback.drawableCaptionBold = bold;
        }
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    return true;
}

bool applyRenderGameLodIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse StaticGameLOD render settings");
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        const container::String blockType = lowerAscii(block.type);
        if (blockType == "staticgamelod") {
            const std::optional<RenderStaticLod> lod = parseLod(block.name);
            if (!lod) {
                diagnostic(diagnostics, "ignored unknown StaticGameLOD '" +
                    block.name + "'");
                continue;
            }
            RenderGameLodProfile& profile =
                settings.lodProfiles[static_cast<size_t>(*lod)];
            for (const auto& [key, value] : block.values)
                applyLodField(key, value, profile, diagnostics);
        } else if (blockType == "dynamicgamelod") {
            const std::optional<RenderDynamicLod> lod =
                parseDynamicLod(block.name);
            if (!lod) {
                diagnostic(diagnostics, "ignored unknown DynamicGameLOD '" +
                    block.name + "'");
                continue;
            }
            RenderDynamicLodProfile& profile =
                settings.dynamicLodProfiles[static_cast<size_t>(*lod)];
            for (const auto& [key, value] : block.values)
                applyDynamicLodField(key, value, profile, diagnostics);
        }
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
    return true;
}

RenderDisplayPresetSet renderDisplayPresetSetFromGameData(
    const RenderGameDataSettings& settings) noexcept {
    RenderDisplayPresetSet result;
    for (size_t index = 0; index < result.fpsLimitEnabled.size(); ++index) {
        result.fpsLimitEnabled[index] =
            settings.lodProfiles[index].fpsLimitEnabled;
    }
    return result;
}

RenderQualityPreferenceOverrides renderQualityPreferenceOverrides(
    const config::GraphPreferences& preferences,
    RenderStaticLod inheritedStaticLod,
    container::Vector<container::String>* diagnostics) {
    RenderQualityPreferenceOverrides result;
    RenderStaticLod selectedLod = inheritedStaticLod;
    const auto readBool = [&](container::StringView key,
                              std::optional<bool>& output) {
        if (!preferences.hasKey(container::String(key))) return;
        bool parsed = false;
        const container::String authored =
            preferences.getString(container::String(key));
        if (parseBool(authored, parsed)) {
            output = parsed;
        } else {
            diagnostic(diagnostics, "ignored malformed Options " +
                container::String(key) + "='" + authored + "'");
        }
    };
    const auto readInteger = [&](container::StringView key,
                                 int64_t& output) {
        const container::String authored =
            preferences.getString(container::String(key));
        if (parseSigned(authored, output)) return true;
        diagnostic(diagnostics, "ignored malformed Options " +
            container::String(key) + "='" + authored + "'");
        return false;
    };
    const auto readFloatPreference = [&](container::StringView key,
                                         std::optional<float>& output) {
        if (!preferences.hasKey(container::String(key))) return;
        const container::String authored =
            preferences.getString(container::String(key));
        float parsed = 0.0f;
        if (parseFloat(authored, parsed)) {
            output = parsed;
        } else {
            diagnostic(diagnostics, "ignored malformed Options " +
                container::String(key) + "='" + authored + "'");
        }
    };
    if (preferences.hasKey("StaticGameLOD")) {
        if (const std::optional<RenderStaticLod> parsed =
                parseLod(preferences.getStaticGameLOD())) {
            selectedLod = *parsed;
            result.feature.staticLod = *parsed;
        } else {
            diagnostic(diagnostics, "ignored unknown Options StaticGameLOD='" +
                preferences.getStaticGameLOD() + "'");
        }
    }

    // RefCode applies individual quality switches only when the user selected
    // CUSTOM. Named Low/Medium/High levels remain authoritative profiles.
    if (selectedLod == RenderStaticLod::Custom) {
        readBool("UseShadowVolumes", result.feature.useShadowVolumes);
        readBool("UseShadowDecals", result.feature.useShadowDecals);
        readBool("UseCloudMap", result.feature.useCloudMap);
        readBool("UseLightMap", result.feature.useLightMap);
        readBool("ShowTrees", result.feature.showTrees);
        readBool("ShowSoftWaterEdge", result.feature.showSoftWaterEdge);
        if (preferences.hasKey("ExtraAnimations")) {
            std::optional<bool> enabled;
            readBool("ExtraAnimations", enabled);
            if (enabled) {
                result.feature.extraAnimations = *enabled;
                result.feature.useDrawModuleLod = !*enabled;
                result.feature.useTreeSway = *enabled;
            }
        }
        readBool("HeatEffects", result.feature.useHeatEffects);
        readBool("BuildingOcclusion", result.feature.behindBuildingMarkers);
        if (preferences.hasKey("MaxParticleCount")) {
            int64_t authored = 0;
            if (readInteger("MaxParticleCount", authored)) {
                result.feature.maximumParticles = boundedUnsigned(
                    std::max<int64_t>(authored, 100),
                    render_game_data_limits::kMaximumParticles);
            }
        }
        if (preferences.hasKey("TextureReduction")) {
            int64_t authored = 0;
            if (readInteger("TextureReduction", authored) && authored >= 0) {
                result.feature.textureReductionFactor = boundedUnsigned(
                    std::min<int64_t>(authored, 2),
                    render_game_data_limits::kMaximumTextureReductionFactor);
            }
        }
        readBool("DynamicLOD", result.feature.dynamicLodEnabled);
        readBool("FPSLimit", result.display.fpsLimitEnabled);
    }
    if (preferences.hasKey("AntiAliasing")) {
        int64_t authored = 0;
        if (readInteger("AntiAliasing", authored)) {
            result.display.legacyAntiAliasing = highestPowerOfTwo(
                boundedUnsigned(authored, 8));
        }
    }
    const container::String finalAaKey = preferences.hasKey(
        "PostProcessAntiAliasing")
        ? "PostProcessAntiAliasing"
        : preferences.hasKey("AntiAliasingMode")
            ? "AntiAliasingMode" : container::String{};
    if (!finalAaKey.empty()) {
        const container::String authored = preferences.getString(finalAaKey);
        const container::String mode = lowerAscii(authored);
        if (mode == "off" || mode == "none" || mode == "0") {
            result.display.antiAliasingMode = RenderAntiAliasingMode::Off;
        } else if (mode == "fxaa") {
            result.display.antiAliasingMode = RenderAntiAliasingMode::Fxaa;
        } else {
            diagnostic(diagnostics, "ignored malformed Options " +
                finalAaKey + "='" + authored + "'");
        }
    }
    readFloatPreference("FxaaSubpixel", result.display.fxaaSubpixel);
    readFloatPreference(
        "FxaaEdgeThreshold", result.display.fxaaEdgeThreshold);
    readFloatPreference(
        "FxaaEdgeThresholdMin", result.display.fxaaEdgeThresholdMin);
    if (preferences.hasKey("TextureFilter")) {
        if (const auto parsed =
                parseTextureFilter(preferences.getString("TextureFilter"))) {
            result.display.textureFilter = *parsed;
        } else {
            diagnostic(diagnostics, "ignored malformed Options TextureFilter='" +
                preferences.getString("TextureFilter") + "'");
        }
    }
    if (preferences.hasKey("AnisotropyLevel")) {
        int64_t authored = 0;
        if (readInteger("AnisotropyLevel", authored)) {
            result.display.anisotropyLevel = highestPowerOfTwo(
                std::clamp<uint32_t>(boundedUnsigned(authored, 16), 2, 16));
        }
    }
    if (preferences.hasKey("Gamma")) {
        int64_t authored = 0;
        if (readInteger("Gamma", authored))
            result.display.gamma = boundedUnsigned(authored, 100);
    }
    if (preferences.hasKey("Resolution")) {
        uint32_t width = 0;
        uint32_t height = 0;
        if (parseResolution(preferences.getString("Resolution"), width, height)) {
            result.display.width = width;
            result.display.height = height;
        } else {
            diagnostic(diagnostics, "ignored malformed Options Resolution='" +
                preferences.getString("Resolution") + "'");
        }
    }
    return result;
}

void applyRenderOptions(
    const config::GraphPreferences& preferences,
    RenderGameDataSettings& settings,
    RenderQualitySettingsManager& manager,
    container::Vector<container::String>* diagnostics) {
    RenderQualityPreferenceOverrides overrides =
        renderQualityPreferenceOverrides(
            preferences, settings.selectedLod, diagnostics);
    // Loading GameLOD establishes one selected complete profile even if the
    // flat Options file omitted StaticGameLOD.
    if (!overrides.feature.staticLod)
        overrides.feature.staticLod = settings.selectedLod;
    manager.configure(
        renderFeaturePresetSetFromGameData(settings),
        renderDisplayPresetSetFromGameData(settings),
        renderFeatureQualityFromGameData(settings),
        renderDisplaySettingsFromGameData(settings),
        overrides.feature, overrides.display);
    const auto quality = manager.snapshot();
    projectRenderFeatureQualityToGameData(
        quality->feature.requested, settings);
    projectRenderDisplaySettingsToGameData(
        quality->display.effective, settings);

    auto& visual = settings.visual;
    auto& input = visual.input;
    auto& camera = visual.camera;
    if (preferences.hasKey("UseAlternateMouse"))
        input.useAlternateMouse = preferences.getBool("UseAlternateMouse");
    if (preferences.hasKey("UseRightMouseScrollWithAlternateMouse"))
        input.useRightMouseScrollWithAlternateMouse = preferences.getBool(
            "UseRightMouseScrollWithAlternateMouse");
    if (preferences.hasKey("RightMouseAlwaysScrolls"))
        input.rightMouseAlwaysScrolls = preferences.getBool(
            "RightMouseAlwaysScrolls");
    if (preferences.hasKey("UseDoubleClickAttackMove"))
        input.doubleClickAttackMove = preferences.getBool(
            "UseDoubleClickAttackMove");
    else if (preferences.hasKey("DoubleClickAttackMove"))
        input.doubleClickAttackMove = preferences.getBool(
            "DoubleClickAttackMove");
    if (preferences.hasKey("DrawScrollAnchor"))
        input.drawScrollAnchor = preferences.getBool("DrawScrollAnchor");
    if (preferences.hasKey("MoveScrollAnchor"))
        input.moveScrollAnchor = preferences.getBool("MoveScrollAnchor");
    if (preferences.hasKey("ScreenEdgeScrollEnabledInWindowedApp"))
        input.screenEdgeScrollWindowed = preferences.getBool(
            "ScreenEdgeScrollEnabledInWindowedApp");
    if (preferences.hasKey("ScreenEdgeScrollEnabledInFullscreenApp"))
        input.screenEdgeScrollFullscreen = preferences.getBool(
            "ScreenEdgeScrollEnabledInFullscreenApp");
    if (preferences.hasKey("CursorCaptureEnabledInWindowedGame"))
        input.cursorCaptureWindowedGame = preferences.getBool(
            "CursorCaptureEnabledInWindowedGame");
    if (preferences.hasKey("CursorCaptureEnabledInFullscreenGame"))
        input.cursorCaptureFullscreenGame = preferences.getBool(
            "CursorCaptureEnabledInFullscreenGame");
    if (preferences.hasKey("ScrollFactor")) {
        const int factor = std::max(1, preferences.getInt("ScrollFactor", 50));
        camera.keyboardScrollSpeedFactor =
            static_cast<float>(factor) / 100.0f;
    } else {
        camera.keyboardScrollSpeedFactor =
            camera.keyboardDefaultScrollSpeedFactor;
    }
    normalizeRenderGameDataSettings(settings, diagnostics);
}

void applyRenderOptions(
    const config::GraphPreferences& preferences,
    RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics) {
    RenderQualitySettingsManager manager;
    applyRenderOptions(preferences, settings, manager, diagnostics);
}

void normalizeRenderGameDataSettings(
    RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics) noexcept {
    auto finiteOr = [](float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    };
    auto& visual = settings.visual;
    auto& camera = visual.camera;
    camera.pitchDegrees = finiteOr(camera.pitchDegrees, 37.5f);
    camera.yawDegrees = finiteOr(camera.yawDegrees, 0.0f);
    camera.initialHeight = std::max(0.0f,
        finiteOr(camera.initialHeight, 232.0f));
    camera.minimumHeight = std::max(0.0f,
        finiteOr(camera.minimumHeight, 120.0f));
    camera.maximumHeight = std::max(camera.minimumHeight,
        finiteOr(camera.maximumHeight, 310.0f));
    camera.initialHeight = std::clamp(
        camera.initialHeight, camera.minimumHeight, camera.maximumHeight);
    camera.adjustSpeed = std::max(0.0f,
        finiteOr(camera.adjustSpeed, 0.3f));
    camera.scrollAmountCutoff = std::max(0.0f,
        finiteOr(camera.scrollAmountCutoff, 50.0f));
    camera.horizontalScrollSpeedFactor = std::max(0.0f,
        finiteOr(camera.horizontalScrollSpeedFactor, 1.6f));
    camera.verticalScrollSpeedFactor = std::max(0.0f,
        finiteOr(camera.verticalScrollSpeedFactor, 2.0f));
    camera.keyboardScrollSpeedFactor = std::max(0.01f,
        finiteOr(camera.keyboardScrollSpeedFactor, 2.0f));
    camera.keyboardDefaultScrollSpeedFactor = std::max(0.01f,
        finiteOr(camera.keyboardDefaultScrollSpeedFactor, 0.5f));
    visual.input.dragTolerancePixels = std::clamp<uint32_t>(
        visual.input.dragTolerancePixels, 1u, 4096u);
    visual.input.dragToleranceWorldUnits = std::clamp<uint32_t>(
        visual.input.dragToleranceWorldUnits, 1u, 1'000'000u);
    visual.input.dragToleranceMilliseconds = std::clamp<uint32_t>(
        visual.input.dragToleranceMilliseconds, 1u, 60'000u);
    camera.keyboardRotateSpeed = std::max(0.0f,
        finiteOr(camera.keyboardRotateSpeed, 0.1f));
    camera.viewportHeightScale = std::clamp(
        finiteOr(camera.viewportHeightScale, 0.8f), 0.1f, 1.0f);
    visual.water.extentX = std::max(0.0f,
        finiteOr(visual.water.extentX, 2000.0f));
    visual.water.extentY = std::max(0.0f,
        finiteOr(visual.water.extentY, 2000.0f));
    visual.water.skyBoxScale = std::max(0.01f,
        std::abs(finiteOr(visual.water.skyBoxScale, 8.4f)));
    visual.objectFeedback.selectionFlashSaturationFactor = std::max(0.0f,
        finiteOr(visual.objectFeedback.selectionFlashSaturationFactor, 0.5f));
    visual.objectFeedback.objectPlacementOpacity = std::clamp(
        finiteOr(visual.objectFeedback.objectPlacementOpacity, 0.45f),
        0.0f, 1.0f);
    visual.objectFeedback.levelGainAnimationDisplaySeconds = std::max(
        0.0f, finiteOr(
            visual.objectFeedback.levelGainAnimationDisplaySeconds, 0.0f));
    visual.objectFeedback.levelGainAnimationZRisePerSecond = std::max(
        0.0f, finiteOr(
            visual.objectFeedback.levelGainAnimationZRisePerSecond, 0.0f));
    visual.objectFeedback.floatingTextMoveUpPerSecond = std::max(
        0.0f, finiteOr(
            visual.objectFeedback.floatingTextMoveUpPerSecond, 30.0f));
    visual.objectFeedback.floatingTextVanishPerSecond = std::max(
        0.0f, finiteOr(
            visual.objectFeedback.floatingTextVanishPerSecond, 3.0f));
    if (visual.objectFeedback.drawableCaptionFont.empty())
        visual.objectFeedback.drawableCaptionFont = "Arial";
    visual.objectFeedback.drawableCaptionPointSize = std::clamp<int32_t>(
        visual.objectFeedback.drawableCaptionPointSize, 1, 256);
    visual.visibility.occludedLuminanceScale = std::clamp(
        finiteOr(visual.visibility.occludedLuminanceScale, 0.5f), 0.0f, 1.0f);
    visual.particleScale = std::max(0.0f,
        finiteOr(visual.particleScale, 1.0f));
    visual.controlBarPower.logarithmicBase = std::max(
        2, visual.controlBarPower.logarithmicBase);
    visual.controlBarPower.intervals = std::max(
        0.01f, finiteOr(visual.controlBarPower.intervals, 3.0f));
    visual.controlBarPower.yellowRange = std::max(
        0, visual.controlBarPower.yellowRange);
    const container::Array<RenderBodyParticleChannel*, 7>
        bodyParticleChannels{
            &visual.bodyParticles.fireSmall,
            &visual.bodyParticles.fireMedium,
            &visual.bodyParticles.fireLarge,
            &visual.bodyParticles.smokeSmall,
            &visual.bodyParticles.smokeMedium,
            &visual.bodyParticles.smokeLarge,
            &visual.bodyParticles.aflame,
        };
    for (RenderBodyParticleChannel* channel : bodyParticleChannels) {
        channel->maximumSystems = std::min(
            channel->maximumSystems, kMaximumAutoBodyParticleBones);
    }
    for (RenderVertexWaterGameData& water : visual.water.vertexWater) {
        water.heightClampLow = finiteOr(water.heightClampLow, 0.0f);
        water.heightClampHigh = std::max(water.heightClampLow,
            finiteOr(water.heightClampHigh, 100.0f));
        water.angleRadians = finiteOr(water.angleRadians, 0.0f);
        water.positionX = finiteOr(water.positionX, 0.0f);
        water.positionY = finiteOr(water.positionY, 0.0f);
        water.positionZ = finiteOr(water.positionZ, 0.0f);
        water.gridCellsX = std::clamp<uint32_t>(
            water.gridCellsX, 1,
            water_surface::performance_limits::
                kMaximumVertexWaterGridCellsPerAxis);
        water.gridCellsY = std::clamp<uint32_t>(
            water.gridCellsY, 1,
            water_surface::performance_limits::
                kMaximumVertexWaterGridCellsPerAxis);
        water.gridSize = std::max(0.01f, finiteOr(water.gridSize, 10.0f));
        water.attenuationA = finiteOr(water.attenuationA, 0.0f);
        water.attenuationB = finiteOr(water.attenuationB, 1.0f);
        water.attenuationC = finiteOr(water.attenuationC, 0.0f);
        water.attenuationRange = std::max(0.0f,
            finiteOr(water.attenuationRange, 100.0f));
    }

    auto& budget = settings.operational;
    budget.maximumParticles = std::min(
        budget.maximumParticles, render_game_data_limits::kMaximumParticles);
    budget.maximumFieldParticles = std::min(
        budget.maximumFieldParticles,
        render_game_data_limits::kMaximumFieldParticles);
    budget.maximumTerrainTracks = std::min(
        budget.maximumTerrainTracks,
        render_game_data_limits::kMaximumTerrainTracks);
    auto& compatibility = settings.compatibility;
    compatibility.maximumRoadSegments = std::min(
        compatibility.maximumRoadSegments,
        render_game_data_limits::kMaximumRoadSegments);
    compatibility.maximumRoadVertices = std::min(
        compatibility.maximumRoadVertices,
        render_game_data_limits::kMaximumRoadVertices);
    compatibility.maximumRoadIndices = std::min(
        compatibility.maximumRoadIndices,
        render_game_data_limits::kMaximumRoadIndices);
    compatibility.maximumRoadTypes = std::min(
        compatibility.maximumRoadTypes,
        render_game_data_limits::kMaximumRoadTypes);
    compatibility.maximumVisibleTranslucentObjects = std::min(
        compatibility.maximumVisibleTranslucentObjects,
        render_game_data_limits::kMaximumVisibleObjectsPerClass);
    compatibility.maximumVisibleOccluderObjects = std::min(
        compatibility.maximumVisibleOccluderObjects,
        render_game_data_limits::kMaximumVisibleObjectsPerClass);
    compatibility.maximumVisibleOccludeeObjects = std::min(
        compatibility.maximumVisibleOccludeeObjects,
        render_game_data_limits::kMaximumVisibleObjectsPerClass);
    compatibility.maximumVisibleOtherObjects = std::min(
        compatibility.maximumVisibleOtherObjects,
        render_game_data_limits::kMaximumVisibleObjectsPerClass);
    budget.modelUploadsPerFrame = std::clamp<uint32_t>(
        budget.modelUploadsPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadsPerFrame);
    budget.modelUploadsPerLoadingFrame = std::clamp<uint32_t>(
        budget.modelUploadsPerLoadingFrame, 1,
        render_game_data_limits::kMaximumModelUploadsPerFrame);
    budget.modelUploadBytesPerFrame = std::clamp<uint64_t>(
        budget.modelUploadBytesPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadBytesPerFrame);
    budget.modelUploadBytesPerLoadingFrame = std::clamp<uint64_t>(
        budget.modelUploadBytesPerLoadingFrame, 1,
        render_game_data_limits::kMaximumModelUploadBytesPerFrame);
    budget.modelUploadMicrosecondsPerFrame = std::clamp<uint32_t>(
        budget.modelUploadMicrosecondsPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadMicrosecondsPerFrame);
    budget.modelUploadMicrosecondsPerLoadingFrame = std::clamp<uint32_t>(
        budget.modelUploadMicrosecondsPerLoadingFrame, 1,
        render_game_data_limits::kMaximumModelUploadMicrosecondsPerFrame);
    budget.initialParticleEmitterCapacity = std::min(
        budget.initialParticleEmitterCapacity,
        render_game_data_limits::kMaximumParticleEmitters);
    budget.maximumParticleEmitters = std::min(
        budget.maximumParticleEmitters,
        render_game_data_limits::kMaximumParticleEmitters);
    budget.initialParticleEmitterCapacity = std::min(
        budget.initialParticleEmitterCapacity,
        budget.maximumParticleEmitters);
    budget.maximumAttachedFxEmitters = std::min(
        budget.maximumAttachedFxEmitters,
        render_game_data_limits::kMaximumAttachedFxEmitters);
    budget.maximumFxPresentationCommands = std::min(
        budget.maximumFxPresentationCommands,
        render_game_data_limits::kMaximumFxPresentationCommands);
    budget.maximumGroundProjectorsPerFrame = std::min(
        budget.maximumGroundProjectorsPerFrame,
        render_game_data_limits::kMaximumGroundProjectorsPerFrame);
    budget.maximumGroundProjectorTextures = std::min(
        budget.maximumGroundProjectorTextures,
        render_game_data_limits::kMaximumGroundProjectorTextures);
    budget.particleDrawExpansionFactor = std::clamp<uint32_t>(
        budget.particleDrawExpansionFactor, 1,
        render_game_data_limits::kMaximumParticleDrawExpansionFactor);
    for (RenderGameLodProfile& profile : settings.lodProfiles) {
        profile.maximumParticles = std::min(profile.maximumParticles,
            render_game_data_limits::kMaximumParticles);
        profile.textureReductionFactor = std::min(
            profile.textureReductionFactor,
            render_game_data_limits::kMaximumTextureReductionFactor);
    }
    for (RenderDynamicLodProfile& profile : settings.dynamicLodProfiles) {
        profile.slowDeathScale = std::max(0.0f,
            finiteOr(profile.slowDeathScale, 1.0f));
    }
    visual.device.width = std::clamp<uint32_t>(
        visual.device.width, 1,
        render_game_data_limits::kMaximumRenderDimension);
    visual.device.height = std::clamp<uint32_t>(
        visual.device.height, 1,
        render_game_data_limits::kMaximumRenderDimension);
    visual.device.textureReductionFactor = std::min(
        visual.device.textureReductionFactor,
        render_game_data_limits::kMaximumTextureReductionFactor);
    visual.device.displayGamma = std::clamp(
        finiteOr(visual.device.displayGamma,
            displayGammaFromOption(visual.device.gamma)),
        0.6f, 2.0f);

    // GameLOD.ini and legacy options remain importable data, not a runtime
    // permission to remove authored visuals.  Normalize every direct consumer
    // to the one fixed full-detail policy as well as enforcing it again in the
    // quality resolver below.
    settings.selectedLod = render_lod_policy::kStaticProfile;
    settings.selectedDynamicLod = render_lod_policy::kDynamicProfile;
    visual.terrain.lod = render_lod_policy::kTerrainProfile;
    visual.terrain.useDrawModuleLod =
        render_lod_policy::kDrawModuleLodEnabled;
    visual.device.dynamicLodEnabled =
        render_lod_policy::kDynamicLodEnabled;
    static_cast<void>(diagnostics);
}

RenderFeatureQualitySettings renderFeatureQualityFromGameData(
    const RenderGameDataSettings& settings) noexcept {
    const auto& visual = settings.visual;
    return {
        .staticLod = settings.selectedLod,
        .dynamicLod = settings.selectedDynamicLod,
        .terrainLod = visual.terrain.lod,
        .maximumParticles = settings.operational.maximumParticles,
        .textureReductionFactor = visual.device.textureReductionFactor,
        .useShadowVolumes = visual.shadows.useVolumes,
        .useShadowDecals = visual.shadows.useDecals,
        .useCloudMap = visual.terrain.useCloudMap,
        .useLightMap = visual.terrain.useLightMap,
        .showSoftWaterEdge = visual.water.showSoftEdge,
        .showTrees = visual.terrain.showTrees,
        .useTreeSway = visual.terrain.useTreeSway,
        .useBuildupScaffolds = visual.terrain.useBuildupScaffolds,
        .extraAnimations = visual.terrain.extraAnimations,
        .useDrawModuleLod = visual.terrain.useDrawModuleLod,
        .useHeatEffects = visual.terrain.useHeatEffects,
        .behindBuildingMarkers = visual.visibility.behindBuildingMarkers,
        .dynamicLodEnabled = visual.device.dynamicLodEnabled,
    };
}

RenderFeaturePresetSet renderFeaturePresetSetFromGameData(
    const RenderGameDataSettings& settings) noexcept {
    RenderFeaturePresetSet result;
    const auto authoredBase = renderFeatureQualityFromGameData(settings);
    result.engineSafe = RenderFeatureQualitySettings{};
    for (size_t index = 0; index < result.profiles.size(); ++index) {
        const RenderGameLodProfile& profile = settings.lodProfiles[index];
        auto quality = authoredBase;
        quality.staticLod = static_cast<RenderStaticLod>(index);
        quality.maximumParticles = std::min(profile.maximumParticles,
            render_game_data_limits::kMaximumParticles);
        quality.textureReductionFactor = std::min(
            profile.textureReductionFactor,
            render_game_data_limits::kMaximumTextureReductionFactor);
        quality.useShadowVolumes = profile.useShadowVolumes;
        quality.useShadowDecals = profile.useShadowDecals;
        quality.useCloudMap = profile.useCloudMap;
        quality.useLightMap = profile.useLightMap;
        quality.showSoftWaterEdge = profile.showSoftWaterEdge;
        quality.showTrees = profile.showTrees;
        quality.useTreeSway = profile.useTreeSway;
        quality.useBuildupScaffolds = profile.useBuildupScaffolds;
        quality.extraAnimations = profile.useBuildupScaffolds;
        quality.useDrawModuleLod = !profile.useBuildupScaffolds;
        quality.useHeatEffects = profile.useHeatEffects;
        quality.dynamicLodEnabled = profile.dynamicLodEnabled;
        result.profiles[index] = quality;
    }
    return result;
}

void projectRenderFeatureQualityToGameData(
    const RenderFeatureQualitySettings& quality,
    RenderGameDataSettings& settings) noexcept {
    settings.selectedLod = quality.staticLod;
    settings.selectedDynamicLod = quality.dynamicLod;
    settings.operational.maximumParticles = quality.maximumParticles;
    auto& visual = settings.visual;
    visual.device.textureReductionFactor = quality.textureReductionFactor;
    visual.device.dynamicLodEnabled = quality.dynamicLodEnabled;
    visual.shadows.useVolumes = quality.useShadowVolumes;
    visual.shadows.useDecals = quality.useShadowDecals;
    visual.terrain.lod = quality.terrainLod;
    visual.terrain.useCloudMap = quality.useCloudMap;
    visual.terrain.useLightMap = quality.useLightMap;
    visual.terrain.showTrees = quality.showTrees;
    visual.terrain.useTreeSway = quality.useTreeSway;
    visual.terrain.useBuildupScaffolds = quality.useBuildupScaffolds;
    visual.terrain.extraAnimations = quality.extraAnimations;
    visual.terrain.useDrawModuleLod = quality.useDrawModuleLod;
    visual.terrain.useHeatEffects = quality.useHeatEffects;
    visual.water.showSoftEdge = quality.showSoftWaterEdge;
    visual.visibility.behindBuildingMarkers = quality.behindBuildingMarkers;
}

RenderDisplaySettings renderDisplaySettingsFromGameData(
    const RenderGameDataSettings& settings) noexcept {
    const auto& device = settings.visual.device;
    return {
        .width = device.width,
        .height = device.height,
        .textureFilter = device.textureFilter,
        .anisotropyLevel = device.anisotropyLevel,
        .gamma = device.gamma,
        .displayGamma = device.displayGamma,
        .legacyAntiAliasing = device.antiAliasing,
        .fpsLimitEnabled = device.fpsLimitEnabled,
    };
}

void projectRenderDisplaySettingsToGameData(
    const RenderDisplaySettings& display,
    RenderGameDataSettings& settings) noexcept {
    auto& device = settings.visual.device;
    device.width = display.width;
    device.height = display.height;
    device.textureFilter = display.textureFilter;
    device.anisotropyLevel = display.anisotropyLevel;
    device.gamma = display.gamma;
    device.displayGamma = display.displayGamma;
    device.antiAliasing = display.legacyAntiAliasing;
    device.fpsLimitEnabled = display.fpsLimitEnabled;
}

void applyRenderFeatureQualityOverrides(
    RenderFeatureQualitySettings& settings,
    const RenderFeatureQualityOverrides& overrides) noexcept {
#define APPLY_FEATURE(field) if (overrides.field) settings.field = *overrides.field
    APPLY_FEATURE(staticLod);
    APPLY_FEATURE(dynamicLod);
    APPLY_FEATURE(terrainLod);
    APPLY_FEATURE(maximumParticles);
    APPLY_FEATURE(textureReductionFactor);
    APPLY_FEATURE(useShadowVolumes);
    APPLY_FEATURE(useShadowDecals);
    APPLY_FEATURE(useCloudMap);
    APPLY_FEATURE(useLightMap);
    APPLY_FEATURE(showSoftWaterEdge);
    APPLY_FEATURE(showTrees);
    APPLY_FEATURE(useTreeSway);
    APPLY_FEATURE(useBuildupScaffolds);
    APPLY_FEATURE(extraAnimations);
    APPLY_FEATURE(useDrawModuleLod);
    APPLY_FEATURE(useHeatEffects);
    APPLY_FEATURE(behindBuildingMarkers);
    APPLY_FEATURE(dynamicLodEnabled);
    APPLY_FEATURE(particleSimulationBackend);
#undef APPLY_FEATURE
}

ResolvedRenderFeatureSnapshot resolveRenderFeatureQuality(
    const RenderFeatureQualitySettings& base,
    const RenderFeatureQualityOverrides& overrides,
    uint64_t revision) noexcept {
    ResolvedRenderFeatureSnapshot result{.requested = base, .revision = revision};
    applyRenderFeatureQualityOverrides(result.requested, overrides);
    auto& quality = result.requested;
    quality.staticLod = render_lod_policy::kStaticProfile;
    quality.dynamicLod = render_lod_policy::kDynamicProfile;
    quality.terrainLod = render_lod_policy::kTerrainProfile;
    quality.useDrawModuleLod = render_lod_policy::kDrawModuleLodEnabled;
    quality.dynamicLodEnabled =
        render_lod_policy::kDynamicLodEnabled;
    if (quality.particleSimulationBackend >
        RenderParticleSimulationBackend::GpuCompute)
        quality.particleSimulationBackend = RenderParticleSimulationBackend::Cpu;
    quality.maximumParticles = render_game_data_limits::kMaximumParticles;
    quality.textureReductionFactor = 0;
    return result;
}

void applyRenderDisplayOverrides(
    RenderDisplaySettings& settings,
    const RenderDisplayOverrides& overrides) noexcept {
#define APPLY_DISPLAY(field) if (overrides.field) settings.field = *overrides.field
    APPLY_DISPLAY(width);
    APPLY_DISPLAY(height);
    APPLY_DISPLAY(refreshRateHz);
    APPLY_DISPLAY(textureFilter);
    APPLY_DISPLAY(anisotropyLevel);
    if (overrides.gamma) {
        settings.gamma = *overrides.gamma;
        if (!overrides.displayGamma)
            settings.displayGamma = displayGammaFromOption(settings.gamma);
    }
    APPLY_DISPLAY(displayGamma);
    APPLY_DISPLAY(legacyAntiAliasing);
    APPLY_DISPLAY(displayMode);
    APPLY_DISPLAY(antiAliasingMode);
    APPLY_DISPLAY(fxaaSubpixel);
    APPLY_DISPLAY(fxaaEdgeThreshold);
    APPLY_DISPLAY(fxaaEdgeThresholdMin);
    APPLY_DISPLAY(verticalSync);
    APPLY_DISPLAY(fpsLimitEnabled);
#undef APPLY_DISPLAY
}

RenderDisplayChangeMask renderDisplayChangeMask(
    const RenderDisplaySettings& before,
    const RenderDisplaySettings& after) noexcept {
    RenderDisplayChangeMask result = RenderDisplayChangeMask::None;
    if (before.width != after.width || before.height != after.height)
        result |= RenderDisplayChangeMask::Resolution;
    if (before.refreshRateHz != after.refreshRateHz ||
        before.displayMode != after.displayMode)
        result |= RenderDisplayChangeMask::OutputMode;
    if (before.verticalSync != after.verticalSync ||
        before.fpsLimitEnabled != after.fpsLimitEnabled)
        result |= RenderDisplayChangeMask::FramePacing;
    if (before.antiAliasingMode != after.antiAliasingMode ||
        before.fxaaSubpixel != after.fxaaSubpixel ||
        before.fxaaEdgeThreshold != after.fxaaEdgeThreshold ||
        before.fxaaEdgeThresholdMin != after.fxaaEdgeThresholdMin ||
        before.legacyAntiAliasing != after.legacyAntiAliasing)
        result |= RenderDisplayChangeMask::AntiAliasing;
    if (before.textureFilter != after.textureFilter ||
        before.anisotropyLevel != after.anisotropyLevel)
        result |= RenderDisplayChangeMask::TextureSampling;
    if (before.gamma != after.gamma ||
        before.displayGamma != after.displayGamma)
        result |= RenderDisplayChangeMask::Gamma;
    return result;
}

ResolvedRenderDisplaySnapshot resolveRenderDisplaySettings(
    const RenderDisplaySettings& base,
    const RenderDisplayOverrides& overrides,
    const RenderDisplayCapabilities& capabilities,
    const RenderDisplaySettings* previousEffective,
    uint64_t revision) noexcept {
    ResolvedRenderDisplaySnapshot result{
        .requested = base, .effective = base, .revision = revision};
    applyRenderDisplayOverrides(result.requested, overrides);
    auto normalize = [](RenderDisplaySettings& value) noexcept {
        value.width = std::clamp(value.width, 1u,
            render_game_data_limits::kMaximumRenderDimension);
        value.height = std::clamp(value.height, 1u,
            render_game_data_limits::kMaximumRenderDimension);
        value.textureFilter = std::min(value.textureFilter, 4u);
        value.anisotropyLevel = highestPowerOfTwo(
            std::clamp(value.anisotropyLevel, 2u, 16u));
        value.gamma = std::min(value.gamma, 100u);
        if (!std::isfinite(value.displayGamma))
            value.displayGamma = displayGammaFromOption(value.gamma);
        value.displayGamma = std::clamp(value.displayGamma, 0.6f, 2.0f);
        value.legacyAntiAliasing = highestPowerOfTwo(
            std::min(value.legacyAntiAliasing, 8u));
        if (value.displayMode > RenderDisplayMode::ExclusiveFullscreen)
            value.displayMode = RenderDisplayMode::Windowed;
        if (value.antiAliasingMode > RenderAntiAliasingMode::Fxaa)
            value.antiAliasingMode = RenderAntiAliasingMode::Off;
        if (!std::isfinite(value.fxaaSubpixel)) value.fxaaSubpixel = 0.75f;
        if (!std::isfinite(value.fxaaEdgeThreshold))
            value.fxaaEdgeThreshold = 0.166f;
        if (!std::isfinite(value.fxaaEdgeThresholdMin))
            value.fxaaEdgeThresholdMin = 0.0833f;
        value.fxaaSubpixel = std::clamp(value.fxaaSubpixel, 0.0f, 1.0f);
        value.fxaaEdgeThreshold = std::clamp(
            value.fxaaEdgeThreshold, 0.0312f, 0.333f);
        value.fxaaEdgeThresholdMin = std::clamp(
            value.fxaaEdgeThresholdMin, 0.0f, 0.125f);
    };
    normalize(result.requested);
    result.effective = result.requested;
    const uint32_t maximumWidth = std::clamp(capabilities.maximumWidth, 1u,
        render_game_data_limits::kMaximumRenderDimension);
    const uint32_t maximumHeight = std::clamp(capabilities.maximumHeight, 1u,
        render_game_data_limits::kMaximumRenderDimension);
    if (result.effective.width > maximumWidth ||
        result.effective.height > maximumHeight) {
        result.effective.width = std::min(result.effective.width, maximumWidth);
        result.effective.height = std::min(result.effective.height, maximumHeight);
        result.fallbackMask |= RenderDisplayFallbackMask::ResolutionClamped;
    }
    if (result.effective.textureFilter == 4u) {
        const uint32_t maximumAnisotropy = highestPowerOfTwo(
            std::clamp(capabilities.maximumAnisotropy, 1u, 16u));
        if (result.effective.anisotropyLevel > maximumAnisotropy) {
            result.effective.anisotropyLevel = maximumAnisotropy;
            result.fallbackMask |= RenderDisplayFallbackMask::AnisotropyClamped;
        }
    }
    if (result.effective.antiAliasingMode == RenderAntiAliasingMode::Fxaa &&
        !capabilities.supportsFxaa) {
        result.effective.antiAliasingMode = RenderAntiAliasingMode::Off;
        result.fallbackMask |= RenderDisplayFallbackMask::FxaaUnavailable;
    }
    const auto applyDesktopMode = [&]() noexcept {
        if (capabilities.desktopWidth == 0u ||
            capabilities.desktopHeight == 0u) {
            return;
        }
        if (result.effective.width != capabilities.desktopWidth ||
            result.effective.height != capabilities.desktopHeight ||
            (capabilities.desktopRefreshRateHz != 0u &&
             result.effective.refreshRateHz !=
                 capabilities.desktopRefreshRateHz)) {
            result.fallbackMask |=
                RenderDisplayFallbackMask::DesktopModeApplied;
        }
        result.effective.width = capabilities.desktopWidth;
        result.effective.height = capabilities.desktopHeight;
        if (capabilities.desktopRefreshRateHz != 0u) {
            result.effective.refreshRateHz =
                capabilities.desktopRefreshRateHz;
        }
    };
    if (result.effective.displayMode ==
            RenderDisplayMode::ExclusiveFullscreen &&
        !capabilities.supportsExclusiveFullscreen) {
        result.effective.displayMode =
            capabilities.supportsBorderlessFullscreen
            ? RenderDisplayMode::BorderlessFullscreen
            : RenderDisplayMode::Windowed;
        result.fallbackMask |=
            RenderDisplayFallbackMask::OutputModeUnavailable;
    }
    if (result.effective.displayMode ==
        RenderDisplayMode::BorderlessFullscreen) {
        if (!capabilities.supportsBorderlessFullscreen) {
            result.effective.displayMode = RenderDisplayMode::Windowed;
            result.fallbackMask |=
                RenderDisplayFallbackMask::OutputModeUnavailable;
        } else {
            // Borderless ignores a requested mode switch and always tracks
            // the desktop output. Record that platform-effective extent
            // before the renderer publishes the independently observed
            // applied/pixel state.
            applyDesktopMode();
        }
    }
    if (previousEffective) {
        auto previous = *previousEffective;
        normalize(previous);
        result.changeMask = renderDisplayChangeMask(previous, result.effective);
    }
    return result;
}

} // namespace engine
