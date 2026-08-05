#include "core/container/hash_containers.h"
#include <miniaudio.h>

#include "AudioListener.h"
#include "AudioSystem.h"

#include "VFS.h"
#include "LocaleResourceLocator.h"
#include "engine/resource/ResourceSchedulerRuntime.h"
#include "debug/debug.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
namespace engine::audio {
namespace {

constexpr size_t kAudioBusCount = audioBusIndex(AudioBus::Count);

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool usable(const math::vec3& value) noexcept {
    const float lengthSq = value.length_sq();
    return finite(value.x()) && finite(value.y()) && finite(value.z()) &&
        finite(lengthSq) && lengthSq > math::EPSILON * math::EPSILON;
}

bool finiteVector(const math::vec3& value) noexcept {
    return finite(value.x()) && finite(value.y()) && finite(value.z());
}

bool validBus(AudioBus bus) noexcept {
    return audioBusIndex(bus) < kAudioBusCount;
}

float unit(float value, float fallback = 1.0f) noexcept {
    return std::clamp(finite(value) ? value : fallback, 0.0f, 1.0f);
}

// User/category controls are normalized units, while an authored AudioEvent
// volume is a linear gain and can legitimately exceed one. Cap only absurd
// malformed input; miniaudio's node output gain safely supports amplification.
float gain(float value, float fallback = 1.0f) noexcept {
    return std::clamp(finite(value) ? value : fallback, 0.0f, 4.0f);
}

container::String canonicalAssetPath(container::StringView path) {
    size_t first = 0;
    while (first < path.size() && std::isspace(static_cast<unsigned char>(path[first]))) ++first;
    size_t last = path.size();
    while (last > first && std::isspace(static_cast<unsigned char>(path[last - 1]))) --last;

    container::String result(path.substr(first, last - first));
    std::replace(result.begin(), result.end(), '\\', '/');
    // Audio assets are VFS-relative names, never host paths. Reject rooting,
    // drive/URI separators and traversal segments before they reach a local
    // VFS layer where `base / ../...` could otherwise escape its mount.
    if (result.empty() || result.front() == '/' || result.find(':') != container::String::npos) {
        return {};
    }
    size_t segmentStart = 0;
    while (segmentStart <= result.size()) {
        const size_t segmentEnd = result.find('/', segmentStart);
        const container::StringView segment(result.data() + segmentStart,
            (segmentEnd == container::String::npos ? result.size() : segmentEnd) - segmentStart);
        if (segment.empty() || segment == "." || segment == "..") return {};
        if (segmentEnd == container::String::npos) break;
        segmentStart = segmentEnd + 1;
    }
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

[[nodiscard]] std::optional<io::LocaleResourceKind> audioResourceKind(
    container::StringView canonicalPath) noexcept {
    if (canonicalPath.starts_with("data/audio/sounds/")) {
        return io::LocaleResourceKind::Sound;
    }
    if (canonicalPath.starts_with("data/audio/speech/")) {
        return io::LocaleResourceKind::Speech;
    }
    if (canonicalPath.starts_with("data/audio/tracks/")) {
        return io::LocaleResourceKind::Music;
    }
    return std::nullopt;
}

AudioListenerSnapshot sanitizeListener(AudioListenerSnapshot input) noexcept {
    if (!usable(input.position)) input.position = {};
    if (!usable(input.forward)) input.forward = {0.0f, 1.0f, 0.0f};
    else input.forward = input.forward.normalized();
    if (!usable(input.up)) input.up = {0.0f, 0.0f, 1.0f};
    else input.up = input.up.normalized();
    if (std::abs(input.forward.dot(input.up)) > 0.999f) {
        input.up = std::abs(input.forward.z()) < 0.999f
            ? math::vec3{0.0f, 0.0f, 1.0f}
            : math::vec3{0.0f, 1.0f, 0.0f};
    }
    input.zoomVolume = unit(input.zoomVolume);
    return input;
}

AudioPlayRequest sanitizeRequest(AudioPlayRequest input) {
    input.assetPath = canonicalAssetPath(input.assetPath);
    if (!validBus(input.bus)) input.bus = AudioBus::Sound;
    input.volume = gain(input.volume);
    input.admissionVolume = gain(input.admissionVolume);
    // The backend uses miniaudio's linear attenuation model, whose gain is
    // `1 - rolloff * distanceFraction`. Values above one become negative
    // before maxDistance rather than giving a sensible steeper falloff.
    input.rolloff = std::clamp(finite(input.rolloff) ? input.rolloff : 1.0f, 0.0f, 1.0f);
    input.pitch = std::clamp(finite(input.pitch) ? input.pitch : 1.0f,
                             kMinimumBackendPitch, kMaximumAudioPitch);
    input.minDistance = std::max(
        finite(input.minDistance) ? input.minDistance : kDefaultAudioMinimumDistance, 0.0f);
    input.maxDistance = std::max(
        finite(input.maxDistance) ? input.maxDistance : kDefaultAudioMaximumDistance,
                                 input.minDistance + 0.001f);
    if (!usable(input.position)) input.position = {};
    return input;
}

AudioVoiceTransform sanitizeTransform(AudioVoiceTransform input) noexcept {
    if (!finiteVector(input.position)) input.position = {};
    if (!finiteVector(input.velocity)) input.velocity = {};
    return input;
}

enum class AudioVoicePool : uint8_t {
    TwoDimensional,
    ThreeDimensional,
    Streaming,
};

AudioVoicePool voicePoolFor(const AudioPlayRequest& request) noexcept {
    if (request.bus == AudioBus::Music || request.bus == AudioBus::Speech) {
        return AudioVoicePool::Streaming;
    }
    return request.spatialMode == AudioSpatialMode::ThreeDimensional
        ? AudioVoicePool::ThreeDimensional
        : AudioVoicePool::TwoDimensional;
}

enum class AudioCommandType : uint8_t {
    Play,
    Stop,
    StopAll,
    SetVolume,
    UpdateTransform,
};

struct AudioCommand final {
    AudioCommandType type = AudioCommandType::Play;
    AudioVoiceHandle handle;
    std::optional<AudioBus> bus;
    AudioPlayRequest request;
    float volume = 1.0f;
    AudioVoiceTransform transform;
};

// The audio callback never reads this queue. It is intentionally a bounded,
// mutex-protected producer/consumer bridge: gameplay can submit a finite
// number of value commands while the presentation thread owns miniaudio state.
class AudioCommandQueue final {
public:
    explicit AudioCommandQueue(size_t capacity)
        : m_capacity(std::max<size_t>(capacity, 1)) {}

    [[nodiscard]] bool tryPush(AudioCommand command) {
        std::scoped_lock lock(m_mutex);
        if (m_closed || m_commands.size() >= m_capacity) return false;
        m_commands.push_back(std::move(command));
        return true;
    }

    [[nodiscard]] bool tryPop(AudioCommand& output) {
        std::scoped_lock lock(m_mutex);
        if (m_commands.empty()) return false;
        output = std::move(m_commands.front());
        m_commands.pop_front();
        return true;
    }

    void clear() noexcept {
        std::scoped_lock lock(m_mutex);
        m_commands.clear();
    }

    // Shutdown detaches the shared endpoint before it clears commands. A
    // producer may already hold a shared_ptr, so closing the queue is needed
    // in addition to clearing it: no late producer can leave a permanently
    // Pending handle after the consumer has gone away.
    void close() noexcept {
        std::scoped_lock lock(m_mutex);
        m_closed = true;
        m_commands.clear();
    }

    [[nodiscard]] size_t size() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_commands.size();
    }

private:
    const size_t m_capacity;
    mutable std::mutex m_mutex;
    container::Deque<AudioCommand> m_commands;
    bool m_closed = false;
};

enum class ClipDecodeProfile : uint8_t {
    PreserveSource,
    SpatialMono,
};

enum class ClipPrepareState : uint8_t {
    Queued,
    Preparing,
    Ready,
    Failed,
    Cancelled,
};

[[nodiscard]] ClipDecodeProfile decodeProfileFor(
    const AudioPlayRequest& request) noexcept {
    return request.spatialMode == AudioSpatialMode::ThreeDimensional
        ? ClipDecodeProfile::SpatialMono
        : ClipDecodeProfile::PreserveSource;
}

[[nodiscard]] container::String clipPrepareKey(
    container::StringView requestedPath, ClipDecodeProfile profile) {
    container::String key(requestedPath);
    key.push_back('\x1f');
    key.push_back(profile == ClipDecodeProfile::SpatialMono ? 'm' : 's');
    return key;
}

struct PreparedAudioClip final {
    container::String resolvedPath;
    container::Vector<uint8_t> encodedBytes;
};

struct StreamingAudioSource;

struct ClipPrepareCompletion final {
    uint64_t generation = 0;
    container::String assetKey;
    container::String prepareKey;
    ClipPrepareState state = ClipPrepareState::Failed;
    container::SharedPtr<const PreparedAudioClip> clip;
    container::SharedPtr<StreamingAudioSource> stream;
    container::String error;
};

// Jobs capture only this shared endpoint and immutable request values. The
// AudioSystem owner may reset or shut down without leaving a worker holding a
// raw Impl/AudioSystem pointer.
struct ClipPrepareSharedState final {
    std::mutex mutex;
    std::condition_variable idle;
    container::Deque<ClipPrepareCompletion> completions;
    uint64_t generation = 1;
    size_t activeJobs = 0;
    size_t reservedStreamBytes = 0;
    size_t streamBufferBudgetBytes = kDefaultStreamBufferBudgetBytes;
    bool accepting = true;
};

struct StreamingAudioSource final {
    ma_pcm_rb ring{};
    bool ringInitialized = false;
    std::atomic<bool> cancel{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> workerDone{false};
    std::mutex waitMutex;
    std::condition_variable wake;
    container::WeakPtr<ClipPrepareSharedState> budgetOwner;
    size_t reservedBytes = 0;

    ~StreamingAudioSource() {
        if (ringInitialized) ma_pcm_rb_uninit(&ring);
        if (reservedBytes != 0) {
            if (const auto owner = budgetOwner.lock()) {
                std::scoped_lock lock(owner->mutex);
                owner->reservedStreamBytes -=
                    std::min(owner->reservedStreamBytes, reservedBytes);
            }
        }
    }

    void requestCancel() noexcept {
        cancel.store(true, std::memory_order_release);
        wake.notify_all();
    }

    void waitForWorker() noexcept {
        std::unique_lock lock(waitMutex);
        wake.wait(lock, [this] {
            return workerDone.load(std::memory_order_acquire);
        });
    }
};

[[nodiscard]] bool resolveAudioAsset(container::StringView assetKey,
                                     container::String& resolvedPath,
                                     container::String* error) {
    const auto locator = io::acquireLocaleResourceLocator();
    if (locator) {
        const std::optional<io::LocaleResourceKind> kind =
            audioResourceKind(assetKey);
        if (kind) {
            if (const auto resolved = locator->resolve(*kind, assetKey)) {
                resolvedPath = *resolved;
            }
        } else if (locator->contains(assetKey)) {
            resolvedPath = container::String(assetKey);
        }
    } else if (io::VFS::instance().exists(assetKey)) {
        resolvedPath = container::String(assetKey);
    }
    if (!resolvedPath.empty()) return true;
    if (error) {
        *error = "VFS could not resolve audio asset '" +
            container::String(assetKey) + "' for the active locale";
    }
    return false;
}

ma_result streamFileRead(ma_decoder* decoder, void* output, size_t bytesToRead,
                         size_t* bytesRead) {
    if (bytesRead) *bytesRead = 0;
    auto* file = static_cast<io::File*>(decoder ? decoder->pUserData : nullptr);
    if (!file || !output) return MA_INVALID_ARGS;
    const size_t read = file->read(output, bytesToRead);
    if (bytesRead) *bytesRead = read;
    if (read != 0) return MA_SUCCESS;
    return file->tell() >= file->size() ? MA_AT_END : MA_ERROR;
}

ma_result streamFileSeek(ma_decoder* decoder, ma_int64 offset,
                         ma_seek_origin origin) {
    auto* file = static_cast<io::File*>(decoder ? decoder->pUserData : nullptr);
    if (!file) return MA_INVALID_ARGS;
    const io::FileSeek seekOrigin = origin == ma_seek_origin_current
        ? io::FileSeek::Current : io::FileSeek::Start;
    return file->seek(offset, seekOrigin) >= 0 ? MA_SUCCESS : MA_BAD_SEEK;
}

[[nodiscard]] container::SharedPtr<const PreparedAudioClip> prepareAudioClip(
    container::StringView assetKey, ClipDecodeProfile profile,
    container::String* error) {
    container::String resolvedPath;
    if (!resolveAudioAsset(assetKey, resolvedPath, error)) return {};

    container::Vector<uint8_t> bytes;
    if (!io::VFS::instance().readToBuffer(resolvedPath, bytes) ||
        bytes.empty()) {
        if (error) {
            *error = "VFS could not resolve audio asset '" +
                container::String(assetKey) + "' for the active locale";
        }
        return {};
    }

    ma_decoder_config decoderConfig = ma_decoder_config_init(
        profile == ClipDecodeProfile::SpatialMono
            ? ma_format_f32 : ma_format_unknown,
        profile == ClipDecodeProfile::SpatialMono ? 1u : 0u, 0);
    const ma_decoder_config* config =
        profile == ClipDecodeProfile::SpatialMono ? &decoderConfig : nullptr;
    ma_decoder decoder{};
    const ma_result result = ma_decoder_init_memory(
        bytes.data(), bytes.size(), config, &decoder);
    if (result != MA_SUCCESS) {
        if (error) {
            *error = "miniaudio could not decode '" + resolvedPath + "': " +
                ma_result_description(result);
        }
        return {};
    }
    ma_decoder_uninit(&decoder);

    auto clip = std::make_shared<PreparedAudioClip>();
    clip->resolvedPath = std::move(resolvedPath);
    clip->encodedBytes = std::move(bytes);
    return clip;
}

void runStreamingAudioJob(
    const container::SharedPtr<ClipPrepareSharedState>& shared,
    const container::SharedPtr<StreamingAudioSource>& source,
    uint64_t generation, container::String assetKey,
    container::String prepareKey, AudioPlayRequest request,
    size_t configuredBufferFrames, size_t configuredWatermarkFrames) {
    ClipPrepareCompletion completion;
    completion.generation = generation;
    completion.assetKey = std::move(assetKey);
    completion.prepareKey = std::move(prepareKey);
    completion.stream = source;

    bool publishedReady = false;
    auto publish = [&](ClipPrepareState state, container::String error = {}) {
        completion.state = state;
        completion.error = std::move(error);
        std::scoped_lock lock(shared->mutex);
        if (shared->accepting && generation == shared->generation &&
            !source->cancel.load(std::memory_order_acquire)) {
            shared->completions.push_back(completion);
        }
    };
    auto fail = [&](container::String error) {
        source->failed.store(true, std::memory_order_release);
        if (!publishedReady) publish(ClipPrepareState::Failed, std::move(error));
    };

    container::String resolvedPath;
    container::String error;
    container::UniquePtr<io::File> file;
    ma_decoder decoder{};
    bool decoderInitialized = false;

    if (!resolveAudioAsset(completion.assetKey, resolvedPath, &error) ||
        !io::VFS::instance().open(resolvedPath, file, io::FileAccess::Read) ||
        !file || file->size() <= 0) {
        if (error.empty()) error = "VFS could not open streaming audio asset '" + resolvedPath + "'";
        fail(std::move(error));
    } else {
        ma_decoder_config decoderConfig = ma_decoder_config_init(
            ma_format_f32,
            request.spatialMode == AudioSpatialMode::ThreeDimensional ? 1u : 0u,
            0);
        ma_result result = ma_decoder_init(
            streamFileRead, streamFileSeek, file.get(), &decoderConfig, &decoder);
        if (result != MA_SUCCESS) {
            fail("stream decoder init failed: " +
                 container::String(ma_result_description(result)));
        } else {
            decoderInitialized = true;
            ma_format format = ma_format_unknown;
            ma_uint32 channels = 0;
            ma_uint32 sampleRate = 0;
            result = ma_decoder_get_data_format(
                &decoder, &format, &channels, &sampleRate, nullptr, 0);
            const size_t frameBytes = static_cast<size_t>(channels) * sizeof(float);
            const size_t bufferFrames = std::max<size_t>(configuredBufferFrames, 1);
            if (result != MA_SUCCESS || format != ma_format_f32 || channels == 0 ||
                sampleRate == 0 || frameBytes > std::numeric_limits<size_t>::max() / bufferFrames) {
                fail("stream decoder returned an invalid PCM format");
            } else {
                const size_t reserveBytes = frameBytes * bufferFrames;
                bool reserved = false;
                {
                    std::scoped_lock lock(shared->mutex);
                    if (shared->accepting && generation == shared->generation &&
                        reserveBytes <= shared->streamBufferBudgetBytes -
                            std::min(shared->streamBufferBudgetBytes,
                                     shared->reservedStreamBytes)) {
                        shared->reservedStreamBytes += reserveBytes;
                        reserved = true;
                    }
                }
                if (!reserved) {
                    fail("stream PCM buffer budget is exhausted");
                } else {
                    source->budgetOwner = shared;
                    source->reservedBytes = reserveBytes;
                    result = ma_pcm_rb_init(
                        ma_format_f32, channels,
                        static_cast<ma_uint32>(std::min<size_t>(
                            bufferFrames, std::numeric_limits<ma_uint32>::max())),
                        nullptr, nullptr, &source->ring);
                    if (result != MA_SUCCESS) {
                        fail("stream PCM ring init failed: " +
                             container::String(ma_result_description(result)));
                    } else {
                        source->ringInitialized = true;
                        ma_pcm_rb_set_sample_rate(&source->ring, sampleRate);
                        const ma_uint32 watermark = static_cast<ma_uint32>(
                            std::max<size_t>(1, std::min(
                                configuredWatermarkFrames, bufferFrames)));
                        bool decodedAny = false;
                        unsigned emptyLoopPasses = 0;
                        while (!source->cancel.load(std::memory_order_acquire)) {
                            ma_uint32 writable = ma_pcm_rb_available_write(&source->ring);
                            if (writable == 0) {
                                std::unique_lock lock(source->waitMutex);
                                source->wake.wait_for(lock, std::chrono::milliseconds(2), [&] {
                                    return source->cancel.load(std::memory_order_acquire);
                                });
                                continue;
                            }
                            writable = std::min<ma_uint32>(writable, 4096);
                            void* destination = nullptr;
                            result = ma_pcm_rb_acquire_write(
                                &source->ring, &writable, &destination);
                            if (result != MA_SUCCESS || writable == 0) continue;
                            ma_uint64 decoded = 0;
                            result = ma_decoder_read_pcm_frames(
                                &decoder, destination, writable, &decoded);
                            if (decoded != 0) {
                                ma_pcm_rb_commit_write(
                                    &source->ring, static_cast<ma_uint32>(decoded));
                                decodedAny = true;
                                emptyLoopPasses = 0;
                            }
                            if (!publishedReady &&
                                ma_pcm_rb_available_read(&source->ring) >= watermark) {
                                publishedReady = true;
                                publish(ClipPrepareState::Ready);
                            }
                            if (result != MA_SUCCESS && result != MA_AT_END) {
                                fail("stream decode failed: " +
                                     container::String(ma_result_description(result)));
                                break;
                            }
                            if (decoded == 0 || result == MA_AT_END) {
                                if (request.loop) {
                                    if (++emptyLoopPasses > 1 ||
                                        ma_decoder_seek_to_pcm_frame(&decoder, 0) != MA_SUCCESS) {
                                        fail("stream loop could not seek to its beginning");
                                        break;
                                    }
                                    continue;
                                }
                                source->eof.store(true, std::memory_order_release);
                                if (!publishedReady) {
                                    if (decodedAny && ma_pcm_rb_available_read(&source->ring) != 0) {
                                        publishedReady = true;
                                        publish(ClipPrepareState::Ready);
                                    } else {
                                        fail("stream contains no decodable PCM frames");
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (decoderInitialized) ma_decoder_uninit(&decoder);
    if (file) file->close();
    // Publish completion under waitMutex.  waitForWorker() blocks in an untimed
    // wait whose predicate is exactly this flag, so a store+notify performed
    // outside the lock can land between the waiter's predicate check and its
    // block — the notify is then lost and the presentation thread hangs forever
    // inside destroyVoice()/shutdown()/stopPendingVoice().
    {
        std::lock_guard lock(source->waitMutex);
        source->workerDone.store(true, std::memory_order_release);
    }
    source->wake.notify_all();
}

} // namespace

struct AudioSystem::Impl final {
    struct CachedClip final {
        container::SharedPtr<const PreparedAudioClip> clip;
        size_t bytes = 0;
        uint64_t lastUse = 0;
    };

    struct Voice final {
        AudioVoiceHandle handle;
        AudioPlayRequest request;
        AudioVoiceTransform transform;
        container::SharedPtr<const PreparedAudioClip> clip;
        container::SharedPtr<StreamingAudioSource> stream;
        ma_decoder decoder{};
        ma_sound sound{};
        bool decoderInitialized = false;
        bool soundInitialized = false;
    };

    struct PendingVoice final {
        AudioPlayRequest request;
        AudioVoiceTransform transform;
        container::String prepareKey;
    };

    struct PrepareFlight final {
        uint64_t generation = 0;
        ClipPrepareState state = ClipPrepareState::Queued;
        container::Vector<uint64_t> waitingHandles;
        container::SharedPtr<StreamingAudioSource> stream;
    };

    AudioSystemConfig config;
    // Producers may live on a confirmed-logic/client thread while shutdown is
    // driven by the presentation thread. Keep the queue endpoint shared until
    // every in-flight producer releases its local reference; otherwise a
    // producer could pass an initialized check just as shutdown deleted the
    // queue underneath it.
    mutable std::mutex commandEndpointMutex;
    container::SharedPtr<AudioCommandQueue> commands;
    std::atomic<uint64_t> nextVoiceValue{1};
    std::atomic<uint64_t> acceptedCommands{0};
    std::atomic<uint64_t> droppedCommands{0};
    std::atomic<uint64_t> processedCommands{0};
    std::atomic<uint64_t> failedPlayRequests{0};
    std::atomic<uint64_t> completedVoices{0};
    std::atomic<uint64_t> preemptedVoices{0};

    static constexpr size_t kRetainedTerminalVoiceStates = 1024;
    mutable std::mutex voiceStateMutex;
    container::HashMap<uint64_t, AudioVoiceState> voiceStates;
    container::Deque<uint64_t> retainedTerminalVoiceOrder;

    std::atomic<bool> initialized{false};
    // `engineAvailable` includes an explicit offline miniaudio graph. Keep
    // it distinct from an actual playback device so a dedicated server can
    // remain a cheap suppressed no-op while tests still exercise EOS.
    std::atomic<bool> engineAvailable{false};
    std::atomic<bool> playbackDeviceAvailable{false};
    std::atomic<bool> offlineMixerAvailable{false};
    ma_engine engine{};
    container::Array<ma_sound_group, kAudioBusCount> busGroups{};
    container::Array<bool, kAudioBusCount> busGroupInitialized{};
    container::Array<AudioBusState, kAudioBusCount> buses{};
    AudioListenerSnapshot listener{};
    container::HashMap<container::String, CachedClip> clips;
    size_t cachedClipBytes = 0;
    uint64_t nextClipUse = 1;
    container::HashMap<uint64_t, container::UniquePtr<Voice>> voices;
    container::HashMap<uint64_t, PendingVoice> pendingVoices;
    container::HashMap<container::String, PrepareFlight> prepareFlights;
    container::SharedPtr<ClipPrepareSharedState> prepareState =
        std::make_shared<ClipPrepareSharedState>();

    [[nodiscard]] container::SharedPtr<AudioCommandQueue> acquireCommandQueue() const {
        std::scoped_lock lock(commandEndpointMutex);
        if (!initialized.load(std::memory_order_acquire)) return {};
        return commands;
    }

    [[nodiscard]] container::SharedPtr<AudioCommandQueue> detachCommandQueueForShutdown() {
        std::scoped_lock lock(commandEndpointMutex);
        if (!initialized.exchange(false, std::memory_order_acq_rel)) return {};
        return std::move(commands);
    }

    void setVoiceState(AudioVoiceHandle handle, AudioVoiceState state) noexcept {
        if (!handle) return;
        std::scoped_lock lock(voiceStateMutex);
        voiceStates[handle.value] = state;
        switch (state) {
        case AudioVoiceState::Completed:
        case AudioVoiceState::Stopped:
        case AudioVoiceState::Preempted:
        case AudioVoiceState::Failed:
        case AudioVoiceState::Suppressed:
            retainedTerminalVoiceOrder.push_back(handle.value);
            while (retainedTerminalVoiceOrder.size() > kRetainedTerminalVoiceStates) {
                const uint64_t expired = retainedTerminalVoiceOrder.front();
                retainedTerminalVoiceOrder.pop_front();
                const auto found = voiceStates.find(expired);
                if (found != voiceStates.end() &&
                    found->second != AudioVoiceState::Pending &&
                    found->second != AudioVoiceState::Playing) {
                    voiceStates.erase(found);
                }
            }
            break;
        case AudioVoiceState::Unknown:
        case AudioVoiceState::Pending:
        case AudioVoiceState::Playing:
            break;
        }
    }

    [[nodiscard]] AudioVoiceState queryVoiceState(AudioVoiceHandle handle) const noexcept {
        if (!handle) return AudioVoiceState::Unknown;
        std::scoped_lock lock(voiceStateMutex);
        const auto found = voiceStates.find(handle.value);
        return found == voiceStates.end() ? AudioVoiceState::Unknown : found->second;
    }

    void eraseVoiceState(AudioVoiceHandle handle) noexcept {
        if (!handle) return;
        std::scoped_lock lock(voiceStateMutex);
        voiceStates.erase(handle.value);
    }

    void stopTrackedVoices() noexcept {
        std::scoped_lock lock(voiceStateMutex);
        for (auto& [handle, state] : voiceStates) {
            if (state == AudioVoiceState::Pending || state == AudioVoiceState::Playing) {
                state = AudioVoiceState::Stopped;
                retainedTerminalVoiceOrder.push_back(handle);
            }
        }
        while (retainedTerminalVoiceOrder.size() > kRetainedTerminalVoiceStates) {
            const uint64_t expired = retainedTerminalVoiceOrder.front();
            retainedTerminalVoiceOrder.pop_front();
            const auto found = voiceStates.find(expired);
            if (found != voiceStates.end() &&
                found->second != AudioVoiceState::Pending &&
                found->second != AudioVoiceState::Playing) {
                voiceStates.erase(found);
            }
        }
    }

    void applyBus(size_t index) noexcept {
        if (!engineAvailable || index >= kAudioBusCount) return;
        const AudioBusState state = buses[index];
        if (busGroupInitialized[index]) {
            float volume = state.enabled ? unit(state.volume) : 0.0f;
            if (static_cast<AudioBus>(index) == AudioBus::Sound3D ||
                static_cast<AudioBus>(index) == AudioBus::Ambient) {
                volume *= listener.zoomVolume;
            }
            // `ma_sound_set_volume()` writes the spatializer gainer's plain
            // float, which races miniaudio's device callback. Node output-bus
            // gain is explicitly atomic in miniaudio and gives a sound group
            // the same category-gain role without that unsafe mutation.
            ma_node_set_output_bus_volume(
                reinterpret_cast<ma_node*>(&busGroups[index]), 0, volume);
            return;
        }
        // A rare individual group-init failure must not silently bypass the
        // user's mute/volume setting. Voices can still attach to engine root;
        // apply this bus gain per voice in that degraded path.
        for (auto& [_, voice] : voices) {
            if (audioBusIndex(voice->request.bus) == index) applyVoiceVolume(*voice);
        }
    }

    void applyBusPause(size_t index) noexcept {
        if (!engineAvailable || index >= kAudioBusCount) return;
        const bool paused = buses[index].paused;
        if (busGroupInitialized[index]) {
            if (paused) {
                static_cast<void>(ma_sound_group_stop(&busGroups[index]));
            } else {
                static_cast<void>(ma_sound_group_start(&busGroups[index]));
            }
            return;
        }
        // A failed group initialization attaches voices directly to the
        // engine root. Preserve the same pause contract in that degraded
        // path instead of silently turning pause into a mute.
        for (auto& [_, voice] : voices) {
            if (!voice->soundInitialized ||
                audioBusIndex(voice->request.bus) != index) {
                continue;
            }
            if (paused) {
                static_cast<void>(ma_sound_stop(&voice->sound));
            } else if (!ma_sound_at_end(&voice->sound)) {
                static_cast<void>(ma_sound_start(&voice->sound));
            }
        }
    }

    [[nodiscard]] float spatialRangeGain(
        const AudioPlayRequest& request,
        const math::vec3& sourcePosition) const noexcept {
        if (request.spatialMode != AudioSpatialMode::ThreeDimensional) {
            return 1.0f;
        }
        const math::vec3 offset = sourcePosition - listener.position;
        const float distanceSquared = offset.length_sq();
        if (!finite(distanceSquared) || distanceSquared < 0.0f) return 0.0f;
        const float distance = std::sqrt(distanceSquared);
        if (distance >= request.maxDistance) return 0.0f;
        if (!config.use3DRangeVolumeFade ||
            distance <= request.minDistance) {
            return 1.0f;
        }
        const float range = request.maxDistance - request.minDistance;
        if (!(range > 0.0f)) return 0.0f;
        const float fraction = std::clamp(
            (distance - request.minDistance) / range, 0.0f, 1.0f);
        return 1.0f - std::pow(fraction, config.rangeVolumeFadeExponent);
    }

    [[nodiscard]] float spatialRangeGain(const Voice& voice) const noexcept {
        return spatialRangeGain(voice.request, voice.transform.position);
    }

    [[nodiscard]] bool belowSampleVolumeThreshold(
        const AudioPlayRequest& request,
        const math::vec3& sourcePosition) const noexcept {
        if (request.bypassSpatialVolumeCull) return false;
        const float effective = request.spatialMode == AudioSpatialMode::ThreeDimensional
            ? request.volume * spatialRangeGain(request, sourcePosition)
            : request.admissionVolume;
        return effective < config.minSampleVolume;
    }

    void applySpatializationPolicy(Voice& voice) noexcept {
        if (!voice.soundInitialized ||
            voice.request.spatialMode != AudioSpatialMode::ThreeDimensional) {
            return;
        }
        ma_sound_set_attenuation_model(
            &voice.sound,
            config.use3DRangeVolumeFade ? ma_attenuation_model_none
                                        : ma_attenuation_model_linear);
        ma_sound_set_rolloff(&voice.sound, voice.request.rolloff);
        ma_sound_set_min_distance(&voice.sound, voice.request.minDistance);
        ma_sound_set_max_distance(&voice.sound, voice.request.maxDistance);
    }

    void applyVoiceVolume(Voice& voice) noexcept {
        if (!voice.soundInitialized) return;
        float volume = voice.request.volume * spatialRangeGain(voice);
        const size_t busIndex = audioBusIndex(voice.request.bus);
        if (!busGroupInitialized[busIndex]) {
            const AudioBusState state = buses[busIndex];
            if (voice.request.spatialMode == AudioSpatialMode::ThreeDimensional) {
                volume *= listener.zoomVolume;
            }
            volume *= state.enabled ? unit(state.volume) : 0.0f;
        }
        // See applyBus(): use the atomically published node output gain rather
        // than `ma_sound_set_volume()`'s non-atomic spatializer gainer.
        ma_node_set_output_bus_volume(
            reinterpret_cast<ma_node*>(&voice.sound), 0, gain(volume));
    }

    void applyListener() noexcept {
        if (!engineAvailable) return;
        const AudioListenerSnapshot state = listener;
        const math::vec3 position = AudioListenerBuilder::toAudioSpace(state.position);
        const math::vec3 forward = AudioListenerBuilder::toAudioSpace(state.forward);
        ma_engine_listener_set_position(&engine, 0, position.x(), position.y(), position.z());
        ma_engine_listener_set_direction(&engine, 0, forward.x(), forward.y(), forward.z());
        // miniaudio stores world-up in non-atomic listener config while its
        // device thread spatializes concurrently. The coordinate bridge makes
        // W3D +Z exactly miniaudio +Y, which is already miniaudio's immutable
        // default; do not rewrite this field every presentation frame.
        // Zoom gain is category-wide. A healthy Sound3D group receives it as
        // one atomic node-output update; only the rare no-group fallback has
        // to touch individual voices.
        const size_t sound3DIndex = audioBusIndex(AudioBus::Sound3D);
        const size_t ambientIndex = audioBusIndex(AudioBus::Ambient);
        applyBus(sound3DIndex);
        applyBus(ambientIndex);
        for (auto& [_, voice] : voices) {
            if (voice->request.spatialMode == AudioSpatialMode::ThreeDimensional) {
                applyVoiceVolume(*voice);
            }
        }
    }

    void destroyVoice(container::HashMap<uint64_t, container::UniquePtr<Voice>>::iterator it,
                      AudioVoiceState terminalState = AudioVoiceState::Stopped) noexcept {
        Voice& voice = *it->second;
        if (voice.soundInitialized) {
            ma_sound_stop(&voice.sound);
            ma_sound_uninit(&voice.sound);
            voice.soundInitialized = false;
        }
        if (voice.stream) {
            voice.stream->requestCancel();
            voice.stream->waitForWorker();
        }
        if (voice.decoderInitialized) {
            ma_decoder_uninit(&voice.decoder);
            voice.decoderInitialized = false;
        }
        setVoiceState(voice.handle, terminalState);
        voices.erase(it);
    }

    void destroyAllVoices(AudioVoiceState terminalState = AudioVoiceState::Stopped) noexcept {
        while (!voices.empty()) destroyVoice(voices.begin(), terminalState);
    }

    bool stopPendingVoice(uint64_t handle) noexcept {
        const auto pending = pendingVoices.find(handle);
        if (pending == pendingVoices.end()) return false;
        const container::String prepareKey = pending->second.prepareKey;
        pendingVoices.erase(pending);
        const auto flight = prepareFlights.find(prepareKey);
        if (flight != prepareFlights.end()) {
            auto& waiting = flight->second.waitingHandles;
            std::erase(waiting, handle);
            if (flight->second.stream) {
                const auto stream = flight->second.stream;
                stream->requestCancel();
                stream->waitForWorker();
                prepareFlights.erase(flight);
            }
        }
        setVoiceState(AudioVoiceHandle{handle}, AudioVoiceState::Stopped);
        return true;
    }

    [[nodiscard]] container::SharedPtr<const PreparedAudioClip> loadClip(container::StringView assetPath,
                                                          container::String* error) {
        const container::String key = canonicalAssetPath(assetPath);
        if (key.empty()) {
            if (error) *error = "audio asset path is empty";
            return {};
        }
        if (const auto found = clips.find(key); found != clips.end()) {
            found->second.lastUse = nextClipUse++;
            return found->second.clip;
        }
        auto clip = prepareAudioClip(
            key, ClipDecodeProfile::PreserveSource, error);
        if (!clip) return {};
        const size_t clipBytes = clip->encodedBytes.size();
        clips.emplace(key, CachedClip{
            .clip = clip,
            .bytes = clipBytes,
            .lastUse = nextClipUse++,
        });
        cachedClipBytes += clipBytes;
        trimClipCache();
        return clip;
    }

    [[nodiscard]] container::SharedPtr<const PreparedAudioClip>
    installPreparedClip(
        const container::String& assetKey,
        container::SharedPtr<const PreparedAudioClip> clip) {
        if (const auto existing = clips.find(assetKey);
            existing != clips.end()) {
            existing->second.lastUse = nextClipUse++;
            return existing->second.clip;
        }
        const size_t bytes = clip ? clip->encodedBytes.size() : 0;
        clips.emplace(assetKey, CachedClip{
            .clip = clip,
            .bytes = bytes,
            .lastUse = nextClipUse++,
        });
        cachedClipBytes += bytes;
        trimClipCache();
        return clip;
    }

    [[nodiscard]] bool queueVoicePreparation(
        AudioVoiceHandle handle, AudioPlayRequest request,
        container::String* error) {
        const container::String assetKey = canonicalAssetPath(request.assetPath);
        if (assetKey.empty()) {
            if (error) *error = "audio asset path is empty";
            return false;
        }
        if (request.bus == AudioBus::Music || request.bus == AudioBus::Speech) {
            const container::String prepareKey =
                assetKey + "\x1estream\x1e" + std::to_string(handle.value);
            PendingVoice pending{
                .request = std::move(request),
                .transform = {},
                .prepareKey = prepareKey,
            };
            pending.transform.position = pending.request.position;
            pendingVoices.insert_or_assign(handle.value, std::move(pending));

            const auto shared = prepareState;
            auto source = std::make_shared<StreamingAudioSource>();
            uint64_t generation = 0;
            {
                std::scoped_lock lock(shared->mutex);
                if (!shared->accepting) {
                    pendingVoices.erase(handle.value);
                    if (error) *error = "audio prepare service is shutting down";
                    return false;
                }
                generation = shared->generation;
                ++shared->activeJobs;
            }
            PrepareFlight flight;
            flight.generation = generation;
            flight.state = ClipPrepareState::Preparing;
            flight.waitingHandles.push_back(handle.value);
            flight.stream = source;
            prepareFlights.emplace(prepareKey, std::move(flight));
            const AudioPlayRequest streamRequest = pendingVoices.find(handle.value)->second.request;
            try {
                engine::resource::ResourceSchedulerRuntime* scheduler =
                    engine::resource::activeResourceSchedulerRuntime();
                if (!scheduler) throw std::runtime_error(
                    "resource scheduler is unavailable");
                engine::resource::ResourceRequest resourceRequest;
                resourceRequest.key.kind =
                    engine::resource::ResourceKind::Audio;
                resourceRequest.key.canonicalIdentity = assetKey;
                resourceRequest.key.variant = handle.value;
                resourceRequest.demand =
                    engine::resource::ResourceDemand::Visible;
                resourceRequest.estimatedBytes = static_cast<uint64_t>(
                    config.streamBufferFrames) * 2u * sizeof(float);
                const engine::resource::ResourceSubmitResult submitted =
                    scheduler->submit(
                    std::move(resourceRequest),
                    [shared, source, generation, assetKey, prepareKey,
                     streamRequest, bufferFrames = config.streamBufferFrames,
                     watermarkFrames = config.streamStartWatermarkFrames](
                        const engine::resource::ResourceTaskContext& context) mutable {
                        if (context.stopRequested()) {
                            source->requestCancel();
                            // Same lost-wakeup rule as the job tail: the flag
                            // waitForWorker() waits on must be published under
                            // waitMutex, or the notify can be missed entirely.
                            {
                                std::lock_guard lock(source->waitMutex);
                                source->workerDone.store(true,
                                    std::memory_order_release);
                            }
                            source->wake.notify_all();
                            return engine::resource::ResourceTaskResult::Failed;
                        }
                        runStreamingAudioJob(
                            shared, source, generation, std::move(assetKey),
                            std::move(prepareKey), std::move(streamRequest),
                            bufferFrames, watermarkFrames);
                        return source->failed.load(std::memory_order_acquire)
                            ? engine::resource::ResourceTaskResult::Failed
                            : engine::resource::ResourceTaskResult::Ready;
                    },
                    [shared](const engine::resource::ResourceCompletion&) {
                        std::scoped_lock lock(shared->mutex);
                        --shared->activeJobs;
                        shared->idle.notify_all();
                    });
                if (!submitted.accepted()) {
                    throw std::runtime_error(
                        "resource scheduler rejected audio stream task");
                }
            } catch (...) {
                source->requestCancel();
                {
                    std::scoped_lock lock(shared->mutex);
                    --shared->activeJobs;
                }
                shared->idle.notify_all();
                prepareFlights.erase(prepareKey);
                pendingVoices.erase(handle.value);
                if (error) *error = "could not schedule audio stream task";
                return false;
            }
            return true;
        }
        if (const auto cached = clips.find(assetKey); cached != clips.end()) {
            cached->second.lastUse = nextClipUse++;
            return startVoice(handle, std::move(request), cached->second.clip,
                              error);
        }

        const ClipDecodeProfile profile = decodeProfileFor(request);
        const container::String prepareKey =
            clipPrepareKey(assetKey, profile);
        PendingVoice pending{
            .request = std::move(request),
            .transform = {},
            .prepareKey = prepareKey,
        };
        pending.transform.position = pending.request.position;
        pendingVoices.insert_or_assign(handle.value, std::move(pending));

        if (auto flight = prepareFlights.find(prepareKey);
            flight != prepareFlights.end()) {
            flight->second.waitingHandles.push_back(handle.value);
            return true;
        }

        const container::SharedPtr<ClipPrepareSharedState> shared =
            prepareState;
        uint64_t generation = 0;
        {
            std::scoped_lock lock(shared->mutex);
            if (!shared->accepting) {
                pendingVoices.erase(handle.value);
                if (error) *error = "audio prepare service is shutting down";
                return false;
            }
            generation = shared->generation;
            ++shared->activeJobs;
        }
        PrepareFlight flight;
        flight.generation = generation;
        flight.state = ClipPrepareState::Preparing;
        flight.waitingHandles.push_back(handle.value);
        prepareFlights.emplace(prepareKey, std::move(flight));

        try {
            engine::resource::ResourceSchedulerRuntime* scheduler =
                engine::resource::activeResourceSchedulerRuntime();
            if (!scheduler) throw std::runtime_error(
                "resource scheduler is unavailable");
            engine::resource::ResourceRequest resourceRequest;
            resourceRequest.key.kind = engine::resource::ResourceKind::Audio;
            resourceRequest.key.canonicalIdentity = assetKey;
            resourceRequest.key.variant = static_cast<uint64_t>(profile);
            resourceRequest.demand = engine::resource::ResourceDemand::Visible;
            resourceRequest.estimatedBytes = 256u * 1024u;
            const engine::resource::ResourceSubmitResult submitted =
                scheduler->submit(
                std::move(resourceRequest),
                [shared, generation, assetKey, prepareKey, profile](
                    const engine::resource::ResourceTaskContext& context) mutable {
                    ClipPrepareCompletion completion;
                    completion.generation = generation;
                    completion.assetKey = assetKey;
                    completion.prepareKey = prepareKey;
                    if (!context.stopRequested()) {
                        completion.clip = prepareAudioClip(
                            assetKey, profile, &completion.error);
                    } else {
                        completion.error = "audio preparation cancelled";
                    }
                    completion.state = completion.clip
                        ? ClipPrepareState::Ready : ClipPrepareState::Failed;
                    const bool ready =
                        completion.state == ClipPrepareState::Ready;
                    {
                        std::scoped_lock lock(shared->mutex);
                        if (shared->accepting &&
                            generation == shared->generation) {
                            shared->completions.push_back(
                                std::move(completion));
                        }
                    }
                    return ready
                        ? engine::resource::ResourceTaskResult::Ready
                        : engine::resource::ResourceTaskResult::Failed;
                },
                [shared](const engine::resource::ResourceCompletion&) {
                    std::scoped_lock lock(shared->mutex);
                    --shared->activeJobs;
                    shared->idle.notify_all();
                });
            if (!submitted.accepted()) {
                throw std::runtime_error(
                    "resource scheduler rejected audio prepare task");
            }
        } catch (...) {
            {
                std::scoped_lock lock(shared->mutex);
                --shared->activeJobs;
            }
            shared->idle.notify_all();
            prepareFlights.erase(prepareKey);
            pendingVoices.erase(handle.value);
            if (error) *error = "could not schedule audio prepare task";
            return false;
        }
        return true;
    }

    void drainPreparedClips() {
        container::Deque<ClipPrepareCompletion> completions;
        uint64_t generation = 0;
        {
            std::scoped_lock lock(prepareState->mutex);
            generation = prepareState->generation;
            completions.swap(prepareState->completions);
        }
        while (!completions.empty()) {
            ClipPrepareCompletion completion =
                std::move(completions.front());
            completions.pop_front();
            if (completion.generation != generation) continue;
            const auto flight = prepareFlights.find(completion.prepareKey);
            if (flight == prepareFlights.end() ||
                flight->second.generation != completion.generation) {
                continue;
            }
            container::Vector<uint64_t> waiting =
                std::move(flight->second.waitingHandles);
            prepareFlights.erase(flight);

            container::SharedPtr<const PreparedAudioClip> installed;
            if (completion.state == ClipPrepareState::Ready &&
                completion.clip) {
                installed = installPreparedClip(
                    completion.assetKey, std::move(completion.clip));
            }
            for (const uint64_t value : waiting) {
                const auto pending = pendingVoices.find(value);
                if (pending == pendingVoices.end()) continue;
                PendingVoice voice = std::move(pending->second);
                pendingVoices.erase(pending);
                const AudioVoiceHandle handle{value};
                if (!installed && !completion.stream) {
                    ++failedPlayRequests;
                    setVoiceState(handle, AudioVoiceState::Failed);
                    TD_LOG_WARN("[Audio] Could not prepare voice {}: {}",
                                value, completion.error);
                    continue;
                }
                voice.request.position = voice.transform.position;
                container::String startError;
                const bool started = completion.stream
                    ? startStreamingVoice(handle, std::move(voice.request),
                                          completion.stream, &startError)
                    : startVoice(handle, std::move(voice.request), installed,
                                 &startError);
                if (!started) {
                    ++failedPlayRequests;
                    setVoiceState(handle, AudioVoiceState::Failed);
                    // A streaming completion can carry the worker-side
                    // failure (VFS resolution, decoder initialization,
                    // buffer reservation) while startStreamingVoice only
                    // sees the terminal empty ring.  Preserve both facts:
                    // "not ready" alone makes an authored voice failure
                    // impossible to distinguish from a backend race.
                    const container::StringView workerError =
                        completion.error.empty()
                        ? container::StringView{}
                        : container::StringView{completion.error};
                    if (workerError.empty()) {
                        TD_LOG_WARN(
                            "[Audio] Could not play voice {} asset='{}': {}",
                            value, completion.assetKey, startError);
                    } else {
                        TD_LOG_WARN(
                            "[Audio] Could not play voice {} asset='{}': {} (worker: {})",
                            value, completion.assetKey, startError,
                            workerError);
                    }
                }
            }
        }
    }

    void cancelPendingPreparations(bool shutdown) noexcept {
        container::Vector<container::SharedPtr<StreamingAudioSource>> streams;
        streams.reserve(prepareFlights.size());
        for (const auto& [_, flight] : prepareFlights) {
            if (flight.stream) {
                flight.stream->requestCancel();
                streams.push_back(flight.stream);
            }
        }
        for (const auto& [handle, pending] : pendingVoices) {
            static_cast<void>(pending);
            setVoiceState(AudioVoiceHandle{handle}, AudioVoiceState::Stopped);
        }
        pendingVoices.clear();
        prepareFlights.clear();
        std::unique_lock lock(prepareState->mutex);
        ++prepareState->generation;
        if (prepareState->generation == 0) ++prepareState->generation;
        prepareState->completions.clear();
        if (shutdown) prepareState->accepting = false;
        if (shutdown) {
            prepareState->idle.wait(lock, [this] {
                return prepareState->activeJobs == 0;
            });
        }
        lock.unlock();
        for (const auto& stream : streams) stream->waitForWorker();
    }

    void trimClipCache() noexcept {
        // Cache residency is independent from playback residency: erasing a
        // cache entry only releases the retained VFS bytes when no Voice is
        // still decoding the shared Clip. This preserves the configured cache
        // budget without cutting off an audible sound midway through playback.
        while (cachedClipBytes > config.clipCacheBudgetBytes && !clips.empty()) {
            const auto evict = std::min_element(clips.begin(), clips.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.lastUse < rhs.second.lastUse;
                });
            cachedClipBytes -= evict->second.bytes;
            clips.erase(evict);
        }
    }

    [[nodiscard]] size_t configuredPoolLimit(AudioVoicePool pool) const noexcept {
        switch (pool) {
        case AudioVoicePool::TwoDimensional: return config.maxTwoDimensionalVoices;
        case AudioVoicePool::ThreeDimensional: return config.maxThreeDimensionalVoices;
        case AudioVoicePool::Streaming: return config.maxStreamingVoices;
        }
        return 0;
    }

    [[nodiscard]] bool preemptFor(const AudioPlayRequest& request) noexcept {
        const AudioVoicePool requestedPool = voicePoolFor(request);
        const size_t poolLimit = configuredPoolLimit(requestedPool);
        const size_t activeInPool = static_cast<size_t>(std::count_if(
            voices.begin(), voices.end(), [requestedPool](const auto& item) {
                return voicePoolFor(item.second->request) == requestedPool;
            }));
        const bool poolIsFull = poolLimit != 0 && activeInPool >= poolLimit;
        if (!poolIsFull && voices.size() < config.maxActiveVoices) return true;

        // Original Miles keeps 2D samples, 3D samples and streams in separate
        // lists. Once AudioSettings has supplied a reservation, never let an
        // incoming 2D/UI sound cannibalize a 3D world voice (or vice versa).
        const bool restrictToRequestedPool = poolLimit != 0;
        auto lowest = voices.end();
        for (auto it = voices.begin(); it != voices.end(); ++it) {
            if (restrictToRequestedPool && voicePoolFor(it->second->request) != requestedPool) {
                continue;
            }
            if (lowest == voices.end()) {
                lowest = it;
                continue;
            }
            const auto candidatePriority = static_cast<uint8_t>(it->second->request.priority);
            const auto lowestPriority = static_cast<uint8_t>(lowest->second->request.priority);
            if (candidatePriority < lowestPriority ||
                (candidatePriority == lowestPriority &&
                 it->second->handle.value < lowest->second->handle.value)) {
                lowest = it;
            }
        }
        // RefCode's MilesAudioManager only cannibalizes a *strictly* lower
        // priority sample. Equal-priority ambience must be allowed to finish
        // instead of being churned by arrival order.
        if (lowest == voices.end() ||
            static_cast<uint8_t>(lowest->second->request.priority) >=
                static_cast<uint8_t>(request.priority)) {
            return false;
        }
        destroyVoice(lowest, AudioVoiceState::Preempted);
        ++preemptedVoices;
        return true;
    }

    [[nodiscard]] bool startStreamingVoice(
        AudioVoiceHandle handle, AudioPlayRequest request,
        container::SharedPtr<StreamingAudioSource> stream,
        container::String* error) {
        const bool hasBufferedFrames = stream && stream->ringInitialized &&
            ma_pcm_rb_available_read(&stream->ring) != 0;
        if (!engineAvailable || !stream || !stream->ringInitialized ||
            (stream->failed.load(std::memory_order_acquire) &&
             !hasBufferedFrames)) {
            if (error) *error = "streaming audio source is not ready";
            if (stream) {
                stream->requestCancel();
                stream->waitForWorker();
            }
            return false;
        }
        // The decoder can discover a malformed trailing block after it has
        // already published a valid start watermark.  RefCode/Miles plays
        // the decodable prefix; rejecting the voice here loses the whole
        // spoken line and reports an asynchronous ready/fail race as a start
        // failure.  Admit the sealed buffered prefix and let the normal
        // stream retirement path report Failed after those frames drain.
        if (!preemptFor(request)) {
            if (error) *error = "streaming voice pool has no strictly lower-priority slot";
            stream->requestCancel();
            stream->waitForWorker();
            return false;
        }

        auto voice = std::make_unique<Voice>();
        voice->handle = handle;
        voice->request = std::move(request);
        voice->transform.position = voice->request.position;
        voice->stream = std::move(stream);
        ma_uint32 flags = 0;
        if (voice->request.spatialMode == AudioSpatialMode::TwoDimensional) {
            flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        }
        const size_t busIndex = audioBusIndex(voice->request.bus);
        ma_sound_group* group = busGroupInitialized[busIndex]
            ? &busGroups[busIndex] : nullptr;
        ma_result result = ma_sound_init_from_data_source(
            &engine, reinterpret_cast<ma_data_source*>(&voice->stream->ring),
            flags, group, &voice->sound);
        if (result != MA_SUCCESS) {
            if (error) *error = "stream sound init failed: " +
                container::String(ma_result_description(result));
            voice->stream->requestCancel();
            voice->stream->waitForWorker();
            return false;
        }
        voice->soundInitialized = true;
        if (voice->request.spatialMode == AudioSpatialMode::ThreeDimensional) {
            const math::vec3 position =
                AudioListenerBuilder::toAudioSpace(voice->transform.position);
            ma_sound_set_positioning(&voice->sound, ma_positioning_absolute);
            ma_sound_set_position(&voice->sound, position.x(), position.y(), position.z());
            applySpatializationPolicy(*voice);
        }
        applyVoiceVolume(*voice);
        ma_sound_set_pitch(&voice->sound, voice->request.pitch);
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            if (error) *error = "stream sound start failed: " +
                container::String(ma_result_description(result));
            ma_sound_uninit(&voice->sound);
            voice->soundInitialized = false;
            voice->stream->requestCancel();
            voice->stream->waitForWorker();
            return false;
        }
        if (!group && buses[busIndex].paused) {
            static_cast<void>(ma_sound_stop(&voice->sound));
        }
        voices.emplace(handle.value, std::move(voice));
        setVoiceState(handle, AudioVoiceState::Playing);
        return true;
    }

    [[nodiscard]] bool startVoice(
        AudioVoiceHandle handle, AudioPlayRequest request,
        container::SharedPtr<const PreparedAudioClip> clip,
        container::String* error) {
        if (!engineAvailable) {
            if (error) *error = "no playback device is available";
            return false;
        }
        if (!clip) return false;
        // A malformed/missing asset must never steal an audible lower-priority
        // voice. Decode/cache first, then perform the bounded-pool preemption.
        if (!preemptFor(request)) {
            if (error) *error = "voice pool has no strictly lower-priority slot";
            return false;
        }

        auto voice = std::make_unique<Voice>();
        voice->handle = handle;
        voice->request = std::move(request);
        voice->transform.position = voice->request.position;
        voice->clip = clip;
        // Generals' positional samples are semantically mono. Some shipped
        // WAVs are stereo (including IMA-ADPCM); forcing this decoder output
        // to one float channel gives a real point source instead of feeding a
        // stereo bed to the spatializer. 2D/music-like requests preserve the
        // source channel layout.
        ma_decoder_config decoderConfig = ma_decoder_config_init(
            voice->request.spatialMode == AudioSpatialMode::ThreeDimensional
                ? ma_format_f32 : ma_format_unknown,
            voice->request.spatialMode == AudioSpatialMode::ThreeDimensional ? 1u : 0u,
            0);
        const ma_decoder_config* decoderConfigPtr =
            voice->request.spatialMode == AudioSpatialMode::ThreeDimensional
                ? &decoderConfig : nullptr;
        ma_result result = ma_decoder_init_memory(voice->clip->encodedBytes.data(),
                                                  voice->clip->encodedBytes.size(), decoderConfigPtr,
                                                  &voice->decoder);
        if (result != MA_SUCCESS) {
            if (error) *error = container::String{"decoder init failed: "} + ma_result_description(result);
            return false;
        }
        voice->decoderInitialized = true;

        ma_uint32 flags = voice->request.loop ? MA_SOUND_FLAG_LOOPING : 0;
        if (voice->request.spatialMode == AudioSpatialMode::TwoDimensional) {
            flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        }
        const size_t busIndex = audioBusIndex(voice->request.bus);
        ma_sound_group* group = busGroupInitialized[busIndex]
            ? &busGroups[busIndex] : nullptr;
        result = ma_sound_init_from_data_source(
            &engine, reinterpret_cast<ma_data_source*>(&voice->decoder), flags,
            group, &voice->sound);
        if (result != MA_SUCCESS) {
            if (error) *error = container::String{"sound init failed: "} + ma_result_description(result);
            ma_decoder_uninit(&voice->decoder);
            voice->decoderInitialized = false;
            return false;
        }
        voice->soundInitialized = true;

        if (voice->request.spatialMode == AudioSpatialMode::ThreeDimensional) {
            const math::vec3 position = AudioListenerBuilder::toAudioSpace(voice->transform.position);
            const math::vec3 velocity = AudioListenerBuilder::toAudioSpace(voice->transform.velocity);
            ma_sound_set_positioning(&voice->sound, ma_positioning_absolute);
            ma_sound_set_position(&voice->sound, position.x(), position.y(), position.z());
            ma_sound_set_velocity(&voice->sound, velocity.x(), velocity.y(), velocity.z());
            applySpatializationPolicy(*voice);
        }
        applyVoiceVolume(*voice);
        ma_sound_set_pitch(&voice->sound, voice->request.pitch);
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            if (error) *error = container::String{"sound start failed: "} + ma_result_description(result);
            ma_sound_uninit(&voice->sound);
            voice->soundInitialized = false;
            ma_decoder_uninit(&voice->decoder);
            voice->decoderInitialized = false;
            return false;
        }
        if (!group && buses[busIndex].paused) {
            static_cast<void>(ma_sound_stop(&voice->sound));
        }
        voices.emplace(handle.value, std::move(voice));
        setVoiceState(handle, AudioVoiceState::Playing);
        return true;
    }

    void updateVoiceTransform(Voice& voice, AudioVoiceTransform transform) noexcept {
        if (voice.request.spatialMode != AudioSpatialMode::ThreeDimensional ||
            !voice.soundInitialized) {
            return;
        }
        transform = sanitizeTransform(transform);
        voice.transform = transform;
        voice.request.position = transform.position;
        const math::vec3 position = AudioListenerBuilder::toAudioSpace(transform.position);
        const math::vec3 velocity = AudioListenerBuilder::toAudioSpace(transform.velocity);
        // Source transforms are atomically consumed by miniaudio's
        // spatializer, unlike its legacy gainer volume setter.
        ma_sound_set_position(&voice.sound, position.x(), position.y(), position.z());
        ma_sound_set_velocity(&voice.sound, velocity.x(), velocity.y(), velocity.z());
        applyVoiceVolume(voice);
    }

    void updateVoiceVolume(Voice& voice, float volume) noexcept {
        // The legacy event override is an absolute authored gain, not a bus
        // multiplier.  Retain the same wide 0..4 range accepted by normal
        // AudioPlayRequest sanitization, then re-apply the current bus state
        // through the atomic node-output path.
        voice.request.volume = gain(volume);
        voice.request.admissionVolume = voice.request.volume;
        applyVoiceVolume(voice);
    }

    void retireCompletedVoices() noexcept {
        // unordered_dense compacts storage on erase, invalidating every
        // iterator at or after the removed slot. First collect stable handle
        // values, then resolve and destroy them in a separate pass.
        container::Vector<uint64_t> completedHandles;
        completedHandles.reserve(voices.size());
        for (const auto& [handle, ownedVoice] : voices) {
            const Voice& voice = *ownedVoice;
            // `ma_sound_is_playing()` reports the node's started state, which
            // remains true after a decoder naturally reaches EOF. `at_end` is
            // the terminal data-source signal; without it one-shots occupied
            // the pool forever and eventually forced unrelated preemption.
            const bool streamFinished = voice.stream &&
                voice.stream->workerDone.load(std::memory_order_acquire) &&
                (voice.stream->eof.load(std::memory_order_acquire) ||
                 voice.stream->failed.load(std::memory_order_acquire)) &&
                ma_pcm_rb_available_read(&voice.stream->ring) == 0;
            if (streamFinished ||
                (!voice.stream && !voice.request.loop && voice.soundInitialized &&
                 ma_sound_at_end(&voice.sound))) {
                completedHandles.push_back(handle);
            }
        }
        for (const uint64_t handle : completedHandles) {
            const auto completed = voices.find(handle);
            if (completed == voices.end()) continue;
            const bool failed = completed->second->stream &&
                completed->second->stream->failed.load(std::memory_order_acquire);
            destroyVoice(completed, failed ? AudioVoiceState::Failed
                                           : AudioVoiceState::Completed);
            if (!failed) ++completedVoices;
        }
    }

    void retireInaudibleSpatialVoices() noexcept {
        container::Vector<uint64_t> inaudible;
        inaudible.reserve(voices.size());
        for (const auto& [handle, voice] : voices) {
            if (voice->request.spatialMode == AudioSpatialMode::ThreeDimensional &&
                belowSampleVolumeThreshold(voice->request,
                                           voice->transform.position)) {
                inaudible.push_back(handle);
            }
        }
        for (const uint64_t handle : inaudible) {
            const auto found = voices.find(handle);
            if (found != voices.end()) destroyVoice(found);
        }
    }
};

AudioSystem::AudioSystem()
    : m_impl(std::make_unique<Impl>()) {}

AudioSystem::~AudioSystem() {
    shutdown();
}

bool AudioSystem::init(const AudioSystemConfig& rawConfig) {
    if (!m_impl) return false;
    {
        std::scoped_lock lock(m_impl->commandEndpointMutex);
        if (m_impl->initialized.load(std::memory_order_acquire)) return false;
    }

    AudioSystemConfig config = rawConfig;
    config.maxActiveVoices = std::max<size_t>(config.maxActiveVoices, 1);
    config.maxPendingCommands = std::max<size_t>(config.maxPendingCommands, 1);
    config.streamBufferFrames = std::clamp<size_t>(
        config.streamBufferFrames, 1, std::numeric_limits<ma_uint32>::max());
    config.streamStartWatermarkFrames = std::clamp<size_t>(
        config.streamStartWatermarkFrames, 1, config.streamBufferFrames);
    config.minSampleVolume = unit(config.minSampleVolume, 0.02f);
    config.rangeVolumeFadeExponent = std::clamp(
        finite(config.rangeVolumeFadeExponent)
            ? config.rangeVolumeFadeExponent : 4.0f,
        0.01f, 16.0f);
    m_impl->config = config;
    m_impl->pendingVoices.clear();
    m_impl->prepareFlights.clear();
    m_impl->prepareState = std::make_shared<ClipPrepareSharedState>();
    m_impl->prepareState->streamBufferBudgetBytes =
        config.streamBufferBudgetBytes;
    {
        std::scoped_lock lock(m_impl->commandEndpointMutex);
        m_impl->commands = std::make_shared<AudioCommandQueue>(config.maxPendingCommands);
        m_impl->initialized.store(true, std::memory_order_release);
    }
    // Exact AudioSettings.ini defaults: Music 55%, 2D/3D effects 80%,
    // speech/streaming 70%. OptionPreferences/Audio.ini will override these
    // in the next data-driven phase rather than hardcoding Miles provider UI.
    m_impl->buses = {{
        {.enabled = true, .volume = 0.55f},
        {.enabled = true, .volume = 0.80f},
        {.enabled = true, .volume = 0.80f},
        {.enabled = true, .volume = 0.80f},
        {.enabled = true, .volume = 0.70f},
    }};
    m_impl->listener = sanitizeListener({});
    m_impl->engineAvailable = false;
    m_impl->playbackDeviceAvailable = false;
    m_impl->offlineMixerAvailable = false;
    if (!config.enablePlaybackDevice && !config.enableOfflineMix) return true;

    // Keep the device stopped while the graph and its category groups are
    // established. This both gives deterministic initial gain state and
    // avoids an audible callback observing a partially constructed graph.
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.noAutoStart = MA_TRUE;
    if (config.enableOfflineMix) {
        // noDevice still builds the complete node graph. Test code pulls it
        // manually with ma_engine_read_pcm_frames(), so no OS endpoint or
        // callback thread participates in EOS/sequence regressions.
        engineConfig.noDevice = MA_TRUE;
        engineConfig.channels = kOfflineMixChannelCount;
        engineConfig.sampleRate = kOfflineMixSampleRate;
    }
    const ma_result engineResult = ma_engine_init(&engineConfig, &m_impl->engine);
    if (engineResult != MA_SUCCESS) {
        TD_LOG_WARN("[Audio] {} unavailable: {}",
                    config.enableOfflineMix ? "Offline mixer" : "Playback device",
                    ma_result_description(engineResult));
        return true;
    }
    m_impl->engineAvailable = true;
    m_impl->offlineMixerAvailable = config.enableOfflineMix;
    m_impl->playbackDeviceAvailable = !config.enableOfflineMix;
    for (size_t index = 0; index < kAudioBusCount; ++index) {
        if (ma_sound_group_init(&m_impl->engine, 0, nullptr, &m_impl->busGroups[index]) != MA_SUCCESS) {
            TD_LOG_WARN("[Audio] Failed to initialize bus {}", index);
            continue;
        }
        m_impl->busGroupInitialized[index] = true;
        m_impl->applyBus(index);
        if (m_impl->buses[index].paused) {
            m_impl->applyBusPause(index);
        }
    }
    m_impl->applyListener();
    if (config.enableOfflineMix) return true;

    const ma_result startResult = ma_engine_start(&m_impl->engine);
    if (startResult != MA_SUCCESS) {
        TD_LOG_WARN("[Audio] Playback device could not start: {}", ma_result_description(startResult));
        for (size_t index = kAudioBusCount; index > 0; --index) {
            const size_t busIndex = index - 1;
            if (m_impl->busGroupInitialized[busIndex]) {
                ma_sound_group_uninit(&m_impl->busGroups[busIndex]);
                m_impl->busGroupInitialized[busIndex] = false;
            }
        }
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineAvailable = false;
        m_impl->playbackDeviceAvailable = false;
        m_impl->offlineMixerAvailable = false;
    }
    return true;
}

void AudioSystem::reset() noexcept {
    if (!m_impl) return;
    if (const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue()) {
        commands->clear();
    }
    m_impl->destroyAllVoices();
    m_impl->cancelPendingPreparations(false);
    m_impl->stopTrackedVoices();
}

void AudioSystem::shutdown() noexcept {
    if (!m_impl) return;
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->detachCommandQueueForShutdown();
    if (!commands) return;

    commands->close();

    // The engine owns the device callback. Stop it before tearing down the
    // graph's groups or decoders so no callback can observe a half-destroyed
    // category node during application shutdown.
    if (m_impl->playbackDeviceAvailable) {
        ma_engine_stop(&m_impl->engine);
    }
    m_impl->destroyAllVoices();
    m_impl->cancelPendingPreparations(true);
    m_impl->stopTrackedVoices();
    m_impl->clips.clear();
    m_impl->cachedClipBytes = 0;
    m_impl->nextClipUse = 1;
    if (m_impl->engineAvailable) {
        for (size_t index = kAudioBusCount; index > 0; --index) {
            const size_t busIndex = index - 1;
            if (m_impl->busGroupInitialized[busIndex]) {
                ma_sound_group_uninit(&m_impl->busGroups[busIndex]);
                m_impl->busGroupInitialized[busIndex] = false;
            }
        }
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineAvailable = false;
        m_impl->playbackDeviceAvailable = false;
        m_impl->offlineMixerAvailable = false;
    }
}

bool AudioSystem::isInitialized() const noexcept {
    return m_impl && m_impl->initialized.load(std::memory_order_acquire);
}

bool AudioSystem::isPlaybackDeviceAvailable() const noexcept {
    return m_impl && m_impl->playbackDeviceAvailable.load(std::memory_order_acquire);
}

bool AudioSystem::isOfflineMixerAvailable() const noexcept {
    return m_impl && m_impl->offlineMixerAvailable.load(std::memory_order_acquire);
}

bool AudioSystem::renderOfflineFrames(uint64_t frameCount) noexcept {
    if (!m_impl || !m_impl->offlineMixerAvailable.load(std::memory_order_acquire)) {
        return false;
    }
    if (frameCount == 0) return true;
    constexpr uint64_t kMaxFramesPerPull = 1u << 20u;
    frameCount = std::min(frameCount, kMaxFramesPerPull);
    const ma_uint32 channels = ma_engine_get_channels(&m_impl->engine);
    if (channels == 0 || frameCount > std::numeric_limits<size_t>::max() / channels) {
        return false;
    }
    container::Vector<float> scratch(static_cast<size_t>(frameCount) * channels);
    return ma_engine_read_pcm_frames(&m_impl->engine, scratch.data(), frameCount, nullptr) == MA_SUCCESS;
}

bool AudioSystem::preload(container::StringView assetPath, container::String* error) {
    if (!m_impl || !m_impl->initialized.load(std::memory_order_acquire)) {
        if (error) *error = "audio system is not initialized";
        return false;
    }
    return static_cast<bool>(m_impl->loadClip(assetPath, error));
}

AudioVoiceHandle AudioSystem::enqueuePlay(AudioPlayRequest request) {
    if (!m_impl) return {};
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return {};
    request = sanitizeRequest(std::move(request));
    if (request.assetPath.empty()) {
        ++m_impl->droppedCommands;
        return {};
    }

    uint64_t value = m_impl->nextVoiceValue.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = m_impl->nextVoiceValue.fetch_add(1, std::memory_order_relaxed);
    const AudioVoiceHandle handle{value};
    m_impl->setVoiceState(handle, AudioVoiceState::Pending);
    if (!commands->tryPush({.type = AudioCommandType::Play,
                            .handle = handle,
                            .request = std::move(request)})) {
        // The caller receives an invalid handle, but retain a terminal state
        // internally so a producer/shutdown race can never strand this newly
        // allocated voice as Pending forever.
        m_impl->setVoiceState(handle, AudioVoiceState::Stopped);
        ++m_impl->droppedCommands;
        return {};
    }
    ++m_impl->acceptedCommands;
    return handle;
}

bool AudioSystem::enqueueStop(AudioVoiceHandle handle) {
    if (!m_impl || !handle) return false;
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return false;
    if (!commands->tryPush({.type = AudioCommandType::Stop, .handle = handle})) {
        ++m_impl->droppedCommands;
        return false;
    }
    ++m_impl->acceptedCommands;
    return true;
}

bool AudioSystem::enqueueStopAll(std::optional<AudioBus> bus) {
    if (!m_impl || (bus && !validBus(*bus))) {
        return false;
    }
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return false;
    if (!commands->tryPush({.type = AudioCommandType::StopAll, .bus = bus})) {
        ++m_impl->droppedCommands;
        return false;
    }
    ++m_impl->acceptedCommands;
    return true;
}

bool AudioSystem::enqueueSetVolume(AudioVoiceHandle handle, float volume) {
    if (!m_impl || !handle || !finite(volume)) return false;
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return false;
    if (!commands->tryPush({.type = AudioCommandType::SetVolume,
                            .handle = handle,
                            .volume = gain(volume)})) {
        ++m_impl->droppedCommands;
        return false;
    }
    ++m_impl->acceptedCommands;
    return true;
}

bool AudioSystem::enqueueUpdateTransform(AudioVoiceHandle handle,
                                         AudioVoiceTransform transform) {
    if (!m_impl || !handle) return false;
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return false;
    if (!commands->tryPush({.type = AudioCommandType::UpdateTransform,
                            .handle = handle,
                            .transform = sanitizeTransform(transform)})) {
        ++m_impl->droppedCommands;
        return false;
    }
    ++m_impl->acceptedCommands;
    return true;
}

AudioVoiceState AudioSystem::voiceState(AudioVoiceHandle handle) const noexcept {
    return m_impl ? m_impl->queryVoiceState(handle) : AudioVoiceState::Unknown;
}

void AudioSystem::publishListener(AudioListenerSnapshot listener) noexcept {
    if (!m_impl || !m_impl->initialized.load(std::memory_order_acquire)) return;
    m_impl->listener = sanitizeListener(listener);
    m_impl->applyListener();
}

AudioListenerSnapshot AudioSystem::listener() const noexcept {
    return m_impl ? m_impl->listener : AudioListenerSnapshot{};
}

void AudioSystem::setBusVolume(AudioBus bus, float volume) noexcept {
    if (!m_impl || !validBus(bus)) return;
    const size_t index = audioBusIndex(bus);
    m_impl->buses[index].volume = unit(volume);
    m_impl->applyBus(index);
}

void AudioSystem::setBusEnabled(AudioBus bus, bool enabled) noexcept {
    if (!m_impl || !validBus(bus)) return;
    const size_t index = audioBusIndex(bus);
    m_impl->buses[index].enabled = enabled;
    m_impl->applyBus(index);
}

void AudioSystem::setBusPaused(AudioBus bus, bool paused) noexcept {
    if (!m_impl || !validBus(bus)) return;
    const size_t index = audioBusIndex(bus);
    if (m_impl->buses[index].paused == paused) return;
    m_impl->buses[index].paused = paused;
    m_impl->applyBusPause(index);
}

AudioBusState AudioSystem::busState(AudioBus bus) const noexcept {
    if (!m_impl || !validBus(bus)) return {};
    return m_impl->buses[audioBusIndex(bus)];
}

void AudioSystem::setRuntimeEventPolicy(AudioRuntimeEventPolicy policy) noexcept {
    if (!m_impl || !m_impl->initialized.load(std::memory_order_acquire)) return;
    m_impl->config.minSampleVolume = unit(policy.minSampleVolume, 0.02f);
    m_impl->config.use3DRangeVolumeFade = policy.use3DRangeVolumeFade;
    m_impl->config.rangeVolumeFadeExponent = std::clamp(
        finite(policy.rangeVolumeFadeExponent)
            ? policy.rangeVolumeFadeExponent : 4.0f,
        0.01f, 16.0f);
    for (auto& [_, voice] : m_impl->voices) {
        m_impl->applySpatializationPolicy(*voice);
        m_impl->applyVoiceVolume(*voice);
    }
}

void AudioSystem::update() {
    if (!m_impl || !m_impl->initialized.load(std::memory_order_acquire)) return;
    const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue();
    if (!commands) return;

    m_impl->drainPreparedClips();
    AudioCommand command;
    // Retire completed samples before allocating a pending request. Otherwise
    // an already-finished voice can wrongly cause a live lower-priority voice
    // to be preempted just because it has not yet been removed this frame.
    if (m_impl->engineAvailable) m_impl->retireCompletedVoices();
    while (commands->tryPop(command)) {
        ++m_impl->processedCommands;
        switch (command.type) {
        case AudioCommandType::Play: {
            // Headless/--nosound operation is an intentional presentation
            // mode. Consume events in order but do not make each one look
            // like a missing-asset failure or spam the diagnostic log.
            if (!m_impl->engineAvailable) {
                m_impl->setVoiceState(command.handle, AudioVoiceState::Suppressed);
                break;
            }
            if (m_impl->belowSampleVolumeThreshold(
                    command.request, command.request.position)) {
                m_impl->setVoiceState(command.handle, AudioVoiceState::Suppressed);
                break;
            }
            container::String error;
            if (!m_impl->queueVoicePreparation(
                    command.handle, std::move(command.request), &error)) {
                ++m_impl->failedPlayRequests;
                m_impl->setVoiceState(command.handle, AudioVoiceState::Failed);
                TD_LOG_WARN("[Audio] Could not play voice {}: {}", command.handle.value, error);
            }
            break;
        }
        case AudioCommandType::Stop:
            if (const auto found = m_impl->voices.find(command.handle.value);
                found != m_impl->voices.end()) {
                m_impl->destroyVoice(found);
            } else if (m_impl->stopPendingVoice(command.handle.value) ||
                       m_impl->queryVoiceState(command.handle) == AudioVoiceState::Pending) {
                m_impl->setVoiceState(command.handle, AudioVoiceState::Stopped);
            }
            break;
        case AudioCommandType::StopAll:
            {
                container::Vector<uint64_t> stoppedHandles;
                stoppedHandles.reserve(m_impl->voices.size());
                for (const auto& [handle, voice] : m_impl->voices) {
                    if (!command.bus || voice->request.bus == *command.bus) {
                        stoppedHandles.push_back(handle);
                    }
                }
                for (const uint64_t handle : stoppedHandles) {
                    const auto remove = m_impl->voices.find(handle);
                    if (remove != m_impl->voices.end()) {
                        m_impl->destroyVoice(remove);
                    }
                }
                container::Vector<uint64_t> stoppedPending;
                for (const auto& [handle, pending] : m_impl->pendingVoices) {
                    if (!command.bus || pending.request.bus == *command.bus) {
                        stoppedPending.push_back(handle);
                    }
                }
                for (const uint64_t handle : stoppedPending) {
                    m_impl->stopPendingVoice(handle);
                }
            }
            break;
        case AudioCommandType::SetVolume:
            if (const auto found = m_impl->voices.find(command.handle.value);
                found != m_impl->voices.end()) {
                m_impl->updateVoiceVolume(*found->second, command.volume);
            } else if (const auto pending =
                           m_impl->pendingVoices.find(command.handle.value);
                       pending != m_impl->pendingVoices.end()) {
                pending->second.request.volume = command.volume;
                pending->second.request.admissionVolume = command.volume;
            }
            break;
        case AudioCommandType::UpdateTransform:
            if (const auto found = m_impl->voices.find(command.handle.value);
                found != m_impl->voices.end()) {
                m_impl->updateVoiceTransform(*found->second, command.transform);
            } else if (const auto pending =
                           m_impl->pendingVoices.find(command.handle.value);
                       pending != m_impl->pendingVoices.end()) {
                pending->second.transform = command.transform;
                pending->second.request.position = command.transform.position;
            }
            break;
        }
    }
    m_impl->drainPreparedClips();
    if (!m_impl->engineAvailable) return;
    m_impl->applyListener();
    m_impl->retireInaudibleSpatialVoices();
    m_impl->retireCompletedVoices();
}

AudioSystemStats AudioSystem::stats() const noexcept {
    AudioSystemStats result;
    if (!m_impl) return result;
    result.acceptedCommands = m_impl->acceptedCommands.load(std::memory_order_relaxed);
    result.droppedCommands = m_impl->droppedCommands.load(std::memory_order_relaxed);
    result.processedCommands = m_impl->processedCommands.load(std::memory_order_relaxed);
    result.failedPlayRequests = m_impl->failedPlayRequests.load(std::memory_order_relaxed);
    result.completedVoices = m_impl->completedVoices.load(std::memory_order_relaxed);
    result.preemptedVoices = m_impl->preemptedVoices.load(std::memory_order_relaxed);
    if (const container::SharedPtr<AudioCommandQueue> commands = m_impl->acquireCommandQueue()) {
        result.pendingCommands = commands->size();
    }
    result.activeVoices = m_impl->voices.size();
    result.cachedClips = m_impl->clips.size();
    result.cachedClipBytes = m_impl->cachedClipBytes;
    result.playbackDeviceAvailable = m_impl->playbackDeviceAvailable;
    result.offlineMixerAvailable = m_impl->offlineMixerAvailable;
    return result;
}

} // namespace engine::audio
