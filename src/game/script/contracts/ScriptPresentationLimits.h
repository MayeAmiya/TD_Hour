#pragma once

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::script {

// Authored presentation resource names cross compiler, session, and client
// ingress boundaries. Keep every producer and consumer on the same limit.
inline constexpr size_t kMaximumScriptPresentationNameLength = 1024;
// Camera operations are replayed by monotonically increasing sequence on the
// presentation thread. Retaining a bounded tail is sufficient for frame-rate
// skew while preventing a long cinematic session from copying an ever-growing
// journal into every UI projection.
inline constexpr size_t kMaximumScriptCameraPresentationCommands = 1024;

// Presentation journals cross a latest-value mailbox.  They must stay
// bounded, but a consumer must also be able to tell when the retained tail no
// longer contains its expected predecessor.  Keep the cursor boundary next to
// the trim operation so producers cannot silently discard a command.
//
// All current callers retain source-order commands with a non-zero
// `stamp.sequence`.  The helper deliberately does not attempt to coalesce
// them: camera paths and view-filter transitions are ordered operations, not
// desired-state values.
template <typename Command>
[[nodiscard]] inline bool trimScriptPresentationJournal(
    container::Vector<Command>& journal, size_t maximum,
    uint64_t& trimmedThroughSequence) {
    if (journal.size() <= maximum) return false;

    const size_t count = journal.size() - maximum;
    const uint64_t lastRemovedSequence = journal[count - 1].stamp.sequence;
    if (lastRemovedSequence > trimmedThroughSequence)
        trimmedThroughSequence = lastRemovedSequence;
    journal.erase(
        journal.begin(),
        journal.begin() + static_cast<std::ptrdiff_t>(count));
    return true;
}

} // namespace engine::script
