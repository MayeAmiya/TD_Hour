#pragma once

#include "core/container/container_types.h"

#include "presentation/render/RenderSceneSnapshot.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

namespace engine::render {

// Bounded single-producer/single-consumer hand-off for complete logic-frame
// snapshots. It is deliberately not a GPU command queue: the consumer is the
// only code allowed to turn this data into D3D12 command lists.
//
// Bounded SPSC history for replaceable world state. The render consumer takes
// the newest complete endpoint and retires older unread samples; ordered
// one-shot events must use their own journal. Retaining a short physical ring
// absorbs producer/consumer overlap without turning stale world states into a
// seconds-long playback queue.
template <size_t Capacity = 8>
class WorldRenderFrameQueue final {
    static_assert(Capacity >= 3);

public:
    // The source remains intact on saturation so the producer can retain it
    // and apply back-pressure instead of losing an interpolation endpoint.
    [[nodiscard]] bool tryPublish(WorldRenderSnapshot& snapshot) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        if (tail - head >= Capacity) return false;
        m_slots[tail % Capacity] = std::move(snapshot);
        m_tail.store(tail + 1u, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool hasCapacity() const noexcept {
        const size_t tail = m_tail.load(std::memory_order_acquire);
        const size_t head = m_head.load(std::memory_order_acquire);
        return tail - head < Capacity;
    }

    [[nodiscard]] bool tryConsumeOldest(WorldRenderSnapshot& output) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail) return false;
        output = std::move(m_slots[head % Capacity]);
        m_slots[head % Capacity] = {};
        m_head.store(head + 1u, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryConsumeNewest(WorldRenderSnapshot& output) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail) return false;
        const size_t newest = tail - 1u;
        output = std::move(m_slots[newest % Capacity]);
        retainSkippedProjectileStreamSamples(output, head, newest);
        for (size_t ordinal = head; ordinal < tail; ++ordinal) {
            m_slots[ordinal % Capacity] = {};
        }
        m_head.store(tail, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        return tail - head;
    }

    // Call only while producer and consumer are stopped (e.g. renderer
    // subsystem shutdown/restart).
    void reset() {
        for (WorldRenderSnapshot& slot : m_slots) slot = {};
        m_head.store(0u, std::memory_order_relaxed);
        m_tail.store(0u, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static bool sameProjectileStream(
        const ProjectileRenderSnapshot& left,
        const ProjectileRenderSnapshot& right) noexcept {
        return left.trailEnabled && right.trailEnabled &&
            left.launcherId == right.launcherId &&
            left.trailStreamInstance == right.trailStreamInstance &&
            left.launchSlot == right.launchSlot &&
            left.trailOwnerGeneration == right.trailOwnerGeneration &&
            left.trailChainIdentity == right.trailChainIdentity &&
            left.trailStreamName == right.trailStreamName;
    }

    void retainSkippedProjectileStreamSamples(
        WorldRenderSnapshot& newestSnapshot,
        size_t head, size_t newest) {
        if (head >= newest || newestSnapshot.projectiles.empty()) return;

        container::Vector<ProjectileRenderSnapshot> merged(
            newestSnapshot.projectiles.begin(),
            newestSnapshot.projectiles.end());
        bool appended = false;
        // Walk newest-to-oldest.  A skipped point is retained only while a
        // projectile from the exact same legacy ProjectileStream survives in
        // the newest endpoint (or in a newer skipped endpoint already kept).
        // This preserves the short flame ribbon without replaying stale world
        // states or resurrecting an ended stream.
        for (size_t ordinal = newest; ordinal-- > head;) {
            const WorldRenderSnapshot& skipped = m_slots[ordinal % Capacity];
            for (const ProjectileRenderSnapshot& candidate :
                 skipped.projectiles) {
                if (!candidate.trailEnabled || candidate.objectId == 0u) {
                    continue;
                }
                const bool streamContinues = std::any_of(
                    merged.begin(), merged.end(),
                    [&candidate](const ProjectileRenderSnapshot& current) {
                        return sameProjectileStream(candidate, current);
                    });
                if (!streamContinues) continue;
                const bool alreadyPresent = std::any_of(
                    merged.begin(), merged.end(),
                    [&candidate](const ProjectileRenderSnapshot& current) {
                        return current.objectId == candidate.objectId;
                    });
                if (alreadyPresent) continue;

                ProjectileRenderSnapshot transient = candidate;
                // Only the ProjectileStream point is durable across skipped
                // endpoints.  The projectile model lives in the replaceable
                // entity column and its old terrain shadow must not survive.
                transient.shadow = {};
                transient.boundingRadius = 0.0f;
                merged.push_back(std::move(transient));
                appended = true;
            }
        }
        if (!appended) return;
        std::sort(
            merged.begin(), merged.end(),
            [](const ProjectileRenderSnapshot& left,
               const ProjectileRenderSnapshot& right) {
                return left.objectId < right.objectId;
            });
        newestSnapshot.projectiles = std::move(merged);
        newestSnapshot.projectiles.seal();
    }

    container::Array<WorldRenderSnapshot, Capacity> m_slots;
    alignas(64) std::atomic<size_t> m_head{0u};
    alignas(64) std::atomic<size_t> m_tail{0u};
};

} // namespace engine::render
