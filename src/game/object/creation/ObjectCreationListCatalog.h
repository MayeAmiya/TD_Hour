#pragma once

#include "core/container/hash_containers.h"

#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>
namespace game {

struct ObjectCreationListContentId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
    friend constexpr bool operator==(ObjectCreationListContentId,
                                     ObjectCreationListContentId) = default;
};

inline constexpr ObjectCreationListContentId
    INVALID_OBJECT_CREATION_LIST_CONTENT_ID{};

enum class ObjectCreationDisposition : uint16_t {
    LikeExisting = 1u << 0,
    OnGroundAligned = 1u << 1,
    SendItFlying = 1u << 2,
    SendItUp = 1u << 3,
    SendItOut = 1u << 4,
    RandomForce = 1u << 5,
    Floating = 1u << 6,
    InheritVelocity = 1u << 7,
    Whirling = 1u << 8,
};

using ObjectCreationDispositionMask = uint16_t;

[[nodiscard]] constexpr ObjectCreationDispositionMask
objectCreationDispositionBit(ObjectCreationDisposition value) noexcept {
    return static_cast<ObjectCreationDispositionMask>(value);
}

[[nodiscard]] constexpr bool objectCreationDispositionAllowsBouncing(
    ObjectCreationDispositionMask value) noexcept {
    constexpr ObjectCreationDispositionMask bounceMask =
        objectCreationDispositionBit(ObjectCreationDisposition::SendItFlying) |
        objectCreationDispositionBit(ObjectCreationDisposition::SendItUp) |
        objectCreationDispositionBit(ObjectCreationDisposition::RandomForce);
    return (value & bounceMask) != 0;
}

struct ObjectCreationFixedOffset final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

// Shared fields of the legacy GenericObjectCreationNugget. Authoring floats
// are normalized to Q32.32 at load time, before they can become persistent
// gameplay state or perturb a confirmed-frame operation.
struct ObjectCreationGenericFields final {
    container::Vector<container::String> names;
    container::String putInContainer;
    container::String particleSystem;
    container::String fadeSound;
    ObjectCreationFixedOffset offset;
    ObjectCreationDispositionMask disposition =
        objectCreationDispositionBit(
            ObjectCreationDisposition::OnGroundAligned);
    math::q32_32 dispositionIntensity{};
    math::q32_32 extraBounciness{};
    math::q32_32 extraFrictionPerSecond{};
    math::q32_32 spinRate{-1.0f};
    math::q32_32 yawRate{-1.0f};
    math::q32_32 rollRate{-1.0f};
    math::q32_32 pitchRate{-1.0f};
    math::q32_32 minimumForceMagnitude{};
    math::q32_32 maximumForceMagnitude{};
    math::q32_32 minimumForcePitchRadians{};
    math::q32_32 maximumForcePitchRadians{};
    math::q32_32 minimumHealth{int32_t{1}};
    math::q32_32 maximumHealth{int32_t{1}};
    math::q32_32 minimumFormationDistanceA{};
    math::q32_32 minimumFormationDistanceB{};
    math::q32_32 maximumFormationDistance{};
    uint32_t count = 1;
    uint32_t minimumLifetimeMilliseconds = 0;
    uint32_t maximumLifetimeMilliseconds = 0;
    uint32_t invulnerableMilliseconds = 0;
    uint32_t fadeMilliseconds = 0;
    bool ignorePrimaryObstacle = false;
    bool orientInForceDirection = false;
    bool spreadFormation = false;
    bool fadeIn = false;
    bool fadeOut = false;
    bool preserveLayer = true;
    bool diesOnBadLand = false;
};

struct ObjectCreationCreateObjectNugget final {
    ObjectCreationGenericFields common;
    uint32_t authoredOrder = 0;
    bool containInsideSourceObject = false;
    bool inheritsVeterancy = false;
    bool skipIfSignificantlyAirborne = false;
    bool requiresLivePlayer = false;
};

struct ObjectCreationDebrisAnimationSet final {
    container::String initial;
    container::String flying;
    container::String final;
};

enum class ObjectCreationMinimumLod : uint8_t {
    Low,
    Medium,
    High,
};

struct ObjectCreationCreateDebrisNugget final {
    ObjectCreationGenericFields common;
    uint32_t authoredOrder = 0;
    container::Vector<ObjectCreationDebrisAnimationSet> animationSets;
    container::String finalFx;
    container::String bounceSound;
    math::q32_32 mass{int32_t{1}};
    uint8_t shadowTypeMask = 0;
    ObjectCreationMinimumLod minimumLod = ObjectCreationMinimumLod::Low;
    bool okToChangeModelColor = false;
};

struct ObjectCreationApplyRandomForceNugget final {
    uint32_t authoredOrder = 0;
    math::q32_32 spinRate{};
    math::q32_32 minimumMagnitude{};
    math::q32_32 maximumMagnitude{};
    math::q32_32 minimumPitchRadians{};
    math::q32_32 maximumPitchRadians{};
};

struct ObjectCreationFireWeaponNugget final {
    uint32_t authoredOrder = 0;
    container::String weapon;
};

struct ObjectCreationAttackNugget final {
    uint32_t authoredOrder = 0;
    int32_t numberOfShots = 1;
    container::String weaponSlot = "PRIMARY";
    container::String deliveryDecal;
    math::q32_32 deliveryDecalRadius{};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    math::q32_32 deliveryDecalMinimumOpacity{int32_t{1}};
    math::q32_32 deliveryDecalMaximumOpacity{int32_t{1}};
    uint32_t deliveryDecalOpacityThrobMilliseconds = 1000;
    container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
};

struct ObjectCreationPayloadEntry final {
    container::String object;
    uint32_t count = 1;
};

// Frozen OCL transport/payload recipe consumed by the confirmed
// DeliverPayloadAI/Contain runtime. Content handles remain names until the
// session snapshot resolves each spawn/weapon at the structural boundary.
struct ObjectCreationDeliverPayloadNugget final {
    uint32_t authoredOrder = 0;
    container::String transport;
    container::String putInContainer;
    container::Vector<ObjectCreationPayloadEntry> payload;
    uint32_t formationSize = 1;
    uint32_t delayDeliveryMaximumMilliseconds = 0;
    math::q32_32 formationSpacing{25.0f};
    math::q32_32 weaponConvergenceFactor{};
    math::q32_32 weaponErrorRadius{};
    math::q32_32 deliveryDistance{};
    math::q32_32 preOpenDistance{};
    ObjectCreationFixedOffset dropOffset;
    ObjectCreationFixedOffset dropVariance;
    math::q32_32 exitPitchRate{};
    math::q32_32 diveStartDistance{};
    math::q32_32 diveEndDistance{};
    math::q32_32 strafeLength{};
    container::String visibleDropBoneBaseName;
    container::String visibleSubObjectBaseName;
    container::String visiblePayloadTemplateName;
    container::String visiblePayloadWeaponTemplate;
    container::String strafingWeaponSlot;
    container::String strafeWeaponFx;
    container::String deliveryDecal;
    math::q32_32 deliveryDecalRadius{};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    math::q32_32 deliveryDecalMinimumOpacity{int32_t{1}};
    math::q32_32 deliveryDecalMaximumOpacity{int32_t{1}};
    uint32_t deliveryDecalOpacityThrobMilliseconds = 1000;
    container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
    bool startAtPreferredHeight = true;
    bool startAtMaximumSpeed = false;
    bool inheritTransportVelocity = false;
    bool parachuteDirectly = false;
    bool selfDestructObject = false;
    bool fireWeapon = false;
    uint32_t maximumAttempts = 1;
    uint32_t dropDelayMilliseconds = 0;
    uint32_t visibleItemsDroppedPerInterval = 0;
    uint32_t visibleNumBones = 0;
};

using ObjectCreationNugget = std::variant<
    ObjectCreationCreateObjectNugget,
    ObjectCreationCreateDebrisNugget,
    ObjectCreationApplyRandomForceNugget,
    ObjectCreationDeliverPayloadNugget,
    ObjectCreationFireWeaponNugget,
    ObjectCreationAttackNugget>;

struct ObjectCreationListDefinition final {
    ObjectCreationListContentId id;
    container::String name;
    container::Vector<ObjectCreationNugget> nuggets;
};

// Immutable-after-load catalog. GameDataLoader owns one sealed shared
// instance; active sessions retain that exact handle through
// GameContentSnapshot and address recipes by stable sorted IDs.
class ObjectCreationListCatalog final {
public:
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfsFiles(
        const container::Vector<container::String>& logicalFiles,
        container::String* error = nullptr);
    // Applies one later INI source to an already loaded private catalog.
    // RefCode clears only a repeated OCL's nugget list; definitions absent
    // from the modifier remain intact.
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(
        container::Span<const container::StringView> loadRoots,
        container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] size_t size() const noexcept { return m_definitions.size(); }
    [[nodiscard]] const ObjectCreationListDefinition*
    find(container::StringView name) const noexcept;
    [[nodiscard]] ObjectCreationListContentId
    findId(container::StringView name) const noexcept;
    [[nodiscard]] const ObjectCreationListDefinition*
    find(ObjectCreationListContentId id) const noexcept;
    [[nodiscard]] const container::Vector<ObjectCreationListDefinition>& all() const noexcept {
        return m_definitions;
    }

private:
    container::Vector<ObjectCreationListDefinition> m_definitions;
    container::HashMap<container::String, ObjectCreationListContentId> m_ids;
    bool m_loaded = false;
};

} // namespace game

template <>
struct std::hash<game::ObjectCreationListContentId> {
    size_t operator()(game::ObjectCreationListContentId value) const noexcept {
        return std::hash<uint32_t>{}(value.value);
    }
};
