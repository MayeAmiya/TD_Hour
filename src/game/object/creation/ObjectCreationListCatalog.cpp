#include "core/container/hash_containers.h"
#include "ObjectCreationListCatalog.h"

#include "LegacyIniDirectory.h"
#include "VFS.h"
#include "debug/debug.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "game/data/base/ContentDiagnostics.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <utility>

namespace game {
namespace {

[[nodiscard]] container::StringView trimView(container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::String canonical(container::StringView value) {
    value = trimView(value);
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : static_cast<char>(character);
        result.push_back(folded == '\\' ? '/' : folded);
    }
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

[[nodiscard]] bool equalInsensitive(container::StringView left,
                                    container::StringView right) {
    return canonical(left) == canonical(right);
}

[[nodiscard]] bool isWeaponSlot(container::StringView value) {
    const container::String slot = canonical(value);
    return slot == "primary" || slot == "secondary" ||
        slot == "tertiary";
}

struct OclCompileState final {
    container::StringView source;
    container::StringView definition;
    container::StringView module;
    bool failed = false;
};

thread_local OclCompileState* g_oclCompileState = nullptr;

void warnOcl(container::String reason, container::String field = {},
             container::String rawValue = {},
             container::String adoptedValue = "affected branch disabled/no-op") {
    processContentDiagnostics().warn({
        .source = g_oclCompileState
            ? container::String{g_oclCompileState->source}
            : "ObjectCreationListCatalog",
        .block = "ObjectCreationList",
        .definition = g_oclCompileState
            ? container::String{g_oclCompileState->definition}
            : container::String{},
        .module = g_oclCompileState
            ? container::String{g_oclCompileState->module}
            : container::String{},
        .field = std::move(field),
        .rawValue = std::move(rawValue),
        .adoptedValue = std::move(adoptedValue),
        .reason = std::move(reason),
    });
}

void failOclField(container::String reason, container::String field,
                  container::StringView rawValue) {
    if (g_oclCompileState) g_oclCompileState->failed = true;
    warnOcl(std::move(reason), std::move(field),
            container::String{rawValue});
}

void failOclDefinition(container::String reason,
                       container::String rawValue = {}) {
    if (g_oclCompileState) g_oclCompileState->failed = true;
    warnOcl(std::move(reason), {}, std::move(rawValue));
}

class OclCompileScope final {
public:
    explicit OclCompileScope(OclCompileState& state) noexcept
        : m_previous(g_oclCompileState) {
        g_oclCompileState = &state;
    }
    ~OclCompileScope() { g_oclCompileState = m_previous; }

private:
    OclCompileState* m_previous = nullptr;
};

[[nodiscard]] bool parseBoolean(container::StringView value,
                                bool fallback = false) {
    const container::String normalized = canonical(value);
    if (normalized == "yes" || normalized == "true" ||
        normalized == "on" || normalized == "1") {
        return true;
    }
    if (normalized == "no" || normalized == "false" ||
        normalized == "off" || normalized == "0") {
        return false;
    }
    failOclField("boolean field has no recognized Yes/No value",
                 "Boolean", value);
    return fallback;
}

[[nodiscard]] float parseFloat(container::StringView value,
                               float fallback = 0.0f) {
    const container::FiniteFloatParseResult parsed =
        container::parseFiniteFloatCompatible(value);
    if (!parsed.accepted()) {
        failOclField(
            parsed.status == container::FiniteFloatParseStatus::NoNumericPrefix
                ? "real field has no numeric prefix"
                : "real field is NaN, infinity or out of range",
            "Real", value);
        return fallback;
    }
    return parsed.value;
}

[[nodiscard]] uint32_t parseUnsigned(container::StringView value,
                                     uint32_t fallback = 0) {
    value = trimView(value);
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        failOclField("unsigned integer field is malformed or out of range",
                     "UnsignedInt", value);
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] int32_t parseSigned(container::StringView value,
                                  int32_t fallback = 0) {
    value = trimView(value);
    int32_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        failOclField("signed integer field is malformed or out of range",
                     "Int", value);
        return fallback;
    }
    return parsed;
}

[[nodiscard]] math::q32_32 fixedFromFloat(float value) {
    if (!std::isfinite(value)) {
        failOclField("fixed-point field is non-finite", "Fixed",
                     std::to_string(value));
        return {};
    }
    constexpr float minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    constexpr float maximum =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    if (value < minimum) {
        failOclField("fixed-point field is below Q32.32 range", "Fixed",
                     std::to_string(value));
        return math::q32_32::from_raw(std::numeric_limits<int64_t>::min());
    }
    if (value >= maximum) {
        failOclField("fixed-point field exceeds Q32.32 range", "Fixed",
                     std::to_string(value));
        return math::q32_32::from_raw(std::numeric_limits<int64_t>::max());
    }
    return math::q32_32{value};
}

[[nodiscard]] math::q32_32 parseFixed(container::StringView value,
                                      float fallback = 0.0f) {
    return fixedFromFloat(parseFloat(value, fallback));
}

[[nodiscard]] math::q32_32 parseAngle(container::StringView value,
                                      float fallbackDegrees = 0.0f) {
    return fixedFromFloat(parseFloat(value, fallbackDegrees) *
                          (std::numbers::pi_v<float> / 180.0f));
}

[[nodiscard]] math::q32_32 parsePercent(container::StringView value,
                                        float fallback = 1.0f) {
    value = trimView(value);
    if (!value.empty() && value.back() == '%') value.remove_suffix(1);
    return fixedFromFloat(parseFloat(value, fallback * 100.0f) / 100.0f);
}

[[nodiscard]] container::Vector<container::String> words(container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) result.emplace_back(value.substr(begin, cursor - begin));
    }
    if (result.size() == 1 && equalInsensitive(result.front(), "None")) {
        result.clear();
    }
    return result;
}

[[nodiscard]] uint32_t parseDecalShadowTypeMask(
    container::StringView value, uint32_t fallback = 0x20u) {
    uint32_t result = 0;
    bool found = false;
    for (container::String token : words(value)) {
        const container::String name = canonical(token);
        uint32_t bit = 0;
        if (name == "shadow_none" || name == "none") {
            return 0;
        } else if (name == "shadow_decal") bit = 0x01u;
        else if (name == "shadow_volume") bit = 0x02u;
        else if (name == "shadow_projection") bit = 0x04u;
        else if (name == "shadow_dynamic_projection") bit = 0x08u;
        else if (name == "shadow_directional_projection") bit = 0x10u;
        else if (name == "shadow_alpha_decal") bit = 0x20u;
        else if (name == "shadow_additive_decal") bit = 0x40u;
        else {
            failOclField("decal Shadow contains an unknown flag",
                         "Shadow", token);
            continue;
        }
        result |= bit;
        found = true;
    }
    return found ? result : fallback;
}

[[nodiscard]] uint8_t parseDebrisShadowTypeMask(
    container::StringView value) {
    uint8_t result = 0;
    for (const container::String& token : words(value)) {
        const container::String normalized = canonical(token);
        if (normalized == "shadow_decal") result |= 0x01u;
        else if (normalized == "shadow_volume") result |= 0x02u;
        else if (normalized == "shadow_projection") result |= 0x04u;
        else if (normalized == "shadow_dynamic_projection" ||
                 normalized == "shadow_dynamic_proj") result |= 0x08u;
        else if (normalized == "shadow_directional_projection") result |= 0x10u;
        else if (normalized == "shadow_alpha_decal") result |= 0x20u;
        else if (normalized == "shadow_additive_decal") result |= 0x40u;
        else failOclField("debris Shadow contains an unknown flag",
                          "Shadow", token);
    }
    return result;
}

[[nodiscard]] container::Array<uint8_t, 4> parseDecalColor(
    container::StringView value, container::Array<uint8_t, 4> fallback) {
    const container::Vector<container::String> parts = words(value);
    if (parts.size() < 3 || parts.size() > 4) {
        failOclField("decal Color must contain R/G/B and optional A",
                     "Color", value);
        return fallback;
    }
    constexpr container::Array<char, 4> names{'r', 'g', 'b', 'a'};
    container::Array<uint8_t, 4> result{0, 0, 0, 255};
    for (size_t index = 0; index < parts.size(); ++index) {
        const container::String& part = parts[index];
        if (part.size() < 3 || part[1] != ':' ||
            static_cast<char>(std::tolower(
                static_cast<unsigned char>(part.front()))) != names[index]) {
            failOclField("decal Color component is malformed",
                         "Color", value);
            return fallback;
        }
        uint32_t channel = 0;
        const auto parsed = std::from_chars(
            part.data() + 2, part.data() + part.size(), channel);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != part.data() + part.size() || channel > 255) {
            failOclField("decal Color component is outside 0..255",
                         "Color", value);
            return fallback;
        }
        result[index] = static_cast<uint8_t>(channel);
    }
    return result;
}

[[nodiscard]] std::optional<container::StringView> valueLast(
    const IniBlock& block, container::StringView key) noexcept {
    for (auto found = block.values.rbegin(); found != block.values.rend(); ++found) {
        if (equalInsensitive(found->first, key)) return found->second;
    }
    return std::nullopt;
}

void parseCoordinate(container::StringView value,
                     ObjectCreationFixedOffset& output) {
    const container::Vector<container::String> components = words(value);
    for (size_t index = 0; index < components.size(); ++index) {
        const container::String& token = components[index];
        const size_t colon = token.find(':');
        if (colon == container::String::npos) {
            failOclField("coordinate component is missing ':'",
                         "Coordinate", token);
            continue;
        }
        const container::String axis = canonical(
            container::StringView{token}.substr(0, colon));
        container::StringView scalarText{token};
        scalarText.remove_prefix(colon + 1u);
        // Shipped OCL files freely mix `X:1`, `X: 1` and combinations of
        // both on one Offset line. FieldParse consumes the next real after an
        // axis marker; preserve that token grammar instead of requiring the
        // colon and number to be one whitespace token.
        if (scalarText.empty()) {
            if (index + 1u >= components.size() ||
                components[index + 1u].find(':') !=
                    container::String::npos) {
                failOclField("coordinate axis has no scalar value",
                             "Coordinate", token);
                continue;
            }
            scalarText = components[++index];
        }
        const math::q32_32 scalar = parseFixed(scalarText);
        if (axis == "x") output.x = scalar;
        else if (axis == "y") output.y = scalar;
        else if (axis == "z") output.z = scalar;
        else failOclField("coordinate contains an unknown axis",
                          "Coordinate", token);
    }
}

[[nodiscard]] ObjectCreationDispositionMask parseDisposition(
    container::StringView value) {
    ObjectCreationDispositionMask result = 0;
    for (const container::String& token : words(value)) {
        const container::String name = canonical(token);
        if (name == "like_existing") result |= objectCreationDispositionBit(ObjectCreationDisposition::LikeExisting);
        else if (name == "on_ground_aligned") result |= objectCreationDispositionBit(ObjectCreationDisposition::OnGroundAligned);
        else if (name == "send_it_flying") result |= objectCreationDispositionBit(ObjectCreationDisposition::SendItFlying);
        else if (name == "send_it_up") result |= objectCreationDispositionBit(ObjectCreationDisposition::SendItUp);
        else if (name == "send_it_out") result |= objectCreationDispositionBit(ObjectCreationDisposition::SendItOut);
        else if (name == "random_force") result |= objectCreationDispositionBit(ObjectCreationDisposition::RandomForce);
        else if (name == "floating") result |= objectCreationDispositionBit(ObjectCreationDisposition::Floating);
        else if (name == "inherit_velocity") result |= objectCreationDispositionBit(ObjectCreationDisposition::InheritVelocity);
        else if (name == "whirling") result |= objectCreationDispositionBit(ObjectCreationDisposition::Whirling);
        else failOclField("Disposition contains an unknown token",
                          "Disposition", token);
    }
    return result;
}

void parseCommon(const IniBlock& block, ObjectCreationGenericFields& output) {
    for (const auto& [key, value] : block.values) {
        if (equalInsensitive(key, "ObjectNames") ||
            equalInsensitive(key, "ModelNames")) {
            output.names = words(value);
        } else if (equalInsensitive(key, "PutInContainer")) output.putInContainer = container::String{trimView(value)};
        else if (equalInsensitive(key, "ParticleSystem")) output.particleSystem = container::String{trimView(value)};
        else if (equalInsensitive(key, "Count")) output.count = static_cast<uint32_t>(std::max(0, parseSigned(value, 1)));
        else if (equalInsensitive(key, "IgnorePrimaryObstacle")) output.ignorePrimaryObstacle = parseBoolean(value, output.ignorePrimaryObstacle);
        else if (equalInsensitive(key, "OrientInForceDirection")) output.orientInForceDirection = parseBoolean(value, output.orientInForceDirection);
        else if (equalInsensitive(key, "ExtraBounciness")) output.extraBounciness = parseFixed(value);
        else if (equalInsensitive(key, "ExtraFriction")) output.extraFrictionPerSecond = parseFixed(value);
        else if (equalInsensitive(key, "Offset")) parseCoordinate(value, output.offset);
        else if (equalInsensitive(key, "Disposition")) output.disposition = parseDisposition(value);
        else if (equalInsensitive(key, "DispositionIntensity")) output.dispositionIntensity = parseFixed(value);
        else if (equalInsensitive(key, "SpinRate")) output.spinRate = parseAngle(value, -1.0f * 180.0f / std::numbers::pi_v<float>);
        else if (equalInsensitive(key, "YawRate")) output.yawRate = parseAngle(value, -1.0f * 180.0f / std::numbers::pi_v<float>);
        else if (equalInsensitive(key, "RollRate")) output.rollRate = parseAngle(value, -1.0f * 180.0f / std::numbers::pi_v<float>);
        else if (equalInsensitive(key, "PitchRate")) output.pitchRate = parseAngle(value, -1.0f * 180.0f / std::numbers::pi_v<float>);
        else if (equalInsensitive(key, "MinForceMagnitude")) output.minimumForceMagnitude = parseFixed(value);
        else if (equalInsensitive(key, "MaxForceMagnitude")) output.maximumForceMagnitude = parseFixed(value);
        else if (equalInsensitive(key, "MinForcePitch")) output.minimumForcePitchRadians = parseAngle(value);
        else if (equalInsensitive(key, "MaxForcePitch")) output.maximumForcePitchRadians = parseAngle(value);
        else if (equalInsensitive(key, "MinLifetime")) output.minimumLifetimeMilliseconds = parseUnsigned(value);
        else if (equalInsensitive(key, "MaxLifetime")) output.maximumLifetimeMilliseconds = parseUnsigned(value);
        else if (equalInsensitive(key, "SpreadFormation")) output.spreadFormation = parseBoolean(value, output.spreadFormation);
        else if (equalInsensitive(key, "MinDistanceAFormation")) output.minimumFormationDistanceA = parseFixed(value);
        else if (equalInsensitive(key, "MinDistanceBFormation")) output.minimumFormationDistanceB = parseFixed(value);
        else if (equalInsensitive(key, "MaxDistanceFormation")) output.maximumFormationDistance = parseFixed(value);
        else if (equalInsensitive(key, "FadeIn")) output.fadeIn = parseBoolean(value, output.fadeIn);
        else if (equalInsensitive(key, "FadeOut")) output.fadeOut = parseBoolean(value, output.fadeOut);
        else if (equalInsensitive(key, "FadeTime")) output.fadeMilliseconds = parseUnsigned(value);
        else if (equalInsensitive(key, "FadeSound")) output.fadeSound = container::String{trimView(value)};
        else if (equalInsensitive(key, "PreserveLayer")) output.preserveLayer = parseBoolean(value, output.preserveLayer);
        else if (equalInsensitive(key, "DiesOnBadLand")) output.diesOnBadLand = parseBoolean(value, output.diesOnBadLand);
    }
    if (output.maximumLifetimeMilliseconds < output.minimumLifetimeMilliseconds) {
        failOclField("MaxLifetime is less than MinLifetime",
                     "MinLifetime/MaxLifetime",
                     std::to_string(output.minimumLifetimeMilliseconds) +
                         "/" +
                         std::to_string(output.maximumLifetimeMilliseconds));
    }
    if (output.maximumForceMagnitude < output.minimumForceMagnitude) {
        failOclField("MaxForceMagnitude is less than MinForceMagnitude",
                     "MinForceMagnitude/MaxForceMagnitude",
                     std::to_string(
                         output.minimumForceMagnitude.to_float()) +
                         "/" +
                         std::to_string(
                             output.maximumForceMagnitude.to_float()));
    }
}

[[nodiscard]] ObjectCreationNugget compileNugget(
    const IniBlock& block, uint32_t authoredOrder) {
    if (equalInsensitive(block.type, "CreateObject")) {
        ObjectCreationCreateObjectNugget result;
        result.authoredOrder = authoredOrder;
        parseCommon(block, result.common);
        for (const auto& [key, value] : block.values) {
            if (equalInsensitive(key, "ContainInsideSourceObject")) result.containInsideSourceObject = parseBoolean(value);
            else if (equalInsensitive(key, "InheritsVeterancy")) result.inheritsVeterancy = parseBoolean(value);
            else if (equalInsensitive(key, "SkipIfSignificantlyAirborne")) result.skipIfSignificantlyAirborne = parseBoolean(value);
            else if (equalInsensitive(key, "InvulnerableTime")) result.common.invulnerableMilliseconds = parseUnsigned(value);
            else if (equalInsensitive(key, "MinHealth")) result.common.minimumHealth = parsePercent(value, 1.0f);
            else if (equalInsensitive(key, "MaxHealth")) result.common.maximumHealth = parsePercent(value, 1.0f);
            else if (equalInsensitive(key, "RequiresLivePlayer")) result.requiresLivePlayer = parseBoolean(value);
        }
        if (result.common.maximumHealth < result.common.minimumHealth) {
            failOclField("MaxHealth is less than MinHealth",
                         "MinHealth/MaxHealth",
                         std::to_string(
                             result.common.minimumHealth.to_float()) +
                             "/" +
                             std::to_string(
                                 result.common.maximumHealth.to_float()));
        }
        if (result.common.names.empty()) {
            failOclField(
                "CreateObject requires at least one object template name",
                "ObjectNames", "");
        }
        return result;
    }
    if (equalInsensitive(block.type, "CreateDebris")) {
        ObjectCreationCreateDebrisNugget result;
        result.authoredOrder = authoredOrder;
        parseCommon(block, result.common);
        for (const auto& [key, value] : block.values) {
            if (equalInsensitive(key, "Mass")) result.mass = parseFixed(value, 1.0f);
            else if (equalInsensitive(key, "AnimationSet")) {
                const container::Vector<container::String> tokens = words(value);
                if (tokens.size() >= 3u) {
                    result.animationSets.push_back({
                        .initial = tokens[0],
                        .flying = tokens[1],
                        .final = tokens[2],
                    });
                } else {
                    failOclField(
                        "AnimationSet requires initial, flying and final animations",
                        "AnimationSet", value);
                }
            }
            else if (equalInsensitive(key, "FXFinal")) result.finalFx = container::String{trimView(value)};
            else if (equalInsensitive(key, "BounceSound")) result.bounceSound = container::String{trimView(value)};
            else if (equalInsensitive(key, "Shadow")) {
                result.shadowTypeMask = parseDebrisShadowTypeMask(value);
            }
            else if (equalInsensitive(key, "MinLODRequired")) {
                const container::String lod = canonical(value);
                if (lod == "high") {
                    result.minimumLod = ObjectCreationMinimumLod::High;
                } else if (lod == "medium") {
                    result.minimumLod = ObjectCreationMinimumLod::Medium;
                } else if (lod == "low") {
                    result.minimumLod = ObjectCreationMinimumLod::Low;
                } else {
                    failOclField("MinLODRequired has an unknown LOD",
                                 "MinLODRequired", value);
                }
            }
            else if (equalInsensitive(key, "OkToChangeModelColor")) result.okToChangeModelColor = parseBoolean(value);
        }
        // RefCode parses this as a positive non-zero real and asserts again
        // before assigning it to PhysicsUpdate.
        if (result.mass.raw() <= 0) {
            failOclField("CreateDebris Mass must be positive and non-zero",
                         "Mass", std::to_string(result.mass.to_float()));
        }
        if (result.common.names.empty()) {
            failOclField(
                "CreateDebris requires at least one model name",
                "ModelNames", "");
        }
        return result;
    }
    if (equalInsensitive(block.type, "ApplyRandomForce")) {
        ObjectCreationApplyRandomForceNugget result;
        result.authoredOrder = authoredOrder;
        for (const auto& [key, value] : block.values) {
            if (equalInsensitive(key, "SpinRate")) result.spinRate = parseAngle(value);
            else if (equalInsensitive(key, "MinForceMagnitude")) result.minimumMagnitude = parseFixed(value);
            else if (equalInsensitive(key, "MaxForceMagnitude")) result.maximumMagnitude = parseFixed(value);
            else if (equalInsensitive(key, "MinForcePitch")) result.minimumPitchRadians = parseAngle(value);
            else if (equalInsensitive(key, "MaxForcePitch")) result.maximumPitchRadians = parseAngle(value);
        }
        if (result.maximumMagnitude < result.minimumMagnitude) {
            failOclField(
                "MaxForceMagnitude is less than MinForceMagnitude",
                "MinForceMagnitude/MaxForceMagnitude",
                std::to_string(result.minimumMagnitude.to_float()) + "/" +
                    std::to_string(result.maximumMagnitude.to_float()));
        }
        return result;
    }
    if (equalInsensitive(block.type, "DeliverPayload")) {
        ObjectCreationDeliverPayloadNugget result;
        result.authoredOrder = authoredOrder;
        for (const auto& [key, value] : block.values) {
            if (equalInsensitive(key, "Transport")) result.transport = container::String{trimView(value)};
            else if (equalInsensitive(key, "PutInContainer")) result.putInContainer = container::String{trimView(value)};
            else if (equalInsensitive(key, "StartAtPreferredHeight")) result.startAtPreferredHeight = parseBoolean(value, result.startAtPreferredHeight);
            else if (equalInsensitive(key, "StartAtMaxSpeed")) result.startAtMaximumSpeed = parseBoolean(value, result.startAtMaximumSpeed);
            else if (equalInsensitive(key, "FormationSize")) result.formationSize = parseUnsigned(value, 1);
            else if (equalInsensitive(key, "FormationSpacing")) result.formationSpacing = parseFixed(value, 25.0f);
            else if (equalInsensitive(key, "WeaponConvergenceFactor")) result.weaponConvergenceFactor = parseFixed(value);
            else if (equalInsensitive(key, "WeaponErrorRadius")) result.weaponErrorRadius = parseFixed(value);
            else if (equalInsensitive(key, "DelayDeliveryMax")) result.delayDeliveryMaximumMilliseconds = parseUnsigned(value);
            else if (equalInsensitive(key, "DeliveryDistance")) result.deliveryDistance = parseFixed(value);
            else if (equalInsensitive(key, "PreOpenDistance")) result.preOpenDistance = parseFixed(value);
            else if (equalInsensitive(key, "MaxAttempts")) result.maximumAttempts = parseUnsigned(value, 1);
            else if (equalInsensitive(key, "DropDelay")) result.dropDelayMilliseconds = parseUnsigned(value);
            else if (equalInsensitive(key, "DropOffset")) parseCoordinate(value, result.dropOffset);
            else if (equalInsensitive(key, "DropVariance")) parseCoordinate(value, result.dropVariance);
            else if (equalInsensitive(key, "InheritTransportVelocity")) result.inheritTransportVelocity = parseBoolean(value);
            else if (equalInsensitive(key, "ExitPitchRate")) result.exitPitchRate = parseFixed(value);
            else if (equalInsensitive(key, "ParachuteDirectly")) result.parachuteDirectly = parseBoolean(value);
            else if (equalInsensitive(key, "VisibleItemsDroppedPerInterval")) result.visibleItemsDroppedPerInterval = parseUnsigned(value);
            else if (equalInsensitive(key, "VisibleDropBoneBaseName")) result.visibleDropBoneBaseName = container::String{trimView(value)};
            else if (equalInsensitive(key, "VisibleSubObjectBaseName")) result.visibleSubObjectBaseName = container::String{trimView(value)};
            else if (equalInsensitive(key, "VisibleNumBones")) result.visibleNumBones = parseUnsigned(value);
            else if (equalInsensitive(key, "VisiblePayloadTemplateName")) result.visiblePayloadTemplateName = container::String{trimView(value)};
            else if (equalInsensitive(key, "VisiblePayloadWeaponTemplate")) result.visiblePayloadWeaponTemplate = container::String{trimView(value)};
            else if (equalInsensitive(key, "SelfDestructObject")) result.selfDestructObject = parseBoolean(value);
            else if (equalInsensitive(key, "FireWeapon")) result.fireWeapon = parseBoolean(value);
            else if (equalInsensitive(key, "DiveStartDistance")) result.diveStartDistance = parseFixed(value);
            else if (equalInsensitive(key, "DiveEndDistance")) result.diveEndDistance = parseFixed(value);
            else if (equalInsensitive(key, "StrafingWeaponSlot")) result.strafingWeaponSlot = container::String{trimView(value)};
            else if (equalInsensitive(key, "StrafeWeaponFX")) result.strafeWeaponFx = container::String{trimView(value)};
            else if (equalInsensitive(key, "StrafeLength")) result.strafeLength = parseFixed(value);
            else if (equalInsensitive(key, "DeliveryDecal")) result.deliveryDecal = container::String{trimView(value)};
            else if (equalInsensitive(key, "DeliveryDecalRadius")) result.deliveryDecalRadius = parseFixed(value);
            else if (equalInsensitive(key, "Payload")) {
                const container::Vector<container::String> tokens = words(value);
                if (tokens.empty() || tokens.size() > 2u ||
                    trimView(tokens.front()).empty()) {
                    failOclField(
                        "Payload requires an object name and optional count",
                        "Payload", value);
                } else {
                    result.payload.push_back({
                        .object = tokens.front(),
                        .count = tokens.size() > 1
                            ? parseUnsigned(tokens[1], 1) : 1,
                    });
                }
            }
        }
        for (const IniBlock& child : block.children) {
            if (!equalInsensitive(child.type, "DeliveryDecal")) continue;
            if (const auto texture = valueLast(child, "Texture");
                texture && !trimView(*texture).empty()) {
                result.deliveryDecal = container::String{trimView(*texture)};
            }
            if (const auto value = valueLast(child, "Style")) {
                result.deliveryDecalShadowTypeMask = parseDecalShadowTypeMask(
                    *value, result.deliveryDecalShadowTypeMask);
            }
            if (const auto value = valueLast(child, "OpacityMin")) {
                result.deliveryDecalMinimumOpacity = parsePercent(
                    *value, result.deliveryDecalMinimumOpacity.to_float());
            }
            if (const auto value = valueLast(child, "OpacityMax")) {
                result.deliveryDecalMaximumOpacity = parsePercent(
                    *value, result.deliveryDecalMaximumOpacity.to_float());
            }
            if (result.deliveryDecalMaximumOpacity <
                result.deliveryDecalMinimumOpacity) {
                failOclField("OpacityMax is less than OpacityMin",
                             "OpacityMin/OpacityMax",
                             std::to_string(
                                 result.deliveryDecalMinimumOpacity.to_float()) +
                                 "/" +
                                 std::to_string(
                                     result.deliveryDecalMaximumOpacity.to_float()));
            }
            if (const auto value = valueLast(child, "OpacityThrobTime")) {
                result.deliveryDecalOpacityThrobMilliseconds = parseUnsigned(
                    *value, result.deliveryDecalOpacityThrobMilliseconds);
            }
            if (const auto value = valueLast(child, "Color")) {
                result.deliveryDecalColor = parseDecalColor(
                    *value, result.deliveryDecalColor);
                result.deliveryDecalUsesPlayerColor = std::all_of(
                    result.deliveryDecalColor.begin(),
                    result.deliveryDecalColor.end(),
                    [](uint8_t channel) { return channel == 0; });
            }
            if (const auto value = valueLast(
                    child, "OnlyVisibleToOwningPlayer")) {
                result.deliveryDecalOnlyVisibleToOwningPlayer = parseBoolean(
                    *value, result.deliveryDecalOnlyVisibleToOwningPlayer);
            }
        }
        if (const auto authoredTransport = valueLast(block, "Transport");
            authoredTransport && trimView(*authoredTransport).empty()) {
            failOclField(
                "DeliverPayload contains an empty transport object reference",
                "Transport", "");
        }
        if (const auto slot = valueLast(block, "StrafingWeaponSlot");
            slot && !isWeaponSlot(*slot)) {
            failOclField(
                "StrafingWeaponSlot must be PRIMARY, SECONDARY or TERTIARY",
                "StrafingWeaponSlot", *slot);
        }
        return result;
    }
    if (equalInsensitive(block.type, "FireWeapon")) {
        ObjectCreationFireWeaponNugget result;
        result.authoredOrder = authoredOrder;
        if (const auto value = valueLast(block, "Weapon")) {
            result.weapon = container::String{trimView(*value)};
        }
        if (result.weapon.empty()) {
            failOclField("FireWeapon requires a weapon template",
                         "Weapon", "");
        }
        return result;
    }

    ObjectCreationAttackNugget result;
    result.authoredOrder = authoredOrder;
    for (const auto& [key, value] : block.values) {
        if (equalInsensitive(key, "NumberOfShots")) result.numberOfShots = parseSigned(value, 1);
        else if (equalInsensitive(key, "WeaponSlot")) result.weaponSlot = container::String{trimView(value)};
        else if (equalInsensitive(key, "DeliveryDecal")) result.deliveryDecal = container::String{trimView(value)};
        else if (equalInsensitive(key, "DeliveryDecalRadius")) result.deliveryDecalRadius = parseFixed(value);
    }
    for (const IniBlock& child : block.children) {
        if (!equalInsensitive(child.type, "DeliveryDecal")) continue;
        if (const std::optional<container::StringView> texture =
                valueLast(child, "Texture");
            texture && !trimView(*texture).empty()) {
            result.deliveryDecal = container::String{trimView(*texture)};
        }
        if (const auto value = valueLast(child, "Style")) {
            result.deliveryDecalShadowTypeMask = parseDecalShadowTypeMask(
                *value, result.deliveryDecalShadowTypeMask);
        }
        if (const auto value = valueLast(child, "OpacityMin")) {
            result.deliveryDecalMinimumOpacity = parsePercent(
                *value, result.deliveryDecalMinimumOpacity.to_float());
        }
        if (const auto value = valueLast(child, "OpacityMax")) {
            result.deliveryDecalMaximumOpacity = parsePercent(
                *value, result.deliveryDecalMaximumOpacity.to_float());
        }
        if (result.deliveryDecalMaximumOpacity <
            result.deliveryDecalMinimumOpacity) {
            failOclField("OpacityMax is less than OpacityMin",
                         "OpacityMin/OpacityMax",
                         std::to_string(
                             result.deliveryDecalMinimumOpacity.to_float()) +
                             "/" +
                             std::to_string(
                                 result.deliveryDecalMaximumOpacity.to_float()));
        }
        if (const auto value = valueLast(child, "OpacityThrobTime")) {
            result.deliveryDecalOpacityThrobMilliseconds = parseUnsigned(
                *value, result.deliveryDecalOpacityThrobMilliseconds);
        }
        if (const auto value = valueLast(child, "Color")) {
            result.deliveryDecalColor = parseDecalColor(
                *value, result.deliveryDecalColor);
            result.deliveryDecalUsesPlayerColor = std::all_of(
                result.deliveryDecalColor.begin(),
                result.deliveryDecalColor.end(),
                [](uint8_t channel) { return channel == 0; });
        }
        if (const auto value = valueLast(
                child, "OnlyVisibleToOwningPlayer")) {
            result.deliveryDecalOnlyVisibleToOwningPlayer = parseBoolean(
                *value, result.deliveryDecalOnlyVisibleToOwningPlayer);
        }
    }
    if (!isWeaponSlot(result.weaponSlot)) {
        failOclField("WeaponSlot must be PRIMARY, SECONDARY or TERTIARY",
                     "WeaponSlot", result.weaponSlot);
    }
    return result;
}

[[nodiscard]] bool supportedNuggetType(
    container::StringView type) {
    return equalInsensitive(type, "CreateObject") ||
        equalInsensitive(type, "CreateDebris") ||
        equalInsensitive(type, "ApplyRandomForce") ||
        equalInsensitive(type, "DeliverPayload") ||
        equalInsensitive(type, "FireWeapon") ||
        equalInsensitive(type, "Attack");
}

[[nodiscard]] bool compileDefinition(
    container::StringView source, const IniBlock& block,
    bool replacesExisting, ObjectCreationListDefinition& definition) {
    OclCompileState state{
        .source = source,
        .definition = definition.name,
    };
    OclCompileScope scope(state);
    uint32_t authoredOrder = 0;
    for (const IniBlock& child : block.children) {
        state.module = child.type;
        if (!supportedNuggetType(child.type)) {
            failOclDefinition(
                "ObjectCreationList contains an unsupported nugget type",
                container::String{child.type});
            continue;
        }
        definition.nuggets.push_back(
            compileNugget(child, authoredOrder++));
    }

    state.module = {};
    if (state.failed) {
        warnOcl(
            replacesExisting
                ? "malformed replacement definition removed the preceding definition under legacy clear semantics"
                : "malformed ObjectCreationList definition was isolated");
        return false;
    }
    // A repeated name is ordinary authored OCL overwrite behavior. RefCode
    // clears the canonical entry and parses the successor, so deterministic
    // last-definition-wins is not degraded content and needs no diagnostic.
    return true;
}

} // namespace

container::Vector<container::String> ObjectCreationListCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return ini::enumerateLegacyIniDirectories(loadRoots);
}

bool ObjectCreationListCatalog::loadFromVfsFiles(
    const container::Vector<container::String>& logicalFiles, container::String* error) {
    if (error) error->clear();
    clear();
    container::HashMap<container::String, ObjectCreationListDefinition> definitions;
    auto& vfs = io::VFS::instance();
    for (const container::String& path : logicalFiles) {
        if (!vfs.exists(path)) {
            OclCompileState state{.source = path};
            OclCompileScope scope(state);
            warnOcl("OCL source disappeared from VFS during load", "File",
                    path, "source skipped; other definitions retained");
            continue;
        }
        const container::Vector<container::String> layers{vfs.readAll(path)};
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            const container::String source = path + " [layer " +
                std::to_string(layerIndex) + "]";
            GeneralsIniParser parser;
            if (!parser.parse(layers[layerIndex], source)) {
                OclCompileState state{.source = path};
                OclCompileScope scope(state);
                warnOcl("could not parse OCL source", "IniLayer", {},
                        "source skipped; other definitions retained");
                continue;
            }
            for (const IniBlock& block : parser.blocks()) {
                if (!equalInsensitive(block.type, "ObjectCreationList")) continue;
                const auto diagnosticScope =
                    contentDiagnosticProvenanceScope(block.source);
                const container::String key = canonical(block.name);
                if (key.empty()) {
                    OclCompileState state{.source = path};
                    OclCompileScope scope(state);
                    warnOcl("ObjectCreationList block has an empty name", "Name",
                            block.name, "block skipped");
                    continue;
                }
                ObjectCreationListDefinition definition;
                definition.name = container::String{trimView(block.name)};
                const bool replacesExisting =
                    definitions.find(key) != definitions.end();
                if (!compileDefinition(
                        path, block, replacesExisting, definition)) {
                    // RefCode obtains the canonical map entry and clears it
                    // before parsing the successor. A malformed successor
                    // therefore cannot reveal the preceding definition.
                    definitions.erase(key);
                    continue;
                }
                // RefCode parseObjectCreationListDefinition calls clear() on
                // a duplicate. Preserve that real last-definition-wins
                // behavior across Default/base/mod layers.
                definitions.insert_or_assign(key, std::move(definition));
            }
        }
    }

    container::Vector<std::pair<container::String, ObjectCreationListDefinition>> ordered;
    ordered.reserve(definitions.size());
    for (auto& entry : definitions) ordered.push_back(std::move(entry));
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    m_definitions.reserve(ordered.size());
    m_ids.reserve(ordered.size());
    for (auto& [key, definition] : ordered) {
        definition.id = ObjectCreationListContentId{
            .value = static_cast<uint32_t>(m_definitions.size() + 1)};
        m_ids.emplace(std::move(key), definition.id);
        m_definitions.push_back(std::move(definition));
    }
    m_loaded = true;
    TD_LOG_INFO("[ObjectCreationListCatalog] Loaded {} OCL recipes from {} source files",
                m_definitions.size(), logicalFiles.size());
    return true;
}

bool ObjectCreationListCatalog::applyOverridesFromVfs(
    container::StringView rawPath, container::String* error) {
    if (error) error->clear();
    if (!m_loaded) {
        if (error) *error = "OCL override requires a loaded base catalog";
        return false;
    }

    const container::String path = canonical(rawPath);
    if (path.empty()) {
        if (error) *error = "OCL override path is empty";
        return false;
    }
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        OclCompileState state{.source = path};
        OclCompileScope scope(state);
        warnOcl("OCL override source is absent from VFS", "File", path,
                "existing catalog retained");
        return true;
    }

    GeneralsIniParser parser;
    const container::String content = vfs.readAll(path);
    if (!parser.parse(content)) {
        OclCompileState state{.source = path};
        OclCompileScope scope(state);
        warnOcl("could not parse OCL override source", "IniLayer", {},
                "existing catalog retained");
        return true;
    }

    // Work entirely in detached values so valid definitions remain
    // publishable while each malformed successor is isolated independently.
    container::HashMap<container::String, ObjectCreationListDefinition>
        definitions;
    definitions.reserve(m_definitions.size());
    for (const ObjectCreationListDefinition& current : m_definitions) {
        definitions.insert_or_assign(canonical(current.name), current);
    }

    for (const IniBlock& block : parser.blocks()) {
        if (!equalInsensitive(block.type, "ObjectCreationList")) continue;
        const container::String key = canonical(block.name);
        if (key.empty()) {
            OclCompileState state{.source = path};
            OclCompileScope scope(state);
            warnOcl("OCL override block has an empty name", "Name",
                    block.name, "block skipped");
            continue;
        }

        // ObjectCreationListStore::parseObjectCreationListDefinition obtains
        // m_ocls[key] and calls clear() even under CreateOverrides. Therefore
        // an authored successor replaces the complete nugget list.
        ObjectCreationListDefinition definition;
        definition.name = container::String{trimView(block.name)};
        const bool replacesExisting =
            definitions.find(key) != definitions.end();
        if (!compileDefinition(path, block, replacesExisting, definition)) {
            // Legacy parsing clears this canonical entry before compiling the
            // successor, even when the successor is malformed.
            definitions.erase(key);
            continue;
        }
        definitions.insert_or_assign(key, std::move(definition));
    }

    container::Vector<std::pair<container::String,
                                ObjectCreationListDefinition>> ordered;
    ordered.reserve(definitions.size());
    for (auto& entry : definitions) ordered.push_back(std::move(entry));
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    if (ordered.size() > std::numeric_limits<uint32_t>::max()) {
        if (error) *error = "OCL catalog exceeds its 32-bit content ID space";
        return false;
    }

    container::Vector<ObjectCreationListDefinition> sealedDefinitions;
    container::HashMap<container::String, ObjectCreationListContentId>
        sealedIds;
    sealedDefinitions.reserve(ordered.size());
    sealedIds.reserve(ordered.size());
    for (auto& [key, definition] : ordered) {
        definition.id = ObjectCreationListContentId{
            .value = static_cast<uint32_t>(sealedDefinitions.size() + 1u)};
        sealedIds.emplace(std::move(key), definition.id);
        sealedDefinitions.push_back(std::move(definition));
    }

    m_definitions = std::move(sealedDefinitions);
    m_ids = std::move(sealedIds);
    m_loaded = true;
    return true;
}

bool ObjectCreationListCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots, container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

void ObjectCreationListCatalog::clear() noexcept {
    m_definitions.clear();
    m_ids.clear();
    m_loaded = false;
}

const ObjectCreationListDefinition* ObjectCreationListCatalog::find(
    container::StringView name) const noexcept {
    return find(findId(name));
}

ObjectCreationListContentId ObjectCreationListCatalog::findId(
    container::StringView name) const noexcept {
    if (!m_loaded || name.empty() || equalInsensitive(name, "None")) return {};
    const auto found = m_ids.find(canonical(name));
    return found == m_ids.end() ? ObjectCreationListContentId{} : found->second;
}

const ObjectCreationListDefinition* ObjectCreationListCatalog::find(
    ObjectCreationListContentId id) const noexcept {
    if (!m_loaded || !id || id.value > m_definitions.size()) return nullptr;
    return &m_definitions[id.value - 1];
}

} // namespace game
