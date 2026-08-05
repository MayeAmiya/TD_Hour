#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "AudioEventCatalog.h"

#include "VFS.h"
#include "debug/debug.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "core/data/ini/LegacyIniDirectory.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace engine::audio {
namespace {

constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnv1aPrime = 1099511628211ull;

constexpr container::StringView kAudioIniRoots[]{
    "data/ini/AudioSettings",
    "data/ini/default/Music",
    "data/ini/Music",
    "data/ini/default/SoundEffects",
    "data/ini/SoundEffects",
    "data/ini/default/Speech",
    "data/ini/Speech",
    "data/ini/default/Voice",
    "data/ini/Voice",
    "data/ini/MiscAudio",
};

container::String trim(container::StringView input) {
    const size_t first = input.find_first_not_of(" \t\r\n");
    if (first == container::StringView::npos) return {};
    const size_t last = input.find_last_not_of(" \t\r\n");
    container::String result(input.substr(first, last - first + 1));
    if (result.size() >= 2 &&
        ((result.front() == '\"' && result.back() == '\"') ||
         (result.front() == '\'' && result.back() == '\''))) {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

container::String lowercase(container::StringView input) {
    container::String result(input);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

container::String canonicalName(container::StringView input) {
    return lowercase(trim(input));
}

container::String stripComment(container::StringView input) {
    const size_t semicolon = input.find(';');
    const size_t slash = input.find("//");
    const size_t comment = std::min(semicolon, slash);
    return trim(input.substr(0, comment));
}

std::pair<container::String, container::String> splitKeyValue(container::StringView input) {
    const size_t equals = input.find('=');
    if (equals != container::StringView::npos) {
        return {lowercase(trim(input.substr(0, equals))), trim(input.substr(equals + 1))};
    }
    const size_t space = input.find_first_of(" \t");
    if (space == container::StringView::npos) return {lowercase(trim(input)), {}};
    return {lowercase(trim(input.substr(0, space))), trim(input.substr(space + 1))};
}

container::Vector<container::String> words(container::StringView input) {
    container::Vector<container::String> result;
    std::istringstream stream{container::String(input)};
    container::String token;
    while (stream >> token) {
        if (token.size() >= 2 &&
            ((token.front() == '\"' && token.back() == '\"') ||
             (token.front() == '\'' && token.back() == '\''))) {
            token = token.substr(1, token.size() - 2);
        }
        if (!token.empty()) result.push_back(std::move(token));
    }
    return result;
}

bool parseFloat(container::StringView input, float& output) noexcept {
    const container::FiniteFloatParseResult parsed =
        container::parseFiniteFloatCompatible(input);
    if (!parsed) {
        TD_LOG_WARN("[AudioEventCatalog] invalid finite Real '{}'", input);
        return false;
    }
    if (parsed.nonCanonical()) {
        TD_LOG_WARN(
            "[AudioEventCatalog] accepted numeric prefix of noncanonical Real '{}'",
            input);
    }
    output = parsed.value;
    return true;
}

bool parseInteger(container::StringView input, int32_t& output) noexcept {
    const container::String value = trim(input);
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto [end, error] = std::from_chars(first, last, output);
    return error == std::errc{} && end != first;
}

bool parseUnsigned(container::StringView input, uint32_t& output) noexcept {
    int32_t signedValue = 0;
    if (!parseInteger(input, signedValue) || signedValue < 0) return false;
    output = static_cast<uint32_t>(signedValue);
    return true;
}

bool parsePercent(container::StringView input, float& output) noexcept {
    container::String value = trim(input);
    if (!value.empty() && value.back() == '%') value.pop_back();
    float number = 0.0f;
    if (!parseFloat(value, number)) return false;
    output = number / 100.0f;
    return true;
}

bool parseYes(container::StringView input, bool& output) noexcept {
    const container::String value = canonicalName(input);
    if (value == "yes" || value == "true" || value == "1") {
        output = true;
        return true;
    }
    if (value == "no" || value == "false" || value == "0") {
        output = false;
        return true;
    }
    return false;
}

float finiteClamp(float value, float low, float high, float fallback) noexcept {
    return std::clamp(std::isfinite(value) ? value : fallback, low, high);
}

uint64_t mixSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

uint64_t eventSeed(container::StringView eventName, uint64_t seed, uint64_t ordinal, uint64_t salt) noexcept {
    uint64_t hash = kFnv1aOffsetBasis;
    for (const unsigned char character : eventName) {
        hash ^= character;
        hash *= kFnv1aPrime;
    }
    return mixSeed(hash ^ seed ^ (ordinal * 0x9e3779b97f4a7c15ull) ^ salt);
}

float randomUnit(uint64_t seed) noexcept {
    constexpr double denominator = static_cast<double>(std::numeric_limits<uint32_t>::max());
    return static_cast<float>(static_cast<double>(static_cast<uint32_t>(seed >> 32u)) / denominator);
}

AudioPriority parsePriority(container::StringView value, AudioPriority fallback) noexcept {
    const container::String lower = canonicalName(value);
    if (lower == "lowest") return AudioPriority::Lowest;
    if (lower == "low") return AudioPriority::Low;
    if (lower == "normal") return AudioPriority::Normal;
    if (lower == "high") return AudioPriority::High;
    if (lower == "critical") return AudioPriority::Critical;
    return fallback;
}

uint16_t parseTypeFlags(container::StringView value, uint16_t fallback) noexcept {
    uint16_t flags = 0;
    for (const container::String& word : words(value)) {
        const container::String lower = lowercase(word);
        if (lower == "ui") flags |= audioEventFlag(AudioEventType::Ui);
        else if (lower == "world") flags |= audioEventFlag(AudioEventType::World);
        else if (lower == "shrouded") flags |= audioEventFlag(AudioEventType::Shrouded);
        else if (lower == "global") flags |= audioEventFlag(AudioEventType::Global);
        else if (lower == "voice") flags |= audioEventFlag(AudioEventType::Voice);
        else if (lower == "player") flags |= audioEventFlag(AudioEventType::Player);
        else if (lower == "allies") flags |= audioEventFlag(AudioEventType::Allies);
        else if (lower == "enemies") flags |= audioEventFlag(AudioEventType::Enemies);
        else if (lower == "everyone") flags |= audioEventFlag(AudioEventType::Everyone);
    }
    return flags == 0 ? fallback : flags;
}

uint16_t parseControlFlags(container::StringView value, uint16_t fallback) noexcept {
    uint16_t flags = 0;
    for (const container::String& word : words(value)) {
        const container::String lower = lowercase(word);
        if (lower == "loop") flags |= audioEventFlag(AudioEventControl::Loop);
        else if (lower == "random") flags |= audioEventFlag(AudioEventControl::Random);
        else if (lower == "all") flags |= audioEventFlag(AudioEventControl::All);
        else if (lower == "postdelay") flags |= audioEventFlag(AudioEventControl::PostDelay);
        else if (lower == "interrupt") flags |= audioEventFlag(AudioEventControl::Interrupt);
    }
    return flags == 0 && trim(value).empty() ? 0 : (flags == 0 ? fallback : flags);
}

void normalizeSlashes(container::String& value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
}

container::String joinPath(container::String first, container::StringView second) {
    normalizeSlashes(first);
    container::String tail = trim(second);
    normalizeSlashes(tail);
    if (tail.empty()) return first;
    if (first.empty()) return tail;
    if (tail.front() == '/') return first + tail;
    return first + '/' + tail;
}

bool hasExtension(container::StringView path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    return dot != container::StringView::npos && (slash == container::StringView::npos || dot > slash);
}

container::String assetPathFor(const AudioEventSettings& settings, const AudioEventDefinition& definition,
                         container::StringView source) {
    if (source.empty()) return {};
    container::String folder;
    switch (definition.kind) {
    case AudioEventKind::SoundEffect: folder = settings.soundsFolder; break;
    case AudioEventKind::Music: folder = settings.musicFolder; break;
    case AudioEventKind::Streaming: folder = settings.streamingFolder; break;
    }

    container::String path = joinPath(joinPath(settings.audioRoot, folder), source);
    // ZH's AudioEventRTS::generateFilenameExtension appends the authored
    // SoundsExtension to both ordinary samples and Streaming/Speech. Music
    // filenames are the sole exception because their extension is authored
    // directly (normally .mp3). Requiring every speech INI to spell out .wav
    // makes otherwise valid campaign dialogue resolve to an extensionless
    // VFS path.
    if (definition.kind != AudioEventKind::Music && !hasExtension(path) &&
        !settings.soundsExtension.empty()) {
        path += '.';
        path += settings.soundsExtension;
    }
    normalizeSlashes(path);
    return lowercase(path);
}

AudioEventDefinition defaultDefinition(container::String name, AudioEventKind kind) {
    AudioEventDefinition result;
    result.name = std::move(name);
    result.kind = kind;
    if (kind == AudioEventKind::Music || kind == AudioEventKind::Streaming) {
        result.typeFlags = audioEventFlag(AudioEventType::Ui) |
            audioEventFlag(AudioEventType::Everyone);
        result.controlFlags = 0;
        result.limit = 0;
    }
    return result;
}

void applyEventField(AudioEventDefinition& event, container::StringView key, container::StringView value) {
    if (key == "filename") event.filename = trim(value);
    else if (key == "volume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) event.volume = finiteClamp(parsed, 0.0f, 4.0f, event.volume);
    } else if (key == "volumeshift") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) event.volumeShift = finiteClamp(parsed, -1.0f, 1.0f, event.volumeShift);
    } else if (key == "minvolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) event.minVolume = finiteClamp(parsed, 0.0f, 4.0f, event.minVolume);
    } else if (key == "pitchshift") {
        const container::Vector<container::String> values = words(value);
        if (values.size() >= 2) {
            float minimum = 0.0f;
            float maximum = 0.0f;
            if (parseFloat(values[0], minimum) && parseFloat(values[1], maximum)) {
                event.pitchShiftMin = finiteClamp(1.0f + minimum / 100.0f,
                                                   kMinimumAuthoredPitch, kMaximumAudioPitch,
                                                   event.pitchShiftMin);
                event.pitchShiftMax = finiteClamp(1.0f + maximum / 100.0f,
                                                   event.pitchShiftMin, kMaximumAudioPitch,
                                                   event.pitchShiftMax);
            }
        }
    } else if (key == "delay") {
        const container::Vector<container::String> values = words(value);
        if (values.size() >= 2) {
            uint32_t minimum = 0;
            uint32_t maximum = 0;
            if (parseUnsigned(values[0], minimum) && parseUnsigned(values[1], maximum)) {
                event.delayMinMilliseconds = minimum;
                event.delayMaxMilliseconds = std::max(minimum, maximum);
            }
        }
    } else if (key == "limit") {
        int32_t parsed = 0;
        if (parseInteger(value, parsed)) event.limit = std::max(parsed, 0);
    } else if (key == "loopcount") {
        int32_t parsed = 0;
        if (parseInteger(value, parsed)) event.loopCount = std::max(parsed, 0);
    } else if (key == "priority") event.priority = parsePriority(value, event.priority);
    else if (key == "type") event.typeFlags = parseTypeFlags(value, event.typeFlags);
    else if (key == "control") event.controlFlags = parseControlFlags(value, event.controlFlags);
    else if (key == "sounds") event.sounds = words(value);
    else if (key == "soundsmorning") event.soundsMorning = words(value);
    else if (key == "soundsevening") event.soundsEvening = words(value);
    else if (key == "soundsnight") event.soundsNight = words(value);
    else if (key == "attack") event.attackSounds = words(value);
    else if (key == "decay") event.decaySounds = words(value);
    else if (key == "minrange") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) event.minDistance = std::max(parsed, 0.0f);
    } else if (key == "maxrange") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) event.maxDistance = std::max(parsed, event.minDistance + 0.001f);
    } else if (key == "lowpasscutoff") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) event.lowPassCutoff = finiteClamp(parsed, 0.0f, 1.0f, 0.0f);
    }
}

void applySettingField(AudioEventSettings& settings, container::StringView key, container::StringView value) {
    if (key == "audioroot") settings.audioRoot = trim(value);
    else if (key == "soundsfolder") settings.soundsFolder = trim(value);
    else if (key == "musicfolder") settings.musicFolder = trim(value);
    else if (key == "streamingfolder") settings.streamingFolder = trim(value);
    else if (key == "soundsextension") settings.soundsExtension = trim(value);
    else if (key == "samplecount2d") {
        uint32_t parsed = 0;
        if (parseUnsigned(value, parsed)) settings.sampleCount2D = std::max<size_t>(parsed, 1);
    } else if (key == "samplecount3d") {
        uint32_t parsed = 0;
        if (parseUnsigned(value, parsed)) settings.sampleCount3D = std::max<size_t>(parsed, 1);
    } else if (key == "streamcount") {
        uint32_t parsed = 0;
        if (parseUnsigned(value, parsed)) settings.streamCount = std::max<size_t>(parsed, 1);
    } else if (key == "audiofootprintinbytes") {
        uint32_t parsed = 0;
        if (parseUnsigned(value, parsed)) settings.cacheBudgetBytes = std::max<size_t>(parsed, 1);
    } else if (key == "globalminrange") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.globalMinDistance = std::max(parsed, 0.0f);
    } else if (key == "globalmaxrange") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.globalMaxDistance = std::max(parsed, settings.globalMinDistance + 0.001f);
    } else if (key == "minsamplevolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.minSampleVolume = finiteClamp(parsed, 0.0f, 1.0f, settings.minSampleVolume);
    } else if (key == "use3dsoundrangevolumefade") {
        bool parsed = false;
        if (parseYes(value, parsed)) settings.use3DRangeVolumeFade = parsed;
    } else if (key == "3dsoundrangevolumefadeexponent") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.rangeVolumeFadeExponent = finiteClamp(parsed, 0.01f, 16.0f, 1.0f);
    } else if (key == "defaultsoundvolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.defaultSoundVolume = finiteClamp(parsed, 0.0f, 1.0f, settings.defaultSoundVolume);
    } else if (key == "default3dsoundvolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.defaultSound3DVolume = finiteClamp(parsed, 0.0f, 1.0f, settings.defaultSound3DVolume);
    } else if (key == "defaultspeechvolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.defaultSpeechVolume = finiteClamp(parsed, 0.0f, 1.0f, settings.defaultSpeechVolume);
    } else if (key == "defaultmusicvolume") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.defaultMusicVolume = finiteClamp(parsed, 0.0f, 1.0f, settings.defaultMusicVolume);
    } else if (key == "microphonedesiredheightaboveterrain") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.listener.desiredHeightAbovePivot = std::max(parsed, 0.0f);
    } else if (key == "microphonemaxpercentagebetweengroundandcamera") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.listener.maxCameraFraction = finiteClamp(parsed, 0.0f, 1.0f, settings.listener.maxCameraFraction);
    } else if (key == "zoommindistance") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.listener.zoomMinDistance = std::max(parsed, 0.0f);
    } else if (key == "zoommaxdistance") {
        float parsed = 0.0f;
        if (parseFloat(value, parsed)) settings.listener.zoomMaxDistance = std::max(parsed, settings.listener.zoomMinDistance + 0.001f);
    } else if (key == "zoomsoundvolumepercentageamount") {
        float parsed = 0.0f;
        if (parsePercent(value, parsed)) settings.listener.zoomVolumeBoost = finiteClamp(parsed, 0.0f, 1.0f, settings.listener.zoomVolumeBoost);
    }
}

void applyMiscAudioField(AudioEventSettings& settings,
                         container::StringView key,
                         container::StringView value) {
    // MiscAudio is not an AudioEvent definition.  It selects named events
    // used by otherwise-generic gameplay/UI call sites.  Keep the selection
    // at the content boundary; presentation code must not hard-code a mod's
    // event name.
    if (key == "guiclicksound") settings.guiClickSound = trim(value);
}

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
}

} // namespace

void AudioEventCatalog::clear() {
    m_settings = {};
    m_events.clear();
}

bool AudioEventCatalog::loadFromVfs(container::String* error) {
    clear();
    auto& vfs = io::VFS::instance();
    size_t parsedLayers = 0;

    const container::Vector<container::String> sourceFiles =
        game::ini::enumerateLegacyIniDirectories(kAudioIniRoots);
    for (const container::String& path : sourceFiles) {
        // The sibling catalogs (DamageFx, Upgrade, ParticleSystem, FXList) all
        // check reachability before reading, because `readAll` returns an empty
        // String both for "empty file" and "could not open".  Without this an
        // audio INI that became unreadable mid-load was skipped in silence.
        if (!vfs.exists(path)) {
            if (error) {
                *error = "Audio INI disappeared from VFS during load: " + path;
            }
            clear();
            return false;
        }
        const container::Vector<container::String> layers{vfs.readAll(path)};
        for (const container::String& content : layers) {
            if (content.empty()) continue;
            ++parsedLayers;

            std::istringstream stream(content);
            container::String rawLine;
            bool inSettings = false;
            bool inMiscAudio = false;
            std::optional<AudioEventDefinition> active;

            const auto commit = [&]() {
                if (!active || active->name.empty()) return;
                const container::String key = canonicalName(active->name);
                if (!key.empty()) m_events.insert_or_assign(key, std::move(*active));
                active.reset();
            };

            while (std::getline(stream, rawLine)) {
                const container::String line = stripComment(rawLine);
                if (line.empty()) continue;
                const auto [command, remainder] = splitKeyValue(line);
                if (command.empty()) continue;

                if (command == "end") {
                    if (active) commit();
                    else {
                        inSettings = false;
                        inMiscAudio = false;
                    }
                    continue;
                }

                if (!active && command == "audiosettings") {
                    inSettings = true;
                    inMiscAudio = false;
                    continue;
                }

                if (!active && command == "miscaudio") {
                    inSettings = false;
                    inMiscAudio = true;
                    continue;
                }

                if (!active && (command == "audioevent" || command == "musictrack" ||
                                command == "dialogevent")) {
                    const AudioEventKind kind = command == "musictrack" ? AudioEventKind::Music :
                        (command == "dialogevent" ? AudioEventKind::Streaming : AudioEventKind::SoundEffect);
                    const container::String name = trim(remainder);
                    if (name.empty()) continue;
                    const container::String key = canonicalName(name);
                    const char* defaultName = kind == AudioEventKind::Music ? "defaultmusictrack" :
                        (kind == AudioEventKind::Streaming ? "defaultdialog" : "defaultsoundeffect");

                    // AudioEventInfo does not sparsely inherit a prior
                    // same-name definition. Every block starts from its
                    // category's current Default* template, then applies only
                    // this block's fields. This matters for later VFS layers
                    // and map.ini overrides: omitted fields must return to the
                    // authored default instead of leaking from an older event.
                    if (key != defaultName) {
                        if (const auto inherited = m_events.find(defaultName); inherited != m_events.end()) {
                            active = inherited->second;
                        } else {
                            active = defaultDefinition(name, kind);
                        }
                    } else {
                        // Default* definitions themselves are sparse patches
                        // in ZH. Only ordinary events restart from the current
                        // category default for every same-name redefinition.
                        if (const auto existing = m_events.find(key);
                            existing != m_events.end()) {
                            active = existing->second;
                        } else {
                            active = defaultDefinition(name, kind);
                        }
                    }
                    active->name = name;
                    active->kind = kind;
                    inSettings = false;
                    inMiscAudio = false;
                    continue;
                }

                if (active) {
                    applyEventField(*active, command, remainder);
                } else if (inSettings) {
                    applySettingField(m_settings, command, remainder);
                } else if (inMiscAudio) {
                    applyMiscAudioField(m_settings, command, remainder);
                }
            }
            commit();
        }
    }

    if (parsedLayers == 0) {
        setError(error, "no AudioSettings/AudioEvent INI layers were found in VFS");
        return false;
    }

    normalizeSlashes(m_settings.audioRoot);
    normalizeSlashes(m_settings.soundsFolder);
    normalizeSlashes(m_settings.musicFolder);
    normalizeSlashes(m_settings.streamingFolder);
    m_settings.soundsExtension = lowercase(m_settings.soundsExtension);
    TD_LOG_INFO("[AudioEventCatalog] Loaded {} events from {} layered VFS INI sources",
                m_events.size(), parsedLayers);
    return true;
}

bool AudioEventCatalog::applyOverrides(
    container::StringView content, container::StringView sourcePath,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourcePath)) {
        setError(error, "could not parse AudioEvent override content");
        return false;
    }

    AudioEventCatalog candidate = *this;
    for (const game::IniBlock& block : parser.blocks()) {
        const container::String type = canonicalName(block.type);
        if (type == "audiosettings") {
            for (const auto& [key, value] : block.values) {
                applySettingField(
                    candidate.m_settings, canonicalName(key), value);
            }
            continue;
        }
        if (type == "miscaudio") {
            for (const auto& [key, value] : block.values) {
                applyMiscAudioField(
                    candidate.m_settings, canonicalName(key), value);
            }
            continue;
        }
        if (type != "audioevent" && type != "musictrack" &&
            type != "dialogevent") {
            continue;
        }

        const AudioEventKind kind = type == "musictrack"
            ? AudioEventKind::Music
            : type == "dialogevent"
                ? AudioEventKind::Streaming
                : AudioEventKind::SoundEffect;
        const container::String name = trim(block.name);
        const container::String key = canonicalName(name);
        if (key.empty()) continue;
        const char* defaultName = kind == AudioEventKind::Music
            ? "defaultmusictrack"
            : kind == AudioEventKind::Streaming
                ? "defaultdialog"
                : "defaultsoundeffect";

        AudioEventDefinition definition;
        if (key == defaultName) {
            const auto existing = candidate.m_events.find(key);
            definition = existing != candidate.m_events.end()
                ? existing->second : defaultDefinition(name, kind);
        } else {
            const auto inherited = candidate.m_events.find(defaultName);
            definition = inherited != candidate.m_events.end()
                ? inherited->second : defaultDefinition(name, kind);
        }
        definition.name = name;
        definition.kind = kind;
        for (const auto& [field, value] : block.values) {
            applyEventField(definition, canonicalName(field), value);
        }
        candidate.m_events.insert_or_assign(key, std::move(definition));
    }

    normalizeSlashes(candidate.m_settings.audioRoot);
    normalizeSlashes(candidate.m_settings.soundsFolder);
    normalizeSlashes(candidate.m_settings.musicFolder);
    normalizeSlashes(candidate.m_settings.streamingFolder);
    candidate.m_settings.soundsExtension =
        lowercase(candidate.m_settings.soundsExtension);
    *this = std::move(candidate);
    return true;
}

bool AudioEventCatalog::applyOverridesFromVfs(
    container::StringView path, container::String* error) {
    if (error) error->clear();
    const container::String ownedPath{path};
    if (ownedPath.empty() || !io::VFS::instance().exists(ownedPath)) {
        setError(error, "AudioEvent override source is unavailable: " +
            ownedPath);
        return false;
    }
    return applyOverrides(
        io::VFS::instance().readAll(ownedPath), ownedPath, error);
}

const AudioEventDefinition* AudioEventCatalog::find(container::StringView eventName) const {
    const auto found = m_events.find(canonicalName(eventName));
    return found == m_events.end() ? nullptr : &found->second;
}

std::optional<ResolvedAudioEventSample> AudioEventCatalog::resolve(
    const AudioEventIntent& intent, AudioEventPlaybackCursor& cursor,
    AudioEventPortion portion, container::String* error) const {
    const AudioEventDefinition* definition = find(intent.eventName);
    if (!definition) {
        setError(error, "AudioEvent '" + intent.eventName + "' is not defined");
        return std::nullopt;
    }

    const container::Vector<container::String>* candidates = nullptr;
    switch (portion) {
    case AudioEventPortion::Attack: candidates = &definition->attackSounds; break;
    case AudioEventPortion::Main: candidates = &definition->sounds; break;
    case AudioEventPortion::Decay: candidates = &definition->decaySounds; break;
    }

    container::String source;
    if (definition->kind == AudioEventKind::Music || definition->kind == AudioEventKind::Streaming) {
        if (portion != AudioEventPortion::Main) return std::nullopt;
        source = definition->filename;
    } else {
        if (!candidates || candidates->empty()) return std::nullopt;
        int32_t* lastIndex = portion == AudioEventPortion::Attack ? &cursor.attackSampleIndex :
            (portion == AudioEventPortion::Decay ? &cursor.decaySampleIndex : &cursor.mainSampleIndex);
        const uint64_t ordinal = cursor.selectionOrdinal++;
        const uint64_t salt = portion == AudioEventPortion::Attack ? 0xA771ACull :
            (portion == AudioEventPortion::Decay ? 0xDEC0A11ull : 0x5A17ull);
        if (portion == AudioEventPortion::Main &&
            !hasAudioEventControl(definition->controlFlags, AudioEventControl::Random)) {
            *lastIndex = (*lastIndex + 1) % static_cast<int32_t>(candidates->size());
        } else if (portion != AudioEventPortion::Main ||
                   hasAudioEventControl(definition->controlFlags, AudioEventControl::Random)) {
            int32_t selected = static_cast<int32_t>(eventSeed(definition->name, intent.variationSeed,
                ordinal, salt) % candidates->size());
            if (portion == AudioEventPortion::Main && candidates->size() > 2 && selected == *lastIndex) {
                selected = (selected + 1) % static_cast<int32_t>(candidates->size());
            }
            *lastIndex = selected;
        }
        source = (*candidates)[static_cast<size_t>(*lastIndex)];
    }

    const container::String assetPath = assetPathFor(m_settings, *definition, source);
    if (assetPath.empty()) {
        setError(error, "AudioEvent '" + definition->name + "' has no playable asset for this portion");
        return std::nullopt;
    }

    if (!cursor.variationInitialized) {
        const uint64_t baseSeed = eventSeed(definition->name, intent.variationSeed, 0, 0xBADC0FFEEull);
        const float volumeMin = std::min(1.0f + definition->volumeShift, 1.0f);
        const float volumeMax = std::max(1.0f + definition->volumeShift, 1.0f);
        cursor.resolvedVolumeShift = volumeMin + (volumeMax - volumeMin) * randomUnit(baseSeed);
        cursor.resolvedPitch = definition->pitchShiftMin +
            (definition->pitchShiftMax - definition->pitchShiftMin) * randomUnit(mixSeed(baseSeed));
        cursor.variationInitialized = true;
    }

    ResolvedAudioEventSample result;
    result.portion = portion;
    result.request.assetPath = assetPath;
    result.request.priority = definition->priority;
    result.request.pitch = finiteClamp(cursor.resolvedPitch, kMinimumBackendPitch,
                                       kMaximumAudioPitch, 1.0f);
    result.request.volume = finiteClamp(definition->volume * cursor.resolvedVolumeShift *
        finiteClamp(intent.volumeScale, 0.0f, 4.0f, 1.0f), 0.0f, 4.0f, 1.0f);
    result.request.admissionVolume = finiteClamp(
        definition->volume * finiteClamp(intent.volumeScale, 0.0f, 4.0f, 1.0f),
        0.0f, 4.0f, 1.0f);
    result.request.minDistance = definition->minDistance;
    result.request.maxDistance = definition->maxDistance;
    result.request.bypassSpatialVolumeCull =
        definition->priority == AudioPriority::Critical ||
        hasAudioEventType(definition->typeFlags, AudioEventType::Global);

    const bool positional = definition->kind == AudioEventKind::SoundEffect &&
        hasAudioEventType(definition->typeFlags, AudioEventType::World) && intent.position.has_value();
    if (definition->kind == AudioEventKind::Music) {
        result.request.bus = AudioBus::Music;
        // Miles' playStream() loops MusicTrack streams independently of the
        // authored Control flags. Preserve that observable behavior at the
        // generic request boundary; normal AudioEvent LOOP is sequenced by
        // AudioEventSequencer so its variations/delay/Decay remain visible.
        result.request.loop = true;
    } else if (definition->kind == AudioEventKind::Streaming) {
        result.request.bus = AudioBus::Speech;
    } else {
        result.request.bus = positional ? AudioBus::Sound3D : AudioBus::Sound;
    }
    result.request.spatialMode = positional ? AudioSpatialMode::ThreeDimensional :
                                               AudioSpatialMode::TwoDimensional;
    if (positional) {
        result.request.position = *intent.position;
        if (hasAudioEventType(definition->typeFlags, AudioEventType::Global)) {
            result.request.minDistance = m_settings.globalMinDistance;
            result.request.maxDistance = m_settings.globalMaxDistance;
        }
    }

    if (portion == AudioEventPortion::Main && definition->delayMaxMilliseconds > 0) {
        const uint64_t delaySeed = eventSeed(definition->name, intent.variationSeed,
            cursor.selectionOrdinal, 0xD311AFull);
        const uint32_t range = definition->delayMaxMilliseconds - definition->delayMinMilliseconds;
        result.delayBeforeNextMilliseconds = definition->delayMinMilliseconds +
            static_cast<uint32_t>(delaySeed % (static_cast<uint64_t>(range) + 1u));
    }
    return result;
}

} // namespace engine::audio
