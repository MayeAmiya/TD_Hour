#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateSoAParity.h"

namespace engine::ai
{

struct AIStateSoASlotSnapshot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateMachineRuntime runtime;
    AIStateData state;
};

enum class AIStateSoASnapshotError : uint8_t
{
    None = 0,
    SizeMismatch = uint8_t{1} << 0,
    SubjectMismatch = uint8_t{1} << 1,
    PayloadTagMismatch = uint8_t{1} << 2,
    PayloadMetadataInvalid = uint8_t{1} << 3,
    BridgeRejected = uint8_t{1} << 4,
};

struct AIStateSoASnapshotResult final
{
    uint8_t bits = 0;
    size_t slotsProcessed = 0;

    [[nodiscard]] constexpr bool succeeded() const noexcept { return bits == 0; }
    [[nodiscard]] constexpr bool has(AIStateSoASnapshotError error) const noexcept
    {
        return (bits & static_cast<uint8_t>(error)) != 0;
    }
    constexpr void add(AIStateSoASnapshotError error) noexcept
    {
        bits |= static_cast<uint8_t>(error);
    }
};

// Output storage is caller-owned so confirmed-tick snapshotting performs no
// hidden allocation. Size is preflighted before the first output is touched.
[[nodiscard]] inline AIStateSoASnapshotResult captureAIStateSoASnapshot(
    const AIStateFamilySoAStorage& storage,
    container::Span<AIStateSoASlotSnapshot> output) noexcept
{
    AIStateSoASnapshotResult result;
    if (output.size() != storage.size())
    {
        result.add(AIStateSoASnapshotError::SizeMismatch);
        return result;
    }

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    for (size_t slot = 0; slot < storage.size(); ++slot)
    {
        AIStateData rebuilt;
        if (!rebuildAIStateDataFromSoASlot(storage, slot, rebuilt).succeeded())
        {
            result.add(AIStateSoASnapshotError::BridgeRejected);
            return result;
        }
        output[slot] = {
            .subject = subjects[slot],
            .runtime = runtimes[slot],
            .state = rebuilt,
        };
        ++result.slotsProcessed;
    }
    return result;
}

// Restore is transactional with respect to all validation failures: every
// slot's identity, payload tag and metadata are checked before any storage or
// runtime field is modified.
[[nodiscard]] inline AIStateSoASnapshotResult restoreAIStateSoASnapshot(
    AIStateFamilySoAStorage& storage,
    container::Span<const AIStateSoASlotSnapshot> snapshot) noexcept
{
    AIStateSoASnapshotResult result;
    if (snapshot.size() != storage.size())
    {
        result.add(AIStateSoASnapshotError::SizeMismatch);
        return result;
    }

    const auto subjects = storage.subjects();
    for (size_t slot = 0; slot < storage.size(); ++slot)
    {
        if (snapshot[slot].subject != subjects[slot])
            result.add(AIStateSoASnapshotError::SubjectMismatch);
        if (!detail::payloadTagMatches(snapshot[slot].state))
            result.add(AIStateSoASnapshotError::PayloadTagMismatch);
        if (snapshot[slot].state.payloadState != AIStateId::Invalid &&
            snapshot[slot].state.activationSequence == 0)
            result.add(AIStateSoASnapshotError::PayloadMetadataInvalid);
    }
    if (!result.succeeded())
        return result;

    auto runtimes = storage.runtimes();
    for (size_t slot = 0; slot < storage.size(); ++slot)
    {
        if (!writeAIStateDataToSoASlot(snapshot[slot].state, storage, slot).succeeded())
        {
            // All possible bridge rejection conditions were preflighted.
            // Keep this observable if a future bridge adds another invariant.
            result.add(AIStateSoASnapshotError::BridgeRejected);
            return result;
        }
        runtimes[slot] = snapshot[slot].runtime;
        ++result.slotsProcessed;
    }
    return result;
}

} // namespace engine::ai
