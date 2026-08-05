#include "engine/renderer/world/effects/MotionBlurPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {

int32_t legacyMotionBlurPanFactor(int32_t authoredAmount) noexcept {
    return authoredAmount < 1 ? kLegacyMotionBlurDefaultPanFactor : authoredAmount;
}

int32_t legacyMotionBlurPanCount(float deltaLength, int32_t panFactor) noexcept {
    const int32_t factor = legacyMotionBlurPanFactor(panFactor);
    const double safeLength = std::isfinite(deltaLength)
        ? std::max(static_cast<double>(deltaLength), 0.0) : 0.0;
    const double minimum = static_cast<double>(factor / 2);
    const double maximum = static_cast<double>(factor);
    const double scaled = safeLength * 200.0 * maximum /
        static_cast<double>(kLegacyMotionBlurDefaultPanFactor);
    return static_cast<int32_t>(std::clamp(scaled, minimum, maximum));
}

LegacyMotionBlurSamplePlan legacyMotionBlurSamplePlan(
    int32_t maxCount, bool additive, LegacyMotionBlurGeometry geometry,
    float priorDeltaX, float priorDeltaY) noexcept {
    LegacyMotionBlurSamplePlan plan;
    plan.additive = additive;
    const int32_t limit = std::clamp(maxCount, 0, kLegacyMotionBlurMaximumTaps);
    plan.tapCount = static_cast<uint32_t>(limit);

    int32_t alpha = additive ? 0x09 : 0x15;
    if (additive) {
        if (maxCount > limit) alpha += (maxCount - limit) / 5;
        if (maxCount == kLegacyMotionBlurMaximumCount) alpha += 60;
    }
    plan.sampleAlpha = static_cast<float>(alpha) / 255.0f;
    plan.stepScaleX = additive ? 0.98f : 0.99f;
    plan.stepScaleY = plan.stepScaleX;

    if (geometry == LegacyMotionBlurGeometry::Radial) {
        const float count = static_cast<float>(std::clamp(
            maxCount, 0, kLegacyMotionBlurMaximumCount));
        plan.baseScale = std::sqrt(std::max(
            1.0f - (count / static_cast<float>(kLegacyMotionBlurMaximumCount)) * 0.90f,
            0.0f));
        return plan;
    }

    plan.centerY = 0.0f;
    // The pan branch uses factor + .006 on U and factor on V.
    plan.stepScaleX += 0.006f;
    if (geometry == LegacyMotionBlurGeometry::EndPan) {
        const float length = std::sqrt(
            priorDeltaX * priorDeltaX + priorDeltaY * priorDeltaY);
        if (std::isfinite(length) && length > std::numeric_limits<float>::epsilon()) {
            plan.centerX += 0.5f * (priorDeltaX / length);
            plan.centerY = 0.5f - 0.5f * (priorDeltaY / length);
        }
    }
    return plan;
}

} // namespace engine::render
