#include "core/container/hash_containers.h"
#include "ParticleSystemCatalog.h"

#include "LegacyIniDirectory.h"
#include "LegacyFxParsing.h"
#include "VFS.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <limits>
namespace engine::fx {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hashByte(uint64_t& value, uint8_t byte) noexcept {
    value ^= byte;
    value *= kFnvPrime;
}

void hashU64(uint64_t& value, uint64_t input) noexcept {
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        hashByte(value, static_cast<uint8_t>((input >> shift) & 0xffu));
    }
}

void hashString(uint64_t& value, container::StringView input) noexcept {
    hashU64(value, static_cast<uint64_t>(input.size()));
    for (const unsigned char character : input) hashByte(value, character);
}

[[nodiscard]] container::String canonicalPath(container::StringView path) {
    container::String result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        return detail::asciiLower(character);
    });
    while (result.starts_with("./")) result.erase(0, 2);
    return result;
}

[[nodiscard]] bool hasLegacySmudgeParticleName(
    container::StringView particleName) noexcept {
    constexpr container::StringView prefix = "SMUD";
    return particleName.size() >= prefix.size() &&
        detail::asciiEqual(particleName.substr(0, prefix.size()), prefix);
}

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
}

template <typename Enum>
bool parseEnum(container::StringView value,
               std::initializer_list<std::pair<container::StringView, Enum>> names,
               Enum& output) {
    for (const auto& [name, candidate] : names) {
        if (detail::asciiEqual(value, name)) {
            output = candidate;
            return true;
        }
    }
    return false;
}

bool assignRange(container::StringView value, ParticleRange& output) {
    const std::optional<detail::ParsedRange> parsed = detail::parseRange(value);
    if (!parsed) return false;
    output = {.minimum = parsed->minimum, .maximum = parsed->maximum};
    return true;
}

bool assignVector(container::StringView value, ParticleVector3& output) {
    const std::optional<detail::ParsedVector3> parsed = detail::parseVector3(value);
    if (!parsed) return false;
    output = {.x = parsed->x, .y = parsed->y, .z = parsed->z};
    return true;
}

bool assignBool(container::StringView value, bool& output) {
    const std::optional<bool> parsed = detail::parseBool(value);
    if (!parsed) return false;
    output = *parsed;
    return true;
}

bool parseAlphaKey(container::StringView value, ParticleAlphaKeyframe& output) {
    const container::Vector<container::StringView> words = detail::splitWords(value);
    if (words.size() != 3) return false;
    const std::optional<float> minimum = detail::parseFloat(words[0]);
    const std::optional<float> maximum = detail::parseFloat(words[1]);
    const std::optional<uint32_t> frame = detail::parseUnsigned(words[2]);
    if (!minimum || !maximum || !frame) return false;
    output = {
        .value = {.minimum = *minimum, .maximum = *maximum},
        .frame = *frame,
    };
    return true;
}

bool parseColorKey(container::StringView value, ParticleColorKeyframe& output) {
    const container::Vector<container::StringView> words = detail::splitWords(value);
    if (words.size() < 4) return false;
    const std::optional<detail::ParsedColor> color = detail::parseColor(value);
    const std::optional<uint32_t> frame = detail::parseUnsigned(words.back());
    if (!color || !frame) return false;
    output = {
        .color = {.red = color->red, .green = color->green, .blue = color->blue},
        .frame = *frame,
    };
    return true;
}

[[nodiscard]] std::optional<size_t> indexedKey(container::StringView key,
                                                container::StringView prefix) noexcept {
    if (key.size() != prefix.size() + 1 ||
        !detail::asciiEqual(key.substr(0, prefix.size()), prefix)) {
        return std::nullopt;
    }
    const char index = key.back();
    return index >= '1' && index <= '8'
        ? std::optional<size_t>{static_cast<size_t>(index - '1')}
        : std::nullopt;
}

} // namespace

container::Vector<container::String> ParticleSystemCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return game::ini::enumerateLegacyIniDirectories(loadRoots);
}

bool ParticleSystemCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots, container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool ParticleSystemCatalog::loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                             container::String* error) {
    if (error) error->clear();
    clear();
    if (logicalFiles.empty()) {
        setError(error, "ParticleSystem INI input set is empty");
        return false;
    }

    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty() || !parsedFiles.insert(path).second) continue;
        // `readAll` returns one String, so the vector always holds exactly one
        // element and the old `layers.empty()` guard was unreachable: a locked or
        // corrupt INI was quietly parsed as empty, producing an incomplete
        // catalog (missing effects at runtime) instead of failing the load.
        if (!io::VFS::instance().exists(path)) {
            setError(error, "ParticleSystem INI disappeared from VFS during load: " + path);
            clear();
            return false;
        }
        const container::Vector<container::String> layers{
            io::VFS::instance().readAll(path)};
        hashString(m_fingerprint, path);
        hashU64(m_fingerprint, static_cast<uint64_t>(layers.size()));
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            hashString(m_fingerprint, layers[layerIndex]);
            const container::String source = path + "#layer" + std::to_string(layerIndex);
            if (!appendParsedLayer(layers[layerIndex], source, error)) {
                clear();
                return false;
            }
        }
    }
    rebuildIndexAndResolveReferences();
    return !m_templates.empty();
}

bool ParticleSystemCatalog::loadFromText(container::StringView content,
                                         container::StringView sourceName,
                                         container::String* error) {
    if (error) error->clear();
    clear();
    hashString(m_fingerprint, sourceName);
    hashString(m_fingerprint, content);
    if (!appendParsedLayer(content, sourceName, error)) {
        clear();
        return false;
    }
    rebuildIndexAndResolveReferences();
    return !m_templates.empty();
}

bool ParticleSystemCatalog::applyOverridesFromVfs(
    container::StringView rawPath, container::String* error) {
    if (error) error->clear();
    const container::String path = canonicalPath(rawPath);
    if (path.empty()) {
        setError(error, "ParticleSystem override path is empty");
        return false;
    }

    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        setError(error,
                 "ParticleSystem override source is absent from VFS: " + path);
        return false;
    }

    // CREATE_OVERRIDES consumes the winning map.ini/solo.ini layer exactly
    // once.  Stage the sparse mutation so a malformed map-local definition
    // cannot partially corrupt the already frozen global catalog copy.
    const container::String content = vfs.readAll(path);
    ParticleSystemCatalog staged = *this;
    bool parsedDefinition = false;
    if (!staged.appendParsedLayer(
            content, path, error, &parsedDefinition)) {
        return false;
    }
    if (!parsedDefinition) return true;

    hashString(staged.m_fingerprint, "ParticleSystemOverride");
    hashString(staged.m_fingerprint, path);
    hashString(staged.m_fingerprint, content);
    staged.rebuildIndexAndResolveReferences();
    *this = std::move(staged);
    return true;
}

void ParticleSystemCatalog::clear() {
    m_templates.clear();
    m_indicesByName.clear();
    m_diagnostics.clear();
    m_fingerprint = kFnvOffsetBasis;
}

const ParticleSystemTemplate* ParticleSystemCatalog::find(container::StringView name) const noexcept {
    const auto found = m_indicesByName.find(detail::canonicalName(name));
    return found != m_indicesByName.end() ? &m_templates[found->second] : nullptr;
}

GpuParticleCompatibilityReason classifyGpuParticleCompatibility(
    const ParticleSystemTemplate& definition) noexcept {
    // Report structural/authority blockers before material blockers.  A
    // template can have several unsupported properties; this stable order
    // gives catalog telemetry one deterministic primary fallback reason.
    switch (definition.kind) {
    case ParticleKind::Billboard: break;
    case ParticleKind::None: return GpuParticleCompatibilityReason::KindNone;
    case ParticleKind::Drawable: return GpuParticleCompatibilityReason::KindDrawable;
    case ParticleKind::Streak: return GpuParticleCompatibilityReason::KindStreak;
    case ParticleKind::Volume: return GpuParticleCompatibilityReason::KindVolume;
    case ParticleKind::Smudge: return GpuParticleCompatibilityReason::KindSmudge;
    case ParticleKind::Count: return GpuParticleCompatibilityReason::KindInvalid;
    default: return GpuParticleCompatibilityReason::KindInvalid;
    }

    const bool authoredSlave = definition.slaveSystem ||
        (!definition.slaveSystemName.empty() &&
         !detail::asciiEqual(definition.slaveSystemName, "none"));
    if (authoredSlave) {
        return GpuParticleCompatibilityReason::SlaveSystemRequiresCpuAuthority;
    }
    const bool authoredAttachment = definition.perParticleAttachedSystem ||
        (!definition.perParticleAttachedSystemName.empty() &&
         !detail::asciiEqual(definition.perParticleAttachedSystemName, "none"));
    if (authoredAttachment) {
        return GpuParticleCompatibilityReason::PerParticleAttachedSystemRequiresCpuAuthority;
    }

    switch (definition.shader) {
    case ParticleShader::Additive:
    case ParticleShader::AlphaTest: break;
    case ParticleShader::None:
        return GpuParticleCompatibilityReason::ShaderNoneRequiresCpuOrdering;
    case ParticleShader::Alpha:
        return GpuParticleCompatibilityReason::ShaderAlphaRequiresCpuOrdering;
    case ParticleShader::Multiply:
        return GpuParticleCompatibilityReason::ShaderMultiplyRequiresCpuOrdering;
    case ParticleShader::Count:
        return GpuParticleCompatibilityReason::ShaderInvalid;
    default:
        return GpuParticleCompatibilityReason::ShaderInvalid;
    }

    // PingPong/Circular integration reads the live CPU emitter position and
    // wind angle every authored frame.  The first GPU contract has no emitter
    // graph or readback, so admitting it would silently change motion.
    if (definition.windMotion != ParticleWindMotion::Invalid &&
        definition.windMotion != ParticleWindMotion::Unused) {
        return GpuParticleCompatibilityReason::DynamicWindRequiresCpuEmitter;
    }
    return GpuParticleCompatibilityReason::Compatible;
}

const ParticleSystemTemplate* ParticleSystemCatalog::find(ParticleTemplateId id) const noexcept {
    return id.value > 0 && id.value <= m_templates.size() ? &m_templates[id.value - 1] : nullptr;
}

ParticleTemplateId ParticleSystemCatalog::findId(container::StringView name) const noexcept {
    const ParticleSystemTemplate* value = find(name);
    return value ? value->id : ParticleTemplateId{};
}

bool ParticleSystemCatalog::appendParsedLayer(container::StringView content,
                                              container::StringView sourceName,
                                              container::String* error,
                                              bool* parsedDefinition) {
    if (parsedDefinition) *parsedDefinition = false;
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse ParticleSystem INI: " + container::String(sourceName));
        return false;
    }

    for (const game::IniBlock& block : parser.blocks()) {
        if (!detail::asciiEqual(block.type, "ParticleSystem")) continue;
        if (parsedDefinition) *parsedDefinition = true;
        const container::String key = detail::canonicalName(detail::trim(block.name));
        if (key.empty()) {
            setError(error, "ParticleSystem with empty name in " + container::String(sourceName));
            return false;
        }
        const auto existing = m_indicesByName.find(key);
        // INI::parseParticleSystemDefinition reuses an existing template and
        // initFromINI writes only fields present in the current block.  Parse
        // into a copy so malformed overrides remain transactional while
        // omitted scalar, vector and indexed keyframe fields are inherited.
        ParticleSystemTemplate parsed = existing == m_indicesByName.end()
            ? ParticleSystemTemplate{}
            : m_templates[existing->second];
        if (!parseTemplate(block, sourceName, parsed, error)) return false;
        if (existing == m_indicesByName.end()) {
            m_indicesByName.emplace(key, m_templates.size());
            m_templates.push_back(std::move(parsed));
        } else {
            m_templates[existing->second] = std::move(parsed);
        }
    }
    return true;
}

bool ParticleSystemCatalog::parseTemplate(const game::IniBlock& block,
                                          container::StringView sourceName,
                                          ParticleSystemTemplate& output,
                                          container::String* error) {
    // Undo only our renderer-derived SMUD promotion before applying a sparse
    // authored layer.  Explicit Type = SMUDGE remains untouched because it
    // never sets smudgeKindInferred.
    if (output.smudgeKindInferred) {
        output.kind = ParticleKind::Billboard;
        output.smudgeKindInferred = false;
    }
    output.name = container::String(detail::trim(block.name));

    const auto invalid = [&](container::StringView key, container::StringView value) {
        setError(error, "invalid ParticleSystem field '" + container::String(key) + " = " +
            container::String(value) + "' for '" + output.name + "' in " + container::String(sourceName));
        return false;
    };

    for (const auto& [key, value] : block.values) {
        if (detail::asciiEqual(key, "Priority")) {
            if (!parseEnum(value, {
                    {"NONE", ParticlePriority::Invalid},
                    {"INVALID_PRIORITY", ParticlePriority::Invalid},
                    {"WEAPON_EXPLOSION", ParticlePriority::WeaponExplosion},
                    {"SCORCHMARK", ParticlePriority::ScorchMark},
                    {"DUST_TRAIL", ParticlePriority::DustTrail},
                    {"BUILDUP", ParticlePriority::BuildUp},
                    {"DEBRIS_TRAIL", ParticlePriority::DebrisTrail},
                    {"UNIT_DAMAGE_FX", ParticlePriority::UnitDamageFx},
                    {"DEATH_EXPLOSION", ParticlePriority::DeathExplosion},
                    {"SEMI_CONSTANT", ParticlePriority::SemiConstant},
                    {"CONSTANT", ParticlePriority::Constant},
                    {"WEAPON_TRAIL", ParticlePriority::WeaponTrail},
                    {"AREA_EFFECT", ParticlePriority::AreaEffect},
                    {"CRITICAL", ParticlePriority::Critical},
                    {"ALWAYS_RENDER", ParticlePriority::AlwaysRender},
                }, output.priority)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "IsOneShot")) {
            if (!assignBool(value, output.oneShot)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "Shader")) {
            if (!parseEnum(value, {
                    {"NONE", ParticleShader::None}, {"ADDITIVE", ParticleShader::Additive},
                    {"ALPHA", ParticleShader::Alpha}, {"ALPHA_TEST", ParticleShader::AlphaTest},
                    {"MULTIPLY", ParticleShader::Multiply},
                }, output.shader)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "Type")) {
            if (!parseEnum(value, {
                    {"NONE", ParticleKind::None}, {"PARTICLE", ParticleKind::Billboard},
                    {"DRAWABLE", ParticleKind::Drawable}, {"STREAK", ParticleKind::Streak},
                    {"VOLUME_PARTICLE", ParticleKind::Volume}, {"SMUDGE", ParticleKind::Smudge},
                }, output.kind)) return invalid(key, value);
            output.smudgeKindInferred = false;
        } else if (detail::asciiEqual(key, "ParticleName")) {
            output.particleName = container::String(detail::trim(value));
        } else if (detail::asciiEqual(key, "AngleX")) {
            if (!assignRange(value, output.angleX)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngleY")) {
            if (!assignRange(value, output.angleY)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngleZ")) {
            if (!assignRange(value, output.angleZ)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngularRateX")) {
            if (!assignRange(value, output.angularRateX)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngularRateY")) {
            if (!assignRange(value, output.angularRateY)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngularRateZ")) {
            if (!assignRange(value, output.angularRateZ)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "AngularDamping")) {
            if (!assignRange(value, output.angularDamping)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelocityDamping")) {
            if (!assignRange(value, output.velocityDamping)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "Gravity")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.gravity = *parsed;
        } else if (detail::asciiEqual(key, "SlaveSystem")) {
            output.slaveSystemName = container::String(detail::trim(value));
        } else if (detail::asciiEqual(key, "SlavePosOffset")) {
            if (!assignVector(value, output.slavePositionOffset)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "PerParticleAttachedSystem")) {
            output.perParticleAttachedSystemName = container::String(detail::trim(value));
        } else if (detail::asciiEqual(key, "Lifetime")) {
            if (!assignRange(value, output.lifetime)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "SystemLifetime")) {
            const std::optional<uint32_t> parsed = detail::parseUnsigned(value);
            if (!parsed) return invalid(key, value);
            output.systemLifetime = *parsed;
        } else if (detail::asciiEqual(key, "Size")) {
            if (!assignRange(value, output.startSize)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "StartSizeRate")) {
            if (!assignRange(value, output.startSizeRate)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "SizeRate")) {
            if (!assignRange(value, output.sizeRate)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "SizeRateDamping")) {
            if (!assignRange(value, output.sizeRateDamping)) return invalid(key, value);
        } else if (const std::optional<size_t> index = indexedKey(key, "Alpha")) {
            if (!parseAlphaKey(value, output.alphaKeys[*index])) return invalid(key, value);
        } else if (const std::optional<size_t> index = indexedKey(key, "Color")) {
            if (!parseColorKey(value, output.colorKeys[*index])) return invalid(key, value);
        } else if (detail::asciiEqual(key, "ColorScale")) {
            if (!assignRange(value, output.colorScale)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "BurstDelay")) {
            if (!assignRange(value, output.burstDelay)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "BurstCount")) {
            if (!assignRange(value, output.burstCount)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "InitialDelay")) {
            if (!assignRange(value, output.initialDelay)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "DriftVelocity")) {
            if (!assignVector(value, output.driftVelocity)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelocityType")) {
            if (!parseEnum(value, {
                    {"NONE", ParticleVelocityKind::None}, {"ORTHO", ParticleVelocityKind::Ortho},
                    {"SPHERICAL", ParticleVelocityKind::Spherical},
                    {"HEMISPHERICAL", ParticleVelocityKind::Hemispherical},
                    {"CYLINDRICAL", ParticleVelocityKind::Cylindrical},
                    {"OUTWARD", ParticleVelocityKind::Outward},
                }, output.velocityKind)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelOrthoX")) {
            if (!assignRange(value, output.velocityOrthoX)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelOrthoY")) {
            if (!assignRange(value, output.velocityOrthoY)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelOrthoZ")) {
            if (!assignRange(value, output.velocityOrthoZ)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelSpherical")) {
            if (!assignRange(value, output.velocitySpherical)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelHemispherical")) {
            if (!assignRange(value, output.velocityHemispherical)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelCylindricalRadial")) {
            if (!assignRange(value, output.velocityCylindricalRadial)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelCylindricalNormal")) {
            if (!assignRange(value, output.velocityCylindricalNormal)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelOutward")) {
            if (!assignRange(value, output.velocityOutward)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VelOutwardOther")) {
            if (!assignRange(value, output.velocityOutwardOther)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VolumeType")) {
            if (!parseEnum(value, {
                    {"NONE", ParticleVolumeKind::None}, {"POINT", ParticleVolumeKind::Point},
                    {"LINE", ParticleVolumeKind::Line}, {"BOX", ParticleVolumeKind::Box},
                    {"SPHERE", ParticleVolumeKind::Sphere}, {"CYLINDER", ParticleVolumeKind::Cylinder},
                }, output.volumeKind)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VolLineStart")) {
            if (!assignVector(value, output.volumeLineStart)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VolLineEnd")) {
            if (!assignVector(value, output.volumeLineEnd)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VolBoxHalfSize")) {
            if (!assignVector(value, output.volumeBoxHalfSize)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "VolSphereRadius")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.volumeSphereRadius = *parsed;
        } else if (detail::asciiEqual(key, "VolCylinderRadius")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.volumeCylinderRadius = *parsed;
        } else if (detail::asciiEqual(key, "VolCylinderLength")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.volumeCylinderLength = *parsed;
        } else if (detail::asciiEqual(key, "IsHollow")) {
            if (!assignBool(value, output.hollow)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "IsGroundAligned")) {
            if (!assignBool(value, output.groundAligned)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "IsEmitAboveGroundOnly")) {
            if (!assignBool(value, output.emitAboveGroundOnly)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "IsParticleUpTowardsEmitter")) {
            if (!assignBool(value, output.particleUpTowardsEmitter)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "WindMotion")) {
            if (!parseEnum(value, {
                    {"NONE", ParticleWindMotion::Invalid},
                    {"UNUSED", ParticleWindMotion::Unused},
                    {"PINGPONG", ParticleWindMotion::PingPong},
                    {"PING_PONG", ParticleWindMotion::PingPong},
                    {"CIRCULAR", ParticleWindMotion::Circular},
                }, output.windMotion)) return invalid(key, value);
        } else if (detail::asciiEqual(key, "WindAngleChangeMin")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windAngleChangeMinimum = *parsed;
        } else if (detail::asciiEqual(key, "WindAngleChangeMax")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windAngleChangeMaximum = *parsed;
        } else if (detail::asciiEqual(key, "WindPingPongStartAngleMin")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windPingPongStartAngleMinimum = *parsed;
        } else if (detail::asciiEqual(key, "WindPingPongStartAngleMax")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windPingPongStartAngleMaximum = *parsed;
        } else if (detail::asciiEqual(key, "WindPingPongEndAngleMin")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windPingPongEndAngleMinimum = *parsed;
        } else if (detail::asciiEqual(key, "WindPingPongEndAngleMax")) {
            const std::optional<float> parsed = detail::parseFloat(value);
            if (!parsed) return invalid(key, value);
            output.windPingPongEndAngleMaximum = *parsed;
        } else {
            m_diagnostics.push_back("unknown ParticleSystem field '" + key + "' for '" +
                                    output.name + "' in " + container::String(sourceName));
        }
    }
    // W3DParticleSys does not use the authored Type token for heat smudges.
    // It routes PARTICLE systems whose texture begins with "SMUD" through
    // SmudgeManager, including the stock SMUDGE.tga definitions.
    if (output.kind == ParticleKind::Billboard &&
        hasLegacySmudgeParticleName(output.particleName)) {
        output.kind = ParticleKind::Smudge;
        output.smudgeKindInferred = true;
    }
    return true;
}

void ParticleSystemCatalog::rebuildIndexAndResolveReferences() {
    m_indicesByName.clear();
    m_indicesByName.reserve(m_templates.size());
    for (size_t index = 0; index < m_templates.size(); ++index) {
        ParticleSystemTemplate& value = m_templates[index];
        value.id = ParticleTemplateId{static_cast<uint32_t>(index + 1)};
        m_indicesByName.insert_or_assign(detail::canonicalName(value.name), index);
    }

    for (ParticleSystemTemplate& value : m_templates) {
        value.slaveSystem = findId(value.slaveSystemName);
        value.perParticleAttachedSystem = findId(value.perParticleAttachedSystemName);
        if (!value.slaveSystemName.empty() && !detail::asciiEqual(value.slaveSystemName, "none") &&
            !value.slaveSystem) {
            m_diagnostics.push_back("ParticleSystem '" + value.name + "' references missing SlaveSystem '" +
                                    value.slaveSystemName + "'");
        }
        if (!value.perParticleAttachedSystemName.empty() &&
            !detail::asciiEqual(value.perParticleAttachedSystemName, "none") &&
            !value.perParticleAttachedSystem) {
            m_diagnostics.push_back("ParticleSystem '" + value.name +
                                    "' references missing PerParticleAttachedSystem '" +
                                    value.perParticleAttachedSystemName + "'");
        }
        value.gpuCompatibilityReason = classifyGpuParticleCompatibility(value);
    }
}

} // namespace engine::fx
