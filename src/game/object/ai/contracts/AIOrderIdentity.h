#pragma once

#include <cstdint>

#include "core/ecs/ObjectId.h"

namespace engine::ai
{

// Mirrors of ObjectAIOrderSource::Count / ObjectAIOrderSystemPurpose::Count,
// duplicated so service protocol headers need not depend on the runtime
// admission header.  ObjectAIOrderAdmission.h static_asserts both against the
// real enums: the previous hand-written literals had already drifted (this bound
// said 22 while the enum had grown to 23 with IntentionalContact), which
// silently invalidated any identity carrying the newest purpose and made
// matchesAIAsyncOrderIdentity drop its completions and feedback.
inline constexpr uint8_t kAIAsyncOrderSourceCount = 3;
inline constexpr uint8_t kAIAsyncOrderSystemPurposeCount = 28;

// Complete source-order identity carried by asynchronous AI value protocols.
// Numeric source/purpose values mirror the ECS/admission enums without making
// service headers depend on an ECS component or runtime storage.
struct AIAsyncOrderIdentity final
{
    ObjectId subject = INVALID_OBJECT_ID;
    uint64_t queueRevision = 0;
    uint64_t externalRevision = 0;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
    uint32_t sourceScriptId = 0;
    uint32_t systemPurposeInstance = 0;
    uint8_t source = 0xff;
    uint8_t systemPurpose = 0xff;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && queueRevision != 0 &&
               source < kAIAsyncOrderSourceCount &&
               systemPurpose < kAIAsyncOrderSystemPurposeCount;
    }

    constexpr bool operator==(const AIAsyncOrderIdentity&) const noexcept = default;
};

} // namespace engine::ai
