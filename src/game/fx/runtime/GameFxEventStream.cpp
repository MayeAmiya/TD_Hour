#include "core/container/container_types.h"
#include "GameFxEventStream.h"

#include <cmath>
#include <limits>
#include <utility>

namespace game {
namespace {

[[nodiscard]] uint64_t mixSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t hashName(container::StringView name) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char value : name) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] bool finiteAnchor(const FxInvocationAnchor& anchor) noexcept {
    return std::isfinite(anchor.position.x()) && std::isfinite(anchor.position.y()) &&
        std::isfinite(anchor.position.z()) && std::isfinite(anchor.rollRadians) &&
        std::isfinite(anchor.pitchRadians) && std::isfinite(anchor.yawRadians);
}

} // namespace

void FxInvocationEventStream::reset(uint64_t presentationEpoch,
                                    uint64_t sessionSeed) noexcept {
    m_presentationEpoch = presentationEpoch;
    m_sessionSeed = sessionSeed;
    m_confirmedFrame = 0;
    m_lastEmittedSequence = 0;
    m_consumedSequence = 0;
    m_rejectedEventCount = 0;
    m_eventOrdinal = 0;
    m_hasConfirmedFrame = false;
    m_events.clear();
}

bool FxInvocationEventStream::beginConfirmedFrame(
    uint64_t confirmedFrame, bool continueSameFrame) noexcept {
    if (m_presentationEpoch == 0 ||
        (m_hasConfirmedFrame &&
         (confirmedFrame < m_confirmedFrame ||
          (confirmedFrame == m_confirmedFrame && !continueSameFrame)))) {
        return false;
    }
    const bool newFrame = !m_hasConfirmedFrame ||
        confirmedFrame > m_confirmedFrame;
    m_confirmedFrame = confirmedFrame;
    if (newFrame) m_eventOrdinal = 0;
    m_hasConfirmedFrame = true;
    return true;
}

bool FxInvocationEventStream::emit(FxInvocationEvent event) {
    const bool stopGroup = event.control ==
        FxInvocationControlKind::StopAttachedParticleGroup;
    const bool stopAllAttached = event.control ==
        FxInvocationControlKind::StopAllAttachedParticles;
    const FxDirectParticleRequest* direct = event.directParticle
        ? &*event.directParticle : nullptr;
    const FxDirectBeamRequest* directBeam = event.directBeam
        ? &*event.directBeam : nullptr;
    const FxDirectScorchRequest* directScorch = event.directScorch
        ? &*event.directScorch : nullptr;
    const FxDirectRopeRequest* directRope = event.directRope
        ? &*event.directRope : nullptr;
    const bool beamEnd = directBeam &&
        directBeam->control == FxDirectBeamControl::End;
    const container::StringView effectName = direct
        ? container::StringView{direct->particleSystemName}
        : directBeam ? (beamEnd
            ? container::StringView{"LASER"}
            : container::StringView{directBeam->objectTemplate})
        : directScorch ? container::StringView{"SCORCH"}
        : directRope ? container::StringView{"ROPE"}
                     : container::StringView{event.fxListName};
    const bool directInvalid = direct &&
        (direct->particleSystemName.empty() || direct->emitterCount == 0 ||
         direct->particleSystemName.find('\0') != container::String::npos ||
         direct->fallbackParticleSystemName.find('\0') !=
             container::String::npos ||
         (direct->fallbackColorKeyTint &&
          (direct->fallbackParticleSystemName.empty() ||
           !std::isfinite(direct->fallbackColorKeyTint->x()) ||
           !std::isfinite(direct->fallbackColorKeyTint->y()) ||
           !std::isfinite(direct->fallbackColorKeyTint->z()) ||
           direct->fallbackColorKeyTint->x() < 0.0f ||
           direct->fallbackColorKeyTint->x() > 1.0f ||
           direct->fallbackColorKeyTint->y() < 0.0f ||
           direct->fallbackColorKeyTint->y() > 1.0f ||
           direct->fallbackColorKeyTint->z() < 0.0f ||
           direct->fallbackColorKeyTint->z() > 1.0f)) ||
         !std::isfinite(direct->footprintMajorRadius) ||
         !std::isfinite(direct->footprintMinorRadius) ||
         !std::isfinite(direct->maximumHeight) ||
         direct->footprintMajorRadius < 0.0f ||
         direct->footprintMinorRadius < 0.0f ||
         direct->maximumHeight < 0.0f ||
        direct->initialDelayMinimumFrames >
             direct->initialDelayMaximumFrames);
    const bool directBeamInvalid = directBeam &&
        (directBeam->objectTemplate.find('\0') != container::String::npos ||
         (beamEnd && directBeam->beamIdentity == 0) ||
         (!beamEnd &&
          (directBeam->objectTemplate.empty() ||
           !event.secondary ||
           (directBeam->control == FxDirectBeamControl::Update &&
            directBeam->beamIdentity == 0))));
    const bool directRopeInvalid = directRope &&
        (directRope->ropeIdentity == 0 ||
         !std::isfinite(directRope->maximumLength) ||
         !std::isfinite(directRope->currentLength) ||
         !std::isfinite(directRope->width) ||
         !std::isfinite(directRope->color.x()) ||
         !std::isfinite(directRope->color.y()) ||
         !std::isfinite(directRope->color.z()) ||
         !std::isfinite(directRope->wobbleLength) ||
         !std::isfinite(directRope->wobbleAmplitude) ||
         !std::isfinite(directRope->wobbleRatePerFrame) ||
         !std::isfinite(directRope->wobblePhase) ||
         !std::isfinite(directRope->verticalOffset) ||
         !std::isfinite(directRope->currentSpeedPerFrame) ||
         !std::isfinite(directRope->maximumSpeedPerFrame) ||
         !std::isfinite(directRope->accelerationPerFrame) ||
         (directRope->control != FxDirectRopeControl::End &&
          (directRope->maximumLength < 1.0f || directRope->width <= 0.0f ||
           directRope->wobbleLength <= 0.0f)));
    const bool directScorchInvalid = directScorch &&
        (!std::isfinite(directScorch->radius) ||
         directScorch->radius <= 0.0f);
    const bool secondaryBoneInvalid = event.secondaryBoneName.empty()
        ? event.secondaryBoneNameIsPrefix ||
              event.secondaryBoneNameSequenceOrdinal != 0 ||
              event.secondaryBoneNamePrefixFallsBackToBare ||
              event.secondaryWorldOffset.x() != 0.0f ||
              event.secondaryWorldOffset.y() != 0.0f ||
              event.secondaryWorldOffset.z() != 0.0f
        : !directBeam || !event.secondary || !event.secondary->object ||
              event.secondaryBoneName.size() > 128 ||
              event.secondaryBoneName.find('\0') !=
                  container::String::npos ||
              (!event.secondaryBoneNameIsPrefix &&
               (event.secondaryBoneNameSequenceOrdinal != 0 ||
                event.secondaryBoneNamePrefixFallsBackToBare));
    const uint32_t directKinds = static_cast<uint32_t>(direct != nullptr) +
        static_cast<uint32_t>(directBeam != nullptr) +
        static_cast<uint32_t>(directScorch != nullptr) +
        static_cast<uint32_t>(directRope != nullptr);
    if (m_presentationEpoch == 0 || !m_hasConfirmedFrame ||
        (!stopGroup && !stopAllAttached && effectName.empty()) ||
        ((stopGroup || stopAllAttached) &&
         (direct || directBeam || directScorch || directRope ||
                       !event.fxListName.empty() ||
                       !event.primary.object ||
                       (stopGroup && event.attachmentGroup == 0) ||
                       (stopAllAttached && event.attachmentGroup != 0))) ||
        (!event.fxListName.empty() && directKinds != 0) ||
        directKinds > 1 || directInvalid || directBeamInvalid ||
        directScorchInvalid || directRopeInvalid || secondaryBoneInvalid ||
        effectName.find('\0') != container::StringView::npos || !finiteAnchor(event.primary) ||
        (event.secondary && !finiteAnchor(*event.secondary)) ||
        !std::isfinite(event.primarySpeed) || !std::isfinite(event.overrideRadius) ||
        ((event.anchorKind == FxInvocationAnchorKind::ObjectAttachment ||
          event.anchorKind == FxInvocationAnchorKind::BonePosition) &&
         !event.primary.object) ||
        (event.anchorKind != FxInvocationAnchorKind::BonePosition &&
          (!event.boneName.empty() || event.boneNameIsPrefix ||
           event.boneNameSequenceOrdinal != 0 ||
           event.boneNamePrefixFallsBackToBare)) ||
        (event.anchorKind == FxInvocationAnchorKind::BonePosition &&
          (event.boneName.empty() || event.boneName.size() > 128 ||
           event.boneName.find('\0') != container::String::npos ||
           (!event.boneNameIsPrefix &&
            (event.boneNameSequenceOrdinal != 0 ||
             event.boneNamePrefixFallsBackToBare)))) ||
        !std::isfinite(event.attachmentLocalOffset.x()) ||
        !std::isfinite(event.attachmentLocalOffset.y()) ||
        !std::isfinite(event.attachmentLocalOffset.z()) ||
         !std::isfinite(event.secondaryWorldOffset.x()) ||
         !std::isfinite(event.secondaryWorldOffset.y()) ||
         !std::isfinite(event.secondaryWorldOffset.z()) ||
         m_eventOrdinal == std::numeric_limits<uint32_t>::max() ||
         m_lastEmittedSequence == std::numeric_limits<uint64_t>::max()) {
        ++m_rejectedEventCount;
        return false;
    }

    const uint64_t ordinal = ++m_eventOrdinal;
    event.streamSequence = ++m_lastEmittedSequence;
    event.confirmedFrame = m_confirmedFrame;
    if (event.localVisibilityRetryFrames != 0 &&
        event.localVisibilityFirstFrame == 0) {
        event.localVisibilityFirstFrame = m_confirmedFrame;
    }
    if (event.eventId == 0) {
        event.eventId = (m_confirmedFrame << 32u) | ordinal;
        if (event.eventId == 0) event.eventId = ordinal;
    }
    if (event.variationSeed == 0) {
        uint64_t key = m_sessionSeed;
        key ^= m_presentationEpoch * 0x94d049bb133111ebull;
        key ^= m_confirmedFrame * 0x9e3779b97f4a7c15ull;
        key ^= ordinal * 0xbf58476d1ce4e5b9ull;
        key ^= hashName(stopGroup ? container::StringView{"STOP_GROUP"}
                                  : effectName);
        if (event.primary.object) {
            key ^= static_cast<uint64_t>(event.primary.object.value) << 17u;
        }
        event.variationSeed = mixSeed(key);
        if (event.variationSeed == 0) event.variationSeed = 1;
    }
    // This is an exactly-once handoff, not a replaceable frame snapshot.
    // `take()` drains it at presentation extraction; an arbitrary size cap
    // would silently turn confirmed explosions, trails, or script FX into
    // missing visual events.
    m_events.push_back(std::move(event));
    return true;
}

container::Vector<FxInvocationEvent> FxInvocationEventStream::take() {
    container::Vector<FxInvocationEvent> output = std::move(m_events);
    m_events.clear();
    if (!output.empty()) m_consumedSequence = output.back().streamSequence;
    return output;
}

} // namespace game
