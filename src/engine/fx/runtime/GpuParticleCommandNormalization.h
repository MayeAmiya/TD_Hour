#pragma once

#include "core/container/container_types.h"
#include "GpuParticleContract.hlsli"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace engine::fx {

struct ParticleGpuCommandBatch final {
    uint32_t authorityEpoch = 0;
    container::Vector<gpu_particle::GpuParticleBirthCommand> births;
    container::Vector<gpu_particle::GpuParticleRetireCommand> retires;
};

struct ParticleGpuCommandNormalization final {
    ParticleGpuCommandBatch batch;
    uint64_t rejectedCommands = 0;
    uint64_t coalescedCommands = 0;
};

struct ParticleGpuCommandNormalizationSummary final {
    uint64_t rejectedCommands = 0;
    uint64_t coalescedCommands = 0;
};

namespace detail {

struct GpuParticleSlotLifecycle final {
    const gpu_particle::GpuParticleBirthCommand* latestBirth = nullptr;
    const gpu_particle::GpuParticleRetireCommand* latestRetire = nullptr;
    const gpu_particle::GpuParticleRetireCommand* latestMatchingRetire = nullptr;
};

[[nodiscard]] constexpr bool gpuCommandSequenceAfter(
    uint32_t candidate, uint32_t reference) noexcept {
    return candidate != reference &&
        static_cast<uint32_t>(candidate - reference) < 0x80000000u;
}

} // namespace detail

// Production callers retain one scratch object and one output batch. The
// value-returning convenience overload below remains useful for probes, while
// frame-to-frame renderer paths avoid allocating a slot table and two output
// vectors for every normalization pass.
struct ParticleGpuCommandNormalizationScratch final {
    struct Stats final {
        size_t slotCapacity = 0;
        size_t slotHighWater = 0;
        size_t birthOutputCapacity = 0;
        size_t birthOutputHighWater = 0;
        size_t retireOutputCapacity = 0;
        size_t retireOutputHighWater = 0;
        uint64_t capacityGrowths = 0;
        uint64_t normalizations = 0;
    };

    [[nodiscard]] const Stats& stats() const noexcept { return statistics; }

    container::Vector<detail::GpuParticleSlotLifecycle> slots;
    Stats statistics;
};

// Output must not alias either input span. Its vector capacities are retained
// by the caller; only sizes and values are replaced.
[[nodiscard]] inline ParticleGpuCommandNormalizationSummary
normalizeGpuParticleCommandsInto(
    ParticleGpuCommandBatch& output,
    ParticleGpuCommandNormalizationScratch& scratch,
    uint32_t authorityEpoch,
    container::Span<const gpu_particle::GpuParticleBirthCommand> births,
    container::Span<const gpu_particle::GpuParticleRetireCommand> retires,
    size_t slotCapacity) {
    ParticleGpuCommandNormalizationSummary summary;
    output.authorityEpoch = authorityEpoch;
    output.births.clear();
    output.retires.clear();
    ++scratch.statistics.normalizations;

    if (authorityEpoch == 0 || slotCapacity == 0) {
        summary.rejectedCommands = births.size() + retires.size();
        return summary;
    }

    const size_t previousSlotCapacity = scratch.slots.capacity();
    scratch.slots.resize(slotCapacity);
    if (scratch.slots.capacity() != previousSlotCapacity) {
        ++scratch.statistics.capacityGrowths;
    }
    scratch.statistics.slotCapacity = scratch.slots.capacity();
    scratch.statistics.slotHighWater = std::max(
        scratch.statistics.slotHighWater, slotCapacity);
    std::fill(scratch.slots.begin(), scratch.slots.end(),
              detail::GpuParticleSlotLifecycle{});

    uint64_t acceptedCommands = 0;
    for (const gpu_particle::GpuParticleBirthCommand& command : births) {
        const uint32_t generation = command.initialState.authorityTokens[1];
        if (command.authorityEpoch != authorityEpoch ||
            command.commandSequence == 0 ||
            command.destinationIndex >= slotCapacity || generation == 0 ||
            command.initialState.authorityTokens[0] !=
                command.destinationIndex) {
            ++summary.rejectedCommands;
            continue;
        }
        ++acceptedCommands;
        detail::GpuParticleSlotLifecycle& slot =
            scratch.slots[command.destinationIndex];
        if (!slot.latestBirth || detail::gpuCommandSequenceAfter(
                command.commandSequence,
                slot.latestBirth->commandSequence)) {
            slot.latestBirth = &command;
        }
    }
    for (const gpu_particle::GpuParticleRetireCommand& command : retires) {
        if (command.authorityEpoch != authorityEpoch ||
            command.commandSequence == 0 ||
            command.destinationIndex >= slotCapacity ||
            command.particleGeneration == 0) {
            ++summary.rejectedCommands;
            continue;
        }
        ++acceptedCommands;
        detail::GpuParticleSlotLifecycle& slot =
            scratch.slots[command.destinationIndex];
        if (!slot.latestRetire || detail::gpuCommandSequenceAfter(
                command.commandSequence,
                slot.latestRetire->commandSequence)) {
            slot.latestRetire = &command;
        }
    }
    for (const gpu_particle::GpuParticleRetireCommand& command : retires) {
        if (command.authorityEpoch != authorityEpoch ||
            command.commandSequence == 0 ||
            command.destinationIndex >= slotCapacity ||
            command.particleGeneration == 0) {
            continue;
        }
        detail::GpuParticleSlotLifecycle& slot =
            scratch.slots[command.destinationIndex];
        if (!slot.latestBirth ||
            command.particleGeneration !=
                slot.latestBirth->initialState.authorityTokens[1] ||
            !detail::gpuCommandSequenceAfter(
                command.commandSequence,
                slot.latestBirth->commandSequence)) {
            continue;
        }
        if (!slot.latestMatchingRetire || detail::gpuCommandSequenceAfter(
                command.commandSequence,
                slot.latestMatchingRetire->commandSequence)) {
            slot.latestMatchingRetire = &command;
        }
    }

    const size_t previousBirthCapacity = output.births.capacity();
    const size_t previousRetireCapacity = output.retires.capacity();
    output.births.reserve(std::min(slotCapacity, births.size()));
    output.retires.reserve(std::min(slotCapacity, retires.size()));
    if (output.births.capacity() != previousBirthCapacity) {
        ++scratch.statistics.capacityGrowths;
    }
    if (output.retires.capacity() != previousRetireCapacity) {
        ++scratch.statistics.capacityGrowths;
    }
    for (const detail::GpuParticleSlotLifecycle& slot : scratch.slots) {
        if (!slot.latestBirth) {
            if (slot.latestRetire) {
                output.retires.push_back(*slot.latestRetire);
            }
            continue;
        }

        // The latest birth overwrites every earlier generation in this slot.
        // Only a newer retire for that exact generation can affect the final
        // state. A stale retire for another generation remains a no-op.
        output.births.push_back(*slot.latestBirth);
        if (slot.latestMatchingRetire) {
            output.retires.push_back(*slot.latestMatchingRetire);
        }
    }

    scratch.statistics.birthOutputCapacity = output.births.capacity();
    scratch.statistics.retireOutputCapacity = output.retires.capacity();
    scratch.statistics.birthOutputHighWater = std::max(
        scratch.statistics.birthOutputHighWater, output.births.size());
    scratch.statistics.retireOutputHighWater = std::max(
        scratch.statistics.retireOutputHighWater, output.retires.size());

    const uint64_t retainedCommands =
        output.births.size() + output.retires.size();
    summary.coalescedCommands = acceptedCommands - retainedCommands;
    return summary;
}

// Reduces an arbitrarily ordered lifecycle journal to at most one birth and
// one generation-matched retire per stable slot. Sequence ordering uses the
// usual bounded uint32 serial-number comparison; a single authority epoch
// cannot contain 2^31 or more unconsumed commands.
[[nodiscard]] inline ParticleGpuCommandNormalization
normalizeGpuParticleCommands(
    uint32_t authorityEpoch,
    container::Span<const gpu_particle::GpuParticleBirthCommand> births,
    container::Span<const gpu_particle::GpuParticleRetireCommand> retires,
    size_t slotCapacity) {
    ParticleGpuCommandNormalization result;
    ParticleGpuCommandNormalizationScratch scratch;
    const ParticleGpuCommandNormalizationSummary summary =
        normalizeGpuParticleCommandsInto(
            result.batch, scratch, authorityEpoch, births, retires,
            slotCapacity);
    result.rejectedCommands = summary.rejectedCommands;
    result.coalescedCommands = summary.coalescedCommands;
    return result;
}

// Renderer retry queues use this after every authored batch. Replacing an
// epoch drops unreachable prior authority; merging within an epoch is always
// normalized immediately, so repeated dispatch failures cannot grow the
// pending journal beyond two commands per GPU slot.
inline void mergeGpuParticleCommandBatchRetained(
    ParticleGpuCommandBatch& pending,
    ParticleGpuCommandBatch incoming,
    size_t slotCapacity,
    ParticleGpuCommandNormalizationScratch& scratch,
    ParticleGpuCommandBatch& work) {
    (void)normalizeGpuParticleCommandsInto(
        work, scratch, incoming.authorityEpoch,
        incoming.births, incoming.retires, slotCapacity);
    if (pending.authorityEpoch == 0 ||
        pending.authorityEpoch != work.authorityEpoch) {
        std::swap(pending, work);
        scratch.statistics.birthOutputCapacity = std::max({
            scratch.statistics.birthOutputCapacity,
            pending.births.capacity(), work.births.capacity()});
        scratch.statistics.retireOutputCapacity = std::max({
            scratch.statistics.retireOutputCapacity,
            pending.retires.capacity(), work.retires.capacity()});
        return;
    }

    pending.births.insert(
        pending.births.end(),
        std::make_move_iterator(work.births.begin()),
        std::make_move_iterator(work.births.end()));
    pending.retires.insert(
        pending.retires.end(),
        std::make_move_iterator(work.retires.begin()),
        std::make_move_iterator(work.retires.end()));
    (void)normalizeGpuParticleCommandsInto(
        work, scratch, pending.authorityEpoch,
        pending.births, pending.retires, slotCapacity);
    std::swap(pending, work);
    scratch.statistics.birthOutputCapacity = std::max({
        scratch.statistics.birthOutputCapacity,
        pending.births.capacity(), work.births.capacity()});
    scratch.statistics.retireOutputCapacity = std::max({
        scratch.statistics.retireOutputCapacity,
        pending.retires.capacity(), work.retires.capacity()});
}

inline void mergeGpuParticleCommandBatch(
    ParticleGpuCommandBatch& pending,
    ParticleGpuCommandBatch incoming,
    size_t slotCapacity) {
    ParticleGpuCommandNormalizationScratch scratch;
    ParticleGpuCommandBatch work;
    mergeGpuParticleCommandBatchRetained(
        pending, std::move(incoming), slotCapacity, scratch, work);
}

} // namespace engine::fx
