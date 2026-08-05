#pragma once

#include "core/container/hash_containers.h"
#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <functional>

namespace engine::render {

enum class GroundDecalPresentationSource : uint8_t {
    ObjectRadius,
    NeutronDelivery,
    DynamicShroudGrid,
    ObjectTerrain,
    SpectreAttackArea,
    SpectreTargetingReticle,
};

enum class GroundDecalPresentationEventKind : uint8_t {
    Begin,
    Update,
    End,
};

struct GroundDecalPresentationKey final {
    GroundDecalPresentationSource source =
        GroundDecalPresentationSource::ObjectRadius;
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;

    friend bool operator==(const GroundDecalPresentationKey&,
                           const GroundDecalPresentationKey&) = default;
};

struct GroundDecalPresentationKeyHash final {
    size_t operator()(const GroundDecalPresentationKey& key) const noexcept {
        size_t result = std::hash<uint32_t>{}(key.object.value);
        result ^= std::hash<uint32_t>{}(key.authoredOrder) +
            0x9e3779b9u + (result << 6u) + (result >> 2u);
        result ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.source)) +
            0x9e3779b9u + (result << 6u) + (result >> 2u);
        return result;
    }
};

struct GroundDecalPresentationEvent final {
    GroundDecalPresentationEventKind kind =
        GroundDecalPresentationEventKind::Begin;
    GroundDecalPresentationKey key;
    uint64_t confirmedFrame = 0;
    uint64_t streamSequence = 0;
    uint8_t ownerPlayer = 0xff;
    container::String textureName;
    math::vec3 position{};
    float radius = 0.0f;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float yawRadians = 0.0f;
    uint32_t shadowTypeMask = 0x20u;
    float minimumOpacity = 1.0f;
    float maximumOpacity = 1.0f;
    uint64_t opacityThrobFrames = 30;
    float fadeInRatePerFrame = 0.0f;
    float fadeOutRatePerFrame = 0.0f;
    float authoritativeOpacity = 1.0f;
    bool hasAuthoritativeOpacity = false;
    math::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    bool onlyVisibleToOwningPlayer = true;
    bool requiresDrawIconUi = false;
    uint32_t decalCount = 30;
    uint32_t gridSnapSize = 23;
    float initialDecalRadius = 100.0f;
    float nativeClearingRange = 0.0f;
    float currentClearingRange = 0.0f;
    uint64_t totalFrames = 1;
    uint64_t stateCountdown = 0;
};

struct GroundDecalPresentationBatch final {
    uint64_t presentationEpoch = 0;
    uint64_t confirmedFrame = 0;
    uint8_t observerPlayer = 0xff;
    bool drawIconUiEnabled = true;
    bool hasCompleteOwnerSet = false;
    container::Vector<GroundDecalPresentationKey> synchronizedOwners;
    container::Vector<GroundDecalPresentationEvent> events;
};

} // namespace engine::render
