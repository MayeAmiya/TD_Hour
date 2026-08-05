#include "core/container/hash_containers.h"
#include "FxListCatalog.h"

#include "LegacyFxParsing.h"
#include "VFS.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
namespace engine::fx {
namespace {

constexpr float kDegreesToRadians = 0.01745329251994329577f;

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

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
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

bool assignColor(container::StringView value, ParticleColor& output) {
    const std::optional<detail::ParsedColor> parsed = detail::parseColor(value);
    if (!parsed) return false;
    output = {.red = parsed->red, .green = parsed->green, .blue = parsed->blue};
    return true;
}

bool assignBool(container::StringView value, bool& output) {
    const std::optional<bool> parsed = detail::parseBool(value);
    if (!parsed) return false;
    output = *parsed;
    return true;
}

[[nodiscard]] std::optional<uint32_t> parseDurationMilliseconds(container::StringView value) {
    value = detail::trim(value);
    float scale = 1.0f;
    if (value.size() >= 2 && detail::asciiEqual(value.substr(value.size() - 2), "ms")) {
        value.remove_suffix(2);
    } else if (value.size() >= 1 && detail::asciiEqual(value.substr(value.size() - 1), "s")) {
        value.remove_suffix(1);
        scale = 1000.0f;
    }
    const std::optional<float> parsed = detail::parseFloat(value);
    if (!parsed || *parsed < 0.0f) return std::nullopt;
    const double milliseconds = static_cast<double>(*parsed) * scale;
    if (milliseconds > static_cast<double>(UINT32_MAX)) return std::nullopt;
    return static_cast<uint32_t>(std::ceil(milliseconds));
}

[[nodiscard]] bool supportedNugget(container::StringView type) noexcept {
    return detail::asciiEqual(type, "Sound") ||
        detail::asciiEqual(type, "RayEffect") ||
        detail::asciiEqual(type, "Tracer") ||
        detail::asciiEqual(type, "LightPulse") ||
        detail::asciiEqual(type, "ViewShake") ||
        detail::asciiEqual(type, "TerrainScorch") ||
        detail::asciiEqual(type, "ParticleSystem") ||
        detail::asciiEqual(type, "FXListAtBonePos");
}

} // namespace

container::Vector<container::String> FxListCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return ParticleSystemCatalog::enumerateVfsLoadFiles(loadRoots);
}

bool FxListCatalog::loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                               container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool FxListCatalog::loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                     container::String* error) {
    if (error) error->clear();
    clear();
    if (logicalFiles.empty()) {
        setError(error, "FXList INI input set is empty");
        return false;
    }

    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty() || !parsedFiles.insert(path).second) continue;
        // Same unreachable-guard shape as ParticleSystemCatalog: `readAll` yields
        // one String, so `layers.empty()` never fired and an unreadable FXList
        // INI silently produced an incomplete catalog.
        if (!io::VFS::instance().exists(path)) {
            setError(error, "FXList INI disappeared from VFS during load: " + path);
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
    rebuildIndexAndResolveFxReferences();
    m_loaded = true;
    return true;
}

bool FxListCatalog::loadFromText(container::StringView content,
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
    rebuildIndexAndResolveFxReferences();
    m_loaded = true;
    return true;
}

bool FxListCatalog::applyOverridesFromVfs(container::StringView rawPath,
                                          container::String* error) {
    if (error) error->clear();
    if (!m_loaded) {
        setError(error, "FXList override requires a loaded base catalog");
        return false;
    }

    const container::String path = canonicalPath(rawPath);
    if (path.empty()) {
        setError(error, "FXList override path is empty");
        return false;
    }

    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        setError(error, "FXList override source is absent from VFS: " + path);
        return false;
    }

    // VFS::readAll() returns the winning mount only. Map.ini/CreateOverrides
    // must not replay every shadowed archive layer.
    const container::String content = vfs.readAll(path);
    FxListCatalog staged = *this;
    bool parsedDefinition = false;
    if (!staged.appendParsedLayer(content, path, error, &parsedDefinition)) return false;
    if (!parsedDefinition) return true;

    hashString(staged.m_fingerprint, "FXListOverride");
    hashString(staged.m_fingerprint, path);
    hashString(staged.m_fingerprint, content);
    staged.rebuildIndexAndResolveFxReferences();

    static_assert(std::is_nothrow_move_assignable_v<FxListCatalog>);
    *this = std::move(staged);
    return true;
}

void FxListCatalog::resolveReferences(const ParticleSystemCatalog& particles) {
    for (FxListDefinition& definition : m_definitions) {
        for (FxNugget& nugget : definition.nuggets) {
            if (auto* particle = std::get_if<FxParticleSystemNugget>(&nugget)) {
                particle->particleSystem = particles.findId(particle->particleSystemName);
                if (!particle->particleSystemName.empty() &&
                    !detail::asciiEqual(particle->particleSystemName, "none") &&
                    !particle->particleSystem) {
                    m_diagnostics.push_back("FXList '" + definition.name +
                        "' references missing ParticleSystem '" + particle->particleSystemName + "'");
                }
            }
        }
    }
}

void FxListCatalog::clear() {
    m_definitions.clear();
    m_indicesByName.clear();
    m_diagnostics.clear();
    m_fingerprint = kFnvOffsetBasis;
    m_loaded = false;
}

const FxListDefinition* FxListCatalog::find(container::StringView name) const noexcept {
    const auto found = m_indicesByName.find(detail::canonicalName(name));
    return found != m_indicesByName.end() ? &m_definitions[found->second] : nullptr;
}

const FxListDefinition* FxListCatalog::find(FxListId id) const noexcept {
    return id.value > 0 && id.value <= m_definitions.size() ? &m_definitions[id.value - 1] : nullptr;
}

FxListId FxListCatalog::findId(container::StringView name) const noexcept {
    if (name.empty() || detail::asciiEqual(name, "none")) return {};
    const FxListDefinition* value = find(name);
    return value ? value->id : FxListId{};
}

bool FxListCatalog::appendParsedLayer(container::StringView content,
                                      container::StringView sourceName,
                                      container::String* error,
                                      bool* parsedDefinition) {
    if (parsedDefinition) *parsedDefinition = false;
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse FXList INI: " + container::String(sourceName));
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (!detail::asciiEqual(block.type, "FXList")) continue;
        if (parsedDefinition) *parsedDefinition = true;
        FxListDefinition parsed;
        if (!parseDefinition(block, sourceName, parsed, error)) return false;
        const container::String key = detail::canonicalName(parsed.name);
        if (key.empty()) {
            setError(error, "FXList with empty name in " + container::String(sourceName));
            return false;
        }
        const auto existing = m_indicesByName.find(key);
        if (existing == m_indicesByName.end()) {
            m_indicesByName.emplace(key, m_definitions.size());
            m_definitions.push_back(std::move(parsed));
        } else {
            m_definitions[existing->second] = std::move(parsed);
        }
    }
    return true;
}

bool FxListCatalog::parseDefinition(const game::IniBlock& block,
                                    container::StringView sourceName,
                                    FxListDefinition& output,
                                    container::String* error) {
    output = {};
    output.name = container::String(detail::trim(block.name));
    output.nuggets.reserve(block.children.size());
    for (const game::IniBlock& child : block.children) {
        if (!supportedNugget(child.type)) {
            m_diagnostics.push_back("unknown FXList nugget '" + child.type + "' for '" +
                                    output.name + "' in " + container::String(sourceName));
            continue;
        }
        FxNugget parsed;
        if (!parseNugget(child, sourceName, parsed, error)) return false;
        output.nuggets.push_back(std::move(parsed));
    }
    return true;
}

bool FxListCatalog::parseNugget(const game::IniBlock& block,
                                container::StringView sourceName,
                                FxNugget& output,
                                container::String* error) {
    const auto invalid = [&](container::StringView key, container::StringView value) {
        setError(error, "invalid FXList nugget field '" + container::String(key) + " = " +
            container::String(value) + "' in " + container::String(sourceName));
        return false;
    };

    if (detail::asciiEqual(block.type, "Sound")) {
        FxSoundNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "Name")) value.name = container::String(detail::trim(field));
            else m_diagnostics.push_back("unknown Sound nugget field '" + key + "'");
        }
        output = std::move(value);
        return true;
    }
    if (detail::asciiEqual(block.type, "RayEffect")) {
        FxRayEffectNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "Name")) value.objectTemplate = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "PrimaryOffset")) {
                if (!assignVector(field, value.primaryOffset)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "SecondaryOffset")) {
                if (!assignVector(field, value.secondaryOffset)) return invalid(key, field);
            } else m_diagnostics.push_back("unknown RayEffect nugget field '" + key + "'");
        }
        output = std::move(value);
        return true;
    }
    if (detail::asciiEqual(block.type, "Tracer")) {
        FxTracerNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "TracerName")) value.tracerName = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "BoneName")) value.boneName = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "Color")) {
                if (!assignColor(field, value.color)) return invalid(key, field);
            } else {
                const std::optional<float> parsed = detail::parseFloat(field);
                if (detail::asciiEqual(key, "Speed")) {
                    if (!parsed) return invalid(key, field);
                    value.speed = *parsed;
                } else if (detail::asciiEqual(key, "DecayAt")) {
                    if (!parsed) return invalid(key, field);
                    value.decayAt = *parsed;
                } else if (detail::asciiEqual(key, "Length")) {
                    if (!parsed) return invalid(key, field);
                    value.length = *parsed;
                } else if (detail::asciiEqual(key, "Width")) {
                    if (!parsed) return invalid(key, field);
                    value.width = *parsed;
                } else if (detail::asciiEqual(key, "Probability")) {
                    if (!parsed) return invalid(key, field);
                    value.probability = *parsed;
                } else m_diagnostics.push_back("unknown Tracer nugget field '" + key + "'");
            }
        }
        output = std::move(value);
        return true;
    }
    if (detail::asciiEqual(block.type, "LightPulse")) {
        FxLightPulseNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "Color")) {
                if (!assignColor(field, value.color)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "RadiusAsPercentOfObjectSize")) {
                const std::optional<float> parsed = detail::parsePercent(field);
                if (!parsed) return invalid(key, field);
                value.radiusAsPercentOfObjectSize = *parsed;
            } else if (detail::asciiEqual(key, "IncreaseTime")) {
                const std::optional<uint32_t> parsed = parseDurationMilliseconds(field);
                if (!parsed) return invalid(key, field);
                value.increaseTimeMilliseconds = *parsed;
            } else if (detail::asciiEqual(key, "DecreaseTime")) {
                const std::optional<uint32_t> parsed = parseDurationMilliseconds(field);
                if (!parsed) return invalid(key, field);
                value.decreaseTimeMilliseconds = *parsed;
            } else if (detail::asciiEqual(key, "Radius")) {
                const std::optional<float> parsed = detail::parseFloat(field);
                if (!parsed) return invalid(key, field);
                value.radius = *parsed;
            } else m_diagnostics.push_back("unknown LightPulse nugget field '" + key + "'");
        }
        output = std::move(value);
        return true;
    }
    if (detail::asciiEqual(block.type, "ViewShake")) {
        FxViewShakeNugget value;
        for (const auto& [key, field] : block.values) {
            if (!detail::asciiEqual(key, "Type")) {
                m_diagnostics.push_back("unknown ViewShake nugget field '" + key + "'");
                continue;
            }
            if (detail::asciiEqual(field, "SUBTLE")) value.type = FxViewShake::Subtle;
            else if (detail::asciiEqual(field, "NORMAL")) value.type = FxViewShake::Normal;
            else if (detail::asciiEqual(field, "STRONG")) value.type = FxViewShake::Strong;
            else if (detail::asciiEqual(field, "SEVERE")) value.type = FxViewShake::Severe;
            else if (detail::asciiEqual(field, "CINE_EXTREME")) value.type = FxViewShake::CineExtreme;
            else if (detail::asciiEqual(field, "CINE_INSANE")) value.type = FxViewShake::CineInsane;
            else return invalid(key, field);
        }
        output = value;
        return true;
    }
    if (detail::asciiEqual(block.type, "TerrainScorch")) {
        FxTerrainScorchNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "Radius")) {
                const std::optional<float> parsed = detail::parseFloat(field);
                if (!parsed) return invalid(key, field);
                value.radius = *parsed;
            } else if (detail::asciiEqual(key, "Type")) {
                if (detail::asciiEqual(field, "RANDOM")) value.type = FxTerrainScorch::Random;
                else if (detail::asciiEqual(field, "SCORCH_1")) value.type = FxTerrainScorch::Scorch1;
                else if (detail::asciiEqual(field, "SCORCH_2")) value.type = FxTerrainScorch::Scorch2;
                else if (detail::asciiEqual(field, "SCORCH_3")) value.type = FxTerrainScorch::Scorch3;
                else if (detail::asciiEqual(field, "SCORCH_4")) value.type = FxTerrainScorch::Scorch4;
                else if (detail::asciiEqual(field, "SHADOW_SCORCH")) value.type = FxTerrainScorch::ShadowScorch;
                else return invalid(key, field);
            } else m_diagnostics.push_back("unknown TerrainScorch nugget field '" + key + "'");
        }
        output = value;
        return true;
    }
    if (detail::asciiEqual(block.type, "ParticleSystem")) {
        FxParticleSystemNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "Name")) value.particleSystemName = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "Offset")) {
                if (!assignVector(field, value.offset)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "Radius")) {
                if (!assignRange(field, value.radius)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "Height")) {
                if (!assignRange(field, value.height)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "InitialDelay")) {
                if (!assignRange(field, value.initialDelay)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "Count")) {
                const std::optional<int32_t> parsed = detail::parseInt(field);
                if (!parsed) return invalid(key, field);
                value.count = *parsed;
            } else if (detail::asciiEqual(key, "RotateX") ||
                       detail::asciiEqual(key, "RotateY") ||
                       detail::asciiEqual(key, "RotateZ")) {
                const std::optional<float> parsed = detail::parseFloat(field);
                if (!parsed) return invalid(key, field);
                const float radians = *parsed * kDegreesToRadians;
                if (detail::asciiEqual(key, "RotateX")) value.rotateX = radians;
                else if (detail::asciiEqual(key, "RotateY")) value.rotateY = radians;
                else value.rotateZ = radians;
            } else if (detail::asciiEqual(key, "OrientToObject")) {
                if (!assignBool(field, value.orientToObject)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "Ricochet")) {
                if (!assignBool(field, value.ricochet)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "AttachToObject")) {
                if (!assignBool(field, value.attachToObject)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "CreateAtGroundHeight")) {
                if (!assignBool(field, value.createAtGroundHeight)) return invalid(key, field);
            } else if (detail::asciiEqual(key, "UseCallersRadius")) {
                if (!assignBool(field, value.useCallersRadius)) return invalid(key, field);
            } else m_diagnostics.push_back("unknown ParticleSystem nugget field '" + key + "'");
        }
        output = std::move(value);
        return true;
    }
    if (detail::asciiEqual(block.type, "FXListAtBonePos")) {
        FxListAtBoneNugget value;
        for (const auto& [key, field] : block.values) {
            if (detail::asciiEqual(key, "FX")) value.fxName = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "BoneName")) value.boneName = container::String(detail::trim(field));
            else if (detail::asciiEqual(key, "OrientToBone")) {
                if (!assignBool(field, value.orientToBone)) return invalid(key, field);
            } else m_diagnostics.push_back("unknown FXListAtBonePos nugget field '" + key + "'");
        }
        output = std::move(value);
        return true;
    }

    setError(error, "unsupported FXList nugget '" + block.type + "' in " + container::String(sourceName));
    return false;
}

void FxListCatalog::rebuildIndexAndResolveFxReferences() {
    m_diagnostics.erase(
        std::remove_if(m_diagnostics.begin(), m_diagnostics.end(),
            [](const container::String& diagnostic) {
                return diagnostic.starts_with("FXList '") &&
                    diagnostic.find(" references missing nested FXList '") !=
                        container::String::npos;
            }),
        m_diagnostics.end());

    m_indicesByName.clear();
    m_indicesByName.reserve(m_definitions.size());
    for (size_t index = 0; index < m_definitions.size(); ++index) {
        FxListDefinition& value = m_definitions[index];
        value.id = FxListId{static_cast<uint32_t>(index + 1)};
        m_indicesByName.insert_or_assign(detail::canonicalName(value.name), index);
    }

    for (FxListDefinition& definition : m_definitions) {
        for (FxNugget& nugget : definition.nuggets) {
            if (auto* nested = std::get_if<FxListAtBoneNugget>(&nugget)) {
                nested->fx = findId(nested->fxName);
                if (!nested->fxName.empty() && !detail::asciiEqual(nested->fxName, "none") &&
                    !nested->fx) {
                    m_diagnostics.push_back("FXList '" + definition.name +
                        "' references missing nested FXList '" + nested->fxName + "'");
                }
            }
        }
    }
}

} // namespace engine::fx
