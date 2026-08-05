#include "engine/renderer/world/resource/WorldTextureDecodeService.h"
#include "engine/renderer/world/resource/WorldTextureDecodeWork.h"

#include "core/platform/runtime_threads.h"
#include "engine/resource/ResourceSchedulerRuntime.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>

namespace engine::render {
namespace {

[[nodiscard]] engine::resource::ResourceDemand resourceDemand(
    RenderAssetPriority priority) noexcept {
    switch (sanitizeRenderAssetPriority(priority)) {
    case RenderAssetPriority::Background:
        return engine::resource::ResourceDemand::Optional;
    case RenderAssetPriority::Preload:
        return engine::resource::ResourceDemand::Prefetch;
    case RenderAssetPriority::Normal:
    case RenderAssetPriority::Visible:
        return engine::resource::ResourceDemand::Visible;
    case RenderAssetPriority::Count:
        break;
    }
    return engine::resource::ResourceDemand::Visible;
}

[[nodiscard]] bool isTerminalResourceState(
    engine::resource::ResourceJobState state) noexcept {
    using engine::resource::ResourceJobState;
    switch (state) {
    case ResourceJobState::Ready:
    case ResourceJobState::Failed:
    case ResourceJobState::Cancelled:
    case ResourceJobState::Stale:
        return true;
    case ResourceJobState::Invalid:
    case ResourceJobState::Queued:
    case ResourceJobState::InFlight:
        return false;
    }
    return false;
}

[[nodiscard]] container::String submitStatusDiagnostic(
    engine::resource::ResourceSubmitStatus status) {
    using engine::resource::ResourceSubmitStatus;
    switch (status) {
    case ResourceSubmitStatus::QueueFull:
        return "resource scheduler texture queue is full; retry pending";
    case ResourceSubmitStatus::ShuttingDown:
        return "resource scheduler is shutting down; retry pending";
    case ResourceSubmitStatus::InvalidRequest:
        return "resource scheduler rejected an invalid texture request";
    case ResourceSubmitStatus::EstimatedBytesTooLarge:
        return "texture CPU job exceeds the resource scheduler byte budget";
    case ResourceSubmitStatus::StaleGeneration:
        return "resource scheduler rejected a stale texture generation";
    case ResourceSubmitStatus::Accepted:
        break;
    }
    return "resource scheduler rejected the texture CPU job";
}

[[nodiscard]] container::String completionStateDiagnostic(
    engine::resource::ResourceJobState state) {
    using engine::resource::ResourceJobState;
    switch (state) {
    case ResourceJobState::Cancelled:
        return "texture CPU job was cancelled";
    case ResourceJobState::Stale:
        return "texture CPU job completed for a stale generation";
    case ResourceJobState::Failed:
        return "resource scheduler texture task failed";
    case ResourceJobState::Invalid:
    case ResourceJobState::Queued:
    case ResourceJobState::InFlight:
    case ResourceJobState::Ready:
        break;
    }
    return "texture CPU job did not produce a result";
}

} // namespace

class WorldTextureDecodeService::Impl final {
public:
    using Diagnostics = WorldTextureDecodeService::Diagnostics;
    using State = WorldTextureDecodeService::State;
    using Phase = WorldTextureDecodeService::Phase;
    using Lookup = WorldTextureDecodeService::Lookup;
    using VariantKind = detail::WorldTextureVariantKind;
    using Request = detail::WorldTextureDecodeRequest;
    using Job = detail::WorldTextureDecodeJob;
    using Completion = detail::WorldTextureDecodeCompletion;

    ~Impl() {
        const container::SharedPtr<DecodeEndpoint> endpoint = m_endpoint;
        {
            std::scoped_lock lock(endpoint->mutex);
            endpoint->accepting = false;
            endpoint->completions.clear();
        }
        if (engine::resource::ResourceSchedulerRuntime* scheduler =
                engine::resource::activeResourceSchedulerRuntime()) {
            for (const ActiveJob& active : m_activeJobs) {
                static_cast<void>(scheduler->cancel(active.ticket));
            }
        }
    }

    [[nodiscard]] Lookup requestOrdinary(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        WorldTextureVariant variant,
        uint32_t reduction,
        RenderAssetPriority priority) {
        Request request;
        request.logicalName.assign(logicalName);
        request.sourceKey = std::move(sourceKey);
        request.variantKey = std::move(variantKey);
        request.kind = VariantKind::Ordinary;
        request.variant = variant;
        request.reduction = reduction;
        request.priority = sanitizeRenderAssetPriority(priority);
        return requestVariant(std::move(request));
    }

    [[nodiscard]] Lookup requestTerrainColor(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        uint32_t gridWidth,
        uint32_t reduction,
        RenderAssetPriority priority) {
        Request request;
        request.logicalName.assign(logicalName);
        request.sourceKey = std::move(sourceKey);
        request.variantKey = std::move(variantKey);
        request.kind = VariantKind::TerrainColor;
        request.gridWidth = gridWidth;
        request.reduction = reduction;
        request.priority = sanitizeRenderAssetPriority(priority);
        return requestVariant(std::move(request));
    }

    [[nodiscard]] Lookup requestTerrainAlphaEdge(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        RenderAssetPriority priority) {
        Request request;
        request.logicalName.assign(logicalName);
        request.sourceKey = std::move(sourceKey);
        request.variantKey = std::move(variantKey);
        request.kind = VariantKind::TerrainAlphaEdge;
        request.priority = sanitizeRenderAssetPriority(priority);
        return requestVariant(std::move(request));
    }

    void pumpCompletions() { pump(); }

    void discardPreparedVariant(container::StringView variantKey) {
        const auto found = m_readyVariants.find(container::String(variantKey));
        if (found == m_readyVariants.end()) return;
        if (found->second) {
            m_reclaimedPreparedBytes += found->second->byteSize;
        }
        m_readyVariants.erase(found);
    }

    size_t collectSourceGarbage(uint64_t maximumBytes, size_t maximumItems) {
        uint64_t retainedBytes = 0;
        for (const auto& [key, source] : m_readySources) {
            static_cast<void>(key);
            retainedBytes += source->pixels.size() + source->gpuPixels.size();
        }
        size_t reclaimed = 0;
        while (retainedBytes > maximumBytes && reclaimed < maximumItems) {
            auto candidate = m_readySources.end();
            uint64_t candidateUse = std::numeric_limits<uint64_t>::max();
            for (auto it = m_readySources.begin(); it != m_readySources.end(); ++it) {
                if (!it->second || it->second.use_count() != 1) continue;
                const auto use = m_sourceLastUse.find(it->first);
                const uint64_t lastUse = use != m_sourceLastUse.end()
                    ? use->second : 0u;
                if (candidate == m_readySources.end() ||
                    std::tie(lastUse, it->first) <
                        std::tie(candidateUse, candidate->first)) {
                    candidate = it;
                    candidateUse = lastUse;
                }
            }
            if (candidate == m_readySources.end()) break;
            const uint64_t bytes = candidate->second->pixels.size() +
                candidate->second->gpuPixels.size();
            m_reclaimedSourceBytes += bytes;
            ++m_reclaimedSources;
            retainedBytes = retainedBytes >= bytes
                ? retainedBytes - bytes : 0u;
            m_sourceLastUse.erase(candidate->first);
            m_readySources.erase(candidate);
            ++reclaimed;
        }
        return reclaimed;
    }

    void reset() {
        m_cancelledVariants += m_pendingVariants.size();
        m_cancelledReady += m_readyVariants.size();
        m_cancelRequestedActive += activeJobCount(true);
        m_cancelledReady += activeJobCount(false);
        ++m_generation;
        if (m_generation == 0u) ++m_generation;
        ++m_resets;
        cancelActiveJobs([](const ActiveJob&) { return true; });
        m_jobs.clear();
        m_pendingSources.clear();
        m_pendingVariants.clear();
        m_waitingBySource.clear();
        m_readySources.clear();
        m_sourceAliases.clear();
        m_sourceLastUse.clear();
        m_sourceFailures.clear();
        m_readyVariants.clear();
        m_variantFailures.clear();
        m_retryableDiagnostics.clear();
        m_variantPriorities.clear();
        // Active old-generation jobs retain their exclusive decoder leases.
        // Do not return those decoders to the new generation's pool.
        m_availableDecoders.clear();
        m_latestStats = {};
        m_cpuUseOrdinal = 0;
        m_latestStats.generation = m_generation;
        m_latestStats.resets = m_resets;
    }

    void invalidateVariants() {
        m_cancelledVariants += m_pendingVariants.size();
        m_cancelledReady += m_readyVariants.size();
        for (const ActiveJob& active : m_activeJobs) {
            if (active.kind != Job::Kind::BuildVariant ||
                active.policyGeneration != m_policyGeneration) {
                continue;
            }
            if (activeJobRunning(active)) ++m_cancelRequestedActive;
            else ++m_cancelledReady;
        }
        ++m_policyGeneration;
        if (m_policyGeneration == 0u) ++m_policyGeneration;
        cancelActiveJobs([this](const ActiveJob& active) {
            return active.kind == Job::Kind::BuildVariant &&
                active.policyGeneration != m_policyGeneration;
        });
        std::erase_if(m_jobs, [](const Job& job) {
            return job.kind == Job::Kind::BuildVariant;
        });
        m_pendingVariants.clear();
        m_waitingBySource.clear();
        m_readyVariants.clear();
        m_variantFailures.clear();
        m_retryableDiagnostics.clear();
        m_variantPriorities.clear();
    }

    [[nodiscard]] TextureManagerStats stats() const noexcept {
        TextureManagerStats result = m_latestStats;
        result.decodedSources = m_readySources.size();
        result.negativeLookups = m_sourceFailures.size();
        result.cpuBytes = 0;
        for (const auto& [key, source] : m_readySources) {
            static_cast<void>(key);
            result.cpuBytes += source->pixels.size();
            result.cpuBytes += source->gpuPixels.size();
        }
        result.generation = m_generation;
        result.resets = m_resets;
        return result;
    }

    [[nodiscard]] Diagnostics diagnostics() const noexcept {
        Diagnostics result{
            .queuedJobs = m_jobs.size(),
            .activeJobs = activeJobCount(true),
            .pendingSources = m_pendingSources.size(),
            .activeSourceJobs = activeJobCount(
                true, Job::Kind::DecodeSource),
            .pendingVariants = m_pendingVariants.size(),
            .activeVariantJobs = activeJobCount(
                true, Job::Kind::BuildVariant),
            .preparedVariants = m_readyVariants.size(),
            .failedVariants = m_variantFailures.size(),
            .staleCompletions = m_staleCompletions,
            .completedCpuJobs = m_completedCpuJobs,
            .preparedBytes = m_preparedBytes,
            .workerNanoseconds = m_workerNanoseconds,
            .cancelledVariants = m_cancelledVariants,
            .cancelledReady = m_cancelledReady,
            .cancelRequestedActive = m_cancelRequestedActive,
            .maximumQueueAge = m_maximumQueueAge,
        };
        for (const auto& [key, payload] : m_readyVariants) {
            static_cast<void>(key);
            if (payload) result.retainedPreparedBytes += payload->byteSize;
        }
        result.reclaimedPreparedBytes = m_reclaimedPreparedBytes;
        result.reclaimedSourceBytes = m_reclaimedSourceBytes;
        result.reclaimedSources = m_reclaimedSources;
        return result;
    }

    [[nodiscard]] std::optional<Phase> phase(
        container::StringView variantKey) const noexcept {
        const container::String key(variantKey);
        if (m_readyVariants.contains(key)) return Phase::Ready;
        if (m_variantFailures.contains(key)) return Phase::Failed;
        if (!m_pendingVariants.contains(key)) return std::nullopt;
        if (std::any_of(
                m_activeJobs.begin(), m_activeJobs.end(),
                [this, &key](const ActiveJob& active) {
                    return active.variantKey == key &&
                        active.policyGeneration == m_policyGeneration &&
                        activeJobRunning(active);
                })) {
            return Phase::Active;
        }
        return Phase::Queued;
    }

private:
    struct DecodeEndpoint final {
        std::mutex mutex;
        container::Deque<Completion> completions;
        bool accepting = true;
    };

    struct ScheduledWork final {
        std::mutex mutex;
        std::optional<Job> job;
        std::optional<Completion> completion;
        Job::Kind kind = Job::Kind::DecodeSource;
        Request request;
        uint64_t generation = 0;
    };

    struct ActiveJob final {
        Job::Kind kind = Job::Kind::DecodeSource;
        Request request;
        container::String variantKey;
        uint64_t policyGeneration = 0;
        uint64_t generation = 0;
        engine::resource::ResourceTicket ticket;
    };

    template <typename Predicate>
    void cancelActiveJobs(Predicate&& predicate) {
        engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime();
        if (!scheduler) return;
        for (const ActiveJob& active : m_activeJobs) {
            if (predicate(active)) {
                static_cast<void>(scheduler->cancel(active.ticket));
            }
        }
    }

    void accumulateTextureStats(const TextureManagerStats& delta) noexcept {
        m_latestStats.proceduralSources += delta.proceduralSources;
        m_latestStats.aliases += delta.aliases;
        m_latestStats.requests += delta.requests;
        m_latestStats.cacheHits += delta.cacheHits;
        m_latestStats.cacheMisses += delta.cacheMisses;
        m_latestStats.decodeAttempts += delta.decodeAttempts;
        m_latestStats.decodeSucceeded += delta.decodeSucceeded;
        m_latestStats.decodeFailed += delta.decodeFailed;
        m_latestStats.missingSources += delta.missingSources;
        m_latestStats.unsupportedSources += delta.unsupportedSources;
        m_latestStats.generation = m_generation;
        m_latestStats.resets = m_resets;
    }

    [[nodiscard]] Lookup requestVariant(Request request) {
        pump();
        request.policyGeneration = m_policyGeneration;
        const container::String requestedVariantKey = request.variantKey;
        if (const auto alias = m_sourceAliases.find(request.sourceKey);
            alias != m_sourceAliases.end()) {
            request.sourceKey = alias->second;
        }
        if (const auto ready = m_readyVariants.find(request.variantKey);
            ready != m_readyVariants.end()) {
            return {
                .state = State::Ready,
                .payload = ready->second,
            };
        }
        if (const auto failed = m_variantFailures.find(request.variantKey);
            failed != m_variantFailures.end()) {
            return {
                .state = State::Failed,
                .diagnostic = failed->second,
            };
        }
        auto& requestedPriority = m_variantPriorities[request.variantKey];
        requestedPriority = std::max(requestedPriority, request.priority);
        if (!m_pendingVariants.insert(request.variantKey).second) {
            const auto retryable = m_retryableDiagnostics.find(
                request.variantKey);
            return {
                .diagnostic = retryable != m_retryableDiagnostics.end()
                    ? retryable->second : container::String{},
            };
        }
        m_retryableDiagnostics.erase(request.variantKey);
        request.enqueueSequence = m_nextEnqueueSequence++;

        if (const auto ready = m_readySources.find(request.sourceKey);
            ready != m_readySources.end()) {
            m_sourceLastUse[request.sourceKey] = ++m_cpuUseOrdinal;
            enqueueVariant(std::move(request), ready->second);
        } else if (const auto failed = m_sourceFailures.find(request.sourceKey);
                   failed != m_sourceFailures.end()) {
            m_pendingVariants.erase(request.variantKey);
            m_variantFailures.emplace(request.variantKey, failed->second);
            m_variantPriorities.erase(request.variantKey);
            return {.state = State::Failed, .diagnostic = failed->second};
        } else {
            m_waitingBySource[request.sourceKey].push_back(request);
            if (m_pendingSources.insert(request.sourceKey).second) {
                Job job;
                job.request = std::move(request);
                job.generation = m_generation;
                m_jobs.push_back(std::move(job));
            }
        }
        launchAvailable();
        const auto retryable = m_retryableDiagnostics.find(
            requestedVariantKey);
        return {
            .diagnostic = retryable != m_retryableDiagnostics.end()
                ? retryable->second : container::String{},
        };
    }

    void enqueueVariant(
        Request request, container::SharedPtr<const RawTexture> source) {
        Job job;
        job.kind = Job::Kind::BuildVariant;
        job.source = std::move(source);
        request.estimatedBytes = request.kind == VariantKind::Ordinary
            ? job.source->gpuPixels.size()
            : static_cast<uint64_t>(job.source->width) *
                job.source->height * 4u;
        job.request = std::move(request);
        job.generation = m_generation;
        m_jobs.push_back(std::move(job));
    }

    void publishCompletion(Completion completion) {
        ++m_completedCpuJobs;
        m_workerNanoseconds += completion.workerNanoseconds;
        m_maximumQueueAge = std::max(
            m_maximumQueueAge, completion.request.deferredPasses);
        if (completion.payload) {
            m_preparedBytes += completion.payload->byteSize;
        }
        if (completion.generation == m_generation) {
            if (completion.kind == Job::Kind::DecodeSource) {
                if (completion.decoder) {
                    m_availableDecoders.push_back(
                        std::move(completion.decoder));
                }
                m_pendingSources.erase(completion.request.sourceKey);
                m_retryableDiagnostics.erase(
                    completion.request.variantKey);
                accumulateTextureStats(completion.stats);
                auto waiting = m_waitingBySource.find(completion.request.sourceKey);
                if (completion.source) {
                    const container::String canonicalKey =
                        completion.canonicalSourceKey.empty()
                        ? completion.request.sourceKey
                        : completion.canonicalSourceKey;
                    m_sourceAliases[completion.request.sourceKey] = canonicalKey;
                    auto [sourceIt, inserted] = m_readySources.emplace(
                        canonicalKey, completion.source);
                    m_sourceLastUse[canonicalKey] = ++m_cpuUseOrdinal;
                    const container::SharedPtr<const RawTexture>& sharedSource =
                        sourceIt->second;
                    if (waiting != m_waitingBySource.end()) {
                        for (Request& request : waiting->second) {
                            m_retryableDiagnostics.erase(request.variantKey);
                            if (request.policyGeneration != m_policyGeneration) {
                                m_pendingVariants.erase(request.variantKey);
                            } else if (request.variantKey ==
                                           completion.request.variantKey &&
                                       completion.request.policyGeneration ==
                                           m_policyGeneration) {
                                m_pendingVariants.erase(request.variantKey);
                                if (completion.payload) {
                                    m_readyVariants[request.variantKey] =
                                        completion.payload;
                                } else {
                                    m_variantFailures[request.variantKey] =
                                        completion.diagnostic;
                                }
                                m_variantPriorities.erase(request.variantKey);
                            } else {
                                enqueueVariant(std::move(request),
                                               sharedSource);
                            }
                        }
                    }
                } else {
                    m_sourceFailures[completion.request.sourceKey] =
                        completion.diagnostic;
                    if (waiting != m_waitingBySource.end()) {
                        for (const Request& request : waiting->second) {
                            m_pendingVariants.erase(request.variantKey);
                            m_variantFailures[request.variantKey] =
                                completion.diagnostic;
                            m_variantPriorities.erase(request.variantKey);
                        }
                    }
                }
                if (waiting != m_waitingBySource.end()) {
                    m_waitingBySource.erase(waiting);
                }
            } else {
                m_retryableDiagnostics.erase(
                    completion.request.variantKey);
                if (completion.request.policyGeneration !=
                    m_policyGeneration) {
                    ++m_staleCompletions;
                } else if (completion.payload) {
                    m_pendingVariants.erase(completion.request.variantKey);
                    m_readyVariants[completion.request.variantKey] =
                        std::move(completion.payload);
                    m_variantPriorities.erase(
                        completion.request.variantKey);
                } else {
                    m_pendingVariants.erase(completion.request.variantKey);
                    m_variantFailures[completion.request.variantKey] =
                        std::move(completion.diagnostic);
                    m_variantPriorities.erase(
                        completion.request.variantKey);
                }
            }
        } else {
            ++m_staleCompletions;
        }
    }

    void pump() {
        container::Deque<Completion> completions;
        {
            std::scoped_lock lock(m_endpoint->mutex);
            completions.swap(m_endpoint->completions);
        }
        while (!completions.empty()) {
            Completion completion = std::move(completions.front());
            completions.pop_front();
            const auto active = std::find_if(
                m_activeJobs.begin(), m_activeJobs.end(),
                [&completion](const ActiveJob& candidate) {
                    return candidate.ticket.sequence() ==
                        completion.schedulerSequence;
                });
            if (active != m_activeJobs.end()) {
                m_activeJobs.erase(active);
            }
            publishCompletion(std::move(completion));
        }
        launchAvailable();
    }

    void setRetryableDiagnostic(
        const Job& job, container::StringView diagnostic) {
        if (job.kind == Job::Kind::DecodeSource) {
            const auto waiting = m_waitingBySource.find(
                job.request.sourceKey);
            if (waiting != m_waitingBySource.end()) {
                for (const Request& request : waiting->second) {
                    m_retryableDiagnostics[request.variantKey] =
                        container::String(diagnostic);
                }
            }
            return;
        }
        m_retryableDiagnostics[job.request.variantKey] =
            container::String(diagnostic);
    }

    void clearRetryableDiagnostic(const ActiveJob& job) {
        if (job.kind == Job::Kind::DecodeSource) {
            const auto waiting = m_waitingBySource.find(
                job.request.sourceKey);
            if (waiting != m_waitingBySource.end()) {
                for (const Request& request : waiting->second) {
                    m_retryableDiagnostics.erase(request.variantKey);
                }
            }
            return;
        }
        m_retryableDiagnostics.erase(job.variantKey);
    }

    static Completion rejectedCompletion(
        Job job, container::String diagnostic) {
        Completion completion;
        completion.kind = job.kind;
        completion.request = std::move(job.request);
        completion.source = std::move(job.source);
        completion.decoder = std::move(job.decoder);
        completion.generation = job.generation;
        completion.diagnostic = std::move(diagnostic);
        return completion;
    }

    void launchAvailable() {
        const size_t maximumInFlight = platform::runtime::resourceWorkerCount();
        while (m_activeJobs.size() < maximumInFlight && !m_jobs.empty()) {
            for (Job& candidate : m_jobs) {
                if (const auto requested = m_variantPriorities.find(candidate.request.variantKey);
                    requested != m_variantPriorities.end()) {
                    candidate.request.priority = std::max(candidate.request.priority, requested->second);
                }
            }
            const auto better = [](const Job& candidate, const Job& current) {
                const uint32_t candidatePriority =
                    effectiveRenderAssetPriority(candidate.request.priority, candidate.request.deferredPasses);
                const uint32_t currentPriority =
                    effectiveRenderAssetPriority(current.request.priority, current.request.deferredPasses);
                if (candidatePriority != currentPriority) {
                    return candidatePriority > currentPriority;
                }
                if (candidate.request.deferredPasses != current.request.deferredPasses) {
                    return candidate.request.deferredPasses > current.request.deferredPasses;
                }
                if (candidate.request.estimatedBytes != current.request.estimatedBytes) {
                    return candidate.request.estimatedBytes < current.request.estimatedBytes;
                }
                return candidate.request.enqueueSequence < current.request.enqueueSequence;
            };
            size_t selected = 0;
            for (size_t index = 1; index < m_jobs.size(); ++index) {
                if (better(m_jobs[index], m_jobs[selected])) selected = index;
            }
            Job job = std::move(m_jobs[selected]);
            m_jobs.erase(m_jobs.begin() + static_cast<std::ptrdiff_t>(selected));
            for (Job& waiting : m_jobs) {
                if (waiting.request.deferredPasses !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++waiting.request.deferredPasses;
                }
            }
            for (auto& [sourceKey, requests] : m_waitingBySource) {
                static_cast<void>(sourceKey);
                for (Request& waiting : requests) {
                    if (waiting.deferredPasses !=
                        std::numeric_limits<uint32_t>::max()) {
                        ++waiting.deferredPasses;
                    }
                }
            }
            if (job.kind == Job::Kind::DecodeSource) {
                if (m_availableDecoders.empty()) {
                    job.decoder = std::make_shared<TextureManager>();
                } else {
                    job.decoder = std::move(m_availableDecoders.back());
                    m_availableDecoders.pop_back();
                }
            }

            engine::resource::ResourceSchedulerRuntime* scheduler =
                engine::resource::activeResourceSchedulerRuntime();
            if (!scheduler) {
                if (job.decoder) {
                    m_availableDecoders.push_back(std::move(job.decoder));
                }
                if (job.request.deferredPasses !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++job.request.deferredPasses;
                }
                setRetryableDiagnostic(
                    job, "resource scheduler is unavailable; retry pending");
                m_jobs.push_front(std::move(job));
                break;
            }

            engine::resource::ResourceRequest resourceRequest;
            resourceRequest.key.kind =
                engine::resource::ResourceKind::Texture;
            resourceRequest.key.canonicalIdentity =
                job.kind == Job::Kind::DecodeSource
                ? job.request.sourceKey : job.request.variantKey;
            resourceRequest.key.variant = job.request.enqueueSequence;
            // Texture generations are cache-local. Keep the process-global
            // generation at zero and enforce stale publication through the
            // immutable job generation carried by the decode endpoint.
            resourceRequest.key.generation = 0;
            resourceRequest.demand = resourceDemand(job.request.priority);
            resourceRequest.lane = engine::resource::ResourceLane::Resource;
            resourceRequest.estimatedBytes = std::max<uint64_t>(
                job.request.estimatedBytes, 1u);

            const container::SharedPtr<ScheduledWork> work =
                std::make_shared<ScheduledWork>();
            {
                std::scoped_lock lock(work->mutex);
                work->kind = job.kind;
                work->request = job.request;
                work->generation = job.generation;
                work->job.emplace(std::move(job));
            }
            ActiveJob active;
            active.kind = work->kind;
            active.request = work->request;
            active.variantKey = work->request.variantKey;
            active.policyGeneration = work->request.policyGeneration;
            active.generation = work->generation;
            const container::SharedPtr<DecodeEndpoint> endpoint = m_endpoint;
            engine::resource::ResourceSubmitResult submitted;
            try {
                submitted = scheduler->submit(
                    std::move(resourceRequest),
                    [work](const engine::resource::ResourceTaskContext& context)
                        noexcept {
                        Job running;
                        {
                            std::scoped_lock lock(work->mutex);
                            if (!work->job) {
                                return engine::resource::ResourceTaskResult::Failed;
                            }
                            running = std::move(*work->job);
                            work->job.reset();
                        }
                        Completion completion;
                        if (context.stopRequested()) {
                            completion = rejectedCompletion(
                                std::move(running),
                                "texture CPU job was cancelled before execution");
                        } else {
                            completion = detail::WorldTextureDecodeWorker::run(
                                std::move(running));
                        }
                        {
                            std::scoped_lock lock(work->mutex);
                            work->completion.emplace(std::move(completion));
                        }
                        return context.stopRequested()
                            ? engine::resource::ResourceTaskResult::Failed
                            : engine::resource::ResourceTaskResult::Ready;
                    },
                    [endpoint, work](
                        const engine::resource::ResourceCompletion& terminal) {
                        Completion completion;
                        {
                            std::scoped_lock lock(work->mutex);
                            if (work->completion) {
                                completion = std::move(*work->completion);
                                work->completion.reset();
                            } else if (work->job) {
                                completion = rejectedCompletion(
                                    std::move(*work->job),
                                    completionStateDiagnostic(terminal.state));
                                work->job.reset();
                            } else {
                                completion.kind = work->kind;
                                completion.request = work->request;
                                completion.generation = work->generation;
                                completion.diagnostic =
                                    completionStateDiagnostic(terminal.state);
                            }
                        }
                        completion.schedulerSequence = terminal.sequence;
                        if (terminal.state !=
                            engine::resource::ResourceJobState::Ready) {
                            completion.source.reset();
                            completion.payload.reset();
                            completion.canonicalSourceKey.clear();
                            completion.stats = {};
                            completion.diagnostic =
                                completionStateDiagnostic(terminal.state);
                        }
                        std::scoped_lock lock(endpoint->mutex);
                        if (endpoint->accepting) {
                            endpoint->completions.push_back(
                                std::move(completion));
                        }
                    });
            } catch (...) {
                submitted.status =
                    engine::resource::ResourceSubmitStatus::QueueFull;
            }

            if (!submitted.accepted()) {
                Job rejected;
                {
                    std::scoped_lock lock(work->mutex);
                    if (work->job) {
                        rejected = std::move(*work->job);
                        work->job.reset();
                    }
                }
                const container::String diagnostic =
                    submitStatusDiagnostic(submitted.status);
                const bool retryable = submitted.status ==
                        engine::resource::ResourceSubmitStatus::QueueFull ||
                    submitted.status ==
                        engine::resource::ResourceSubmitStatus::ShuttingDown;
                if (retryable) {
                    if (rejected.decoder) {
                        m_availableDecoders.push_back(
                            std::move(rejected.decoder));
                    }
                    if (rejected.request.deferredPasses !=
                        std::numeric_limits<uint32_t>::max()) {
                        ++rejected.request.deferredPasses;
                    }
                    setRetryableDiagnostic(rejected, diagnostic);
                    m_jobs.push_front(std::move(rejected));
                    break;
                }
                publishCompletion(rejectedCompletion(
                    std::move(rejected), diagnostic));
                continue;
            }

            active.ticket = submitted.ticket;
            clearRetryableDiagnostic(active);
            m_activeJobs.push_back(std::move(active));
        }
    }

    [[nodiscard]] static bool activeJobRunning(
        const ActiveJob& active) noexcept {
        return active.ticket.valid() &&
            !isTerminalResourceState(active.ticket.state());
    }

    [[nodiscard]] size_t activeJobCount(bool running) const noexcept {
        return static_cast<size_t>(std::count_if(
            m_activeJobs.begin(), m_activeJobs.end(),
            [running](const ActiveJob& active) {
                return activeJobRunning(active) == running;
            }));
    }

    [[nodiscard]] size_t activeJobCount(
        bool running, Job::Kind kind) const noexcept {
        return static_cast<size_t>(std::count_if(
            m_activeJobs.begin(), m_activeJobs.end(),
            [running, kind](const ActiveJob& active) {
                return active.kind == kind &&
                    activeJobRunning(active) == running;
            }));
    }

    container::HashSet<container::String> m_pendingSources;
    container::HashSet<container::String> m_pendingVariants;
    container::HashMap<container::String,
                       container::Vector<Request>> m_waitingBySource;
    container::HashMap<container::String,
                       container::SharedPtr<const RawTexture>> m_readySources;
    container::HashMap<container::String, container::String> m_sourceAliases;
    container::HashMap<container::String, uint64_t> m_sourceLastUse;
    container::HashMap<container::String, container::String> m_sourceFailures;
    container::HashMap<container::String,
                       container::SharedPtr<const Lookup::Payload>> m_readyVariants;
    container::HashMap<container::String, container::String> m_variantFailures;
    container::HashMap<container::String, container::String>
        m_retryableDiagnostics;
    container::HashMap<container::String, RenderAssetPriority>
        m_variantPriorities;
    container::Deque<Job> m_jobs;
    container::Vector<ActiveJob> m_activeJobs;
    container::SharedPtr<DecodeEndpoint> m_endpoint =
        std::make_shared<DecodeEndpoint>();
    container::Vector<container::SharedPtr<TextureManager>>
        m_availableDecoders;
    TextureManagerStats m_latestStats;
    uint64_t m_generation = 1;
    uint64_t m_policyGeneration = 1;
    uint64_t m_resets = 0;
    uint64_t m_staleCompletions = 0;
    uint64_t m_nextEnqueueSequence = 1;
    uint64_t m_completedCpuJobs = 0;
    uint64_t m_preparedBytes = 0;
    uint64_t m_workerNanoseconds = 0;
    uint64_t m_cancelledVariants = 0;
    uint64_t m_cancelledReady = 0;
    uint64_t m_cancelRequestedActive = 0;
    uint32_t m_maximumQueueAge = 0;
    uint64_t m_cpuUseOrdinal = 0;
    uint64_t m_reclaimedPreparedBytes = 0;
    uint64_t m_reclaimedSourceBytes = 0;
    uint64_t m_reclaimedSources = 0;
};

WorldTextureDecodeService::WorldTextureDecodeService()
    : m_impl(std::make_unique<Impl>()) {}

WorldTextureDecodeService::~WorldTextureDecodeService() = default;

WorldTextureDecodeService::Lookup WorldTextureDecodeService::requestOrdinary(
    container::StringView logicalName,
    container::String sourceKey,
    container::String variantKey,
    WorldTextureVariant variant,
    uint32_t reduction,
    RenderAssetPriority priority) {
    return m_impl->requestOrdinary(
        logicalName, std::move(sourceKey), std::move(variantKey),
        variant, reduction, priority);
}

WorldTextureDecodeService::Lookup
WorldTextureDecodeService::requestTerrainColor(
    container::StringView logicalName,
    container::String sourceKey,
    container::String variantKey,
    uint32_t gridWidth,
    uint32_t reduction,
    RenderAssetPriority priority) {
    return m_impl->requestTerrainColor(
        logicalName, std::move(sourceKey), std::move(variantKey),
        gridWidth, reduction, priority);
}

WorldTextureDecodeService::Lookup
WorldTextureDecodeService::requestTerrainAlphaEdge(
    container::StringView logicalName,
    container::String sourceKey,
    container::String variantKey,
    RenderAssetPriority priority) {
    return m_impl->requestTerrainAlphaEdge(
        logicalName, std::move(sourceKey), std::move(variantKey), priority);
}

void WorldTextureDecodeService::pumpCompletions() {
    m_impl->pumpCompletions();
}

void WorldTextureDecodeService::discardPreparedVariant(
    container::StringView variantKey) {
    m_impl->discardPreparedVariant(variantKey);
}

size_t WorldTextureDecodeService::collectSourceGarbage(
    uint64_t maximumBytes, size_t maximumItems) {
    return m_impl->collectSourceGarbage(maximumBytes, maximumItems);
}

void WorldTextureDecodeService::reset() {
    m_impl->reset();
}

void WorldTextureDecodeService::invalidateVariants() {
    m_impl->invalidateVariants();
}

TextureManagerStats WorldTextureDecodeService::stats() const noexcept {
    return m_impl->stats();
}

WorldTextureDecodeService::Diagnostics
WorldTextureDecodeService::diagnostics() const noexcept {
    return m_impl->diagnostics();
}

std::optional<WorldTextureDecodeService::Phase>
WorldTextureDecodeService::phase(
    container::StringView variantKey) const noexcept {
    return m_impl->phase(variantKey);
}

} // namespace engine::render
