#pragma once

#include "core/container/container_types.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace engine {
struct RenderGameDataSettings;
struct ResolvedRenderFeatureSnapshot;
}

namespace engine::render {

using RenderEntityId = uint64_t;
using RenderMatrix = math::transform;
using RenderVector = math::vec3;
using RenderQuaternion = math::quat;
inline constexpr size_t kRenderWeaponSlotCount = 3;

// Copy-cheap immutable segmented snapshot column. Fresh extraction values are
// packed into bounded leaves while unchanged object groups may retain slices
// of prior leaves. The pointer directory keeps indexed reads O(1) without
// forcing cold strings/nested vectors through a per-tick payload copy.
template <typename T>
class SharedSnapshotVector final {
    using Vector = container::Vector<T>;

    struct Leaf final {
        Vector values;
    };

    struct Segment final {
        container::SharedPtr<const Leaf> leaf;
        size_t offset = 0;
        size_t count = 0;
    };

    struct Storage final {
        container::Vector<Segment> segments;
        container::Vector<size_t> segmentEnds;
        container::Vector<const T*> directory;
        size_t retainedElementCapacity = 0;
    };

public:
    using value_type = T;

    class ElementHandle final {
    public:
        ElementHandle() = default;

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_leaf && m_index < m_leaf->values.size();
        }
        [[nodiscard]] const T& get() const noexcept {
            return m_leaf->values[m_index];
        }
        [[nodiscard]] const T& operator*() const noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return &get(); }

        [[nodiscard]] static ElementHandle own(T value) {
            auto leaf = std::make_shared<Leaf>();
            leaf->values.push_back(std::move(value));
            return ElementHandle{std::move(leaf), 0u};
        }

    private:
        friend class SharedSnapshotVector;
        ElementHandle(container::SharedPtr<const Leaf> leaf, size_t index)
            : m_leaf(std::move(leaf)), m_index(index) {}

        container::SharedPtr<const Leaf> m_leaf;
        size_t m_index = 0;
    };

    class const_iterator final {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;
        [[nodiscard]] reference operator*() const noexcept {
            return (*m_owner)[m_index];
        }
        [[nodiscard]] pointer operator->() const noexcept { return &**this; }
        [[nodiscard]] reference operator[](difference_type offset) const noexcept {
            return (*m_owner)[static_cast<size_t>(
                static_cast<difference_type>(m_index) + offset)];
        }
        const_iterator& operator++() noexcept { ++m_index; return *this; }
        const_iterator operator++(int) noexcept {
            const_iterator copy = *this; ++*this; return copy;
        }
        const_iterator& operator--() noexcept { --m_index; return *this; }
        const_iterator operator--(int) noexcept {
            const_iterator copy = *this; --*this; return copy;
        }
        const_iterator& operator+=(difference_type offset) noexcept {
            m_index = static_cast<size_t>(
                static_cast<difference_type>(m_index) + offset);
            return *this;
        }
        const_iterator& operator-=(difference_type offset) noexcept {
            return *this += -offset;
        }
        friend const_iterator operator+(
            const_iterator value, difference_type offset) noexcept {
            value += offset; return value;
        }
        friend const_iterator operator+(
            difference_type offset, const_iterator value) noexcept {
            value += offset; return value;
        }
        friend const_iterator operator-(
            const_iterator value, difference_type offset) noexcept {
            value -= offset; return value;
        }
        friend difference_type operator-(
            const const_iterator& left,
            const const_iterator& right) noexcept {
            return static_cast<difference_type>(left.m_index) -
                static_cast<difference_type>(right.m_index);
        }
        friend bool operator==(
            const const_iterator&, const const_iterator&) noexcept = default;
        friend auto operator<=>(
            const const_iterator& left,
            const const_iterator& right) noexcept {
            return left.m_index <=> right.m_index;
        }

    private:
        friend class SharedSnapshotVector;
        const_iterator(const SharedSnapshotVector* owner, size_t index) noexcept
            : m_owner(owner), m_index(index) {}

        const SharedSnapshotVector* m_owner = nullptr;
        size_t m_index = 0;
    };

    SharedSnapshotVector() = default;
    SharedSnapshotVector(const SharedSnapshotVector&) = default;
    SharedSnapshotVector(SharedSnapshotVector&&) noexcept = default;
    SharedSnapshotVector& operator=(const SharedSnapshotVector&) = default;
    SharedSnapshotVector& operator=(SharedSnapshotVector&&) noexcept = default;
    SharedSnapshotVector& operator=(Vector value) {
        m_shared.reset();
        m_segments.clear();
        m_segmentSize = 0;
        m_owned = std::move(value);
        return *this;
    }

    [[nodiscard]] Vector& mutableValues() {
        if (m_shared || !m_segments.empty()) {
            throw std::logic_error(
                "sealed SharedSnapshotVector cannot be rewritten");
        }
        return m_owned;
    }
    void seal() {
        if (m_shared) return;
        flushOwnedLeaves();
        auto storage = std::make_shared<Storage>();
        storage->segments = std::move(m_segments);
        storage->segmentEnds.reserve(storage->segments.size());
        storage->directory.reserve(m_segmentSize);
        size_t end = 0;
        for (const Segment& segment : storage->segments) {
            end += segment.count;
            storage->segmentEnds.push_back(end);
            storage->retainedElementCapacity += segment.leaf->values.capacity();
            for (size_t index = 0; index < segment.count; ++index) {
                storage->directory.push_back(
                    &segment.leaf->values[segment.offset + index]);
            }
        }
        m_shared = std::move(storage);
        m_segmentSize = 0;
        m_owned.clear();
    }

    [[nodiscard]] bool isSealed() const noexcept {
        return static_cast<bool>(m_shared);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] size_t size() const noexcept {
        return m_shared ? m_shared->directory.size()
                        : m_segmentSize + m_owned.size();
    }
    [[nodiscard]] size_t capacity() const noexcept {
        return m_shared ? m_shared->retainedElementCapacity
                        : m_segmentSize + m_owned.capacity();
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator{this, 0u};
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator{this, size()};
    }
    [[nodiscard]] const T& operator[](size_t index) const noexcept {
        if (m_shared) return *m_shared->directory[index];
        size_t first = 0;
        for (const Segment& segment : m_segments) {
            if (index < first + segment.count) {
                return segment.leaf->values[
                    segment.offset + index - first];
            }
            first += segment.count;
        }
        return m_owned[index - first];
    }
    [[nodiscard]] const T& front() const noexcept { return (*this)[0]; }
    [[nodiscard]] const T& back() const noexcept {
        return (*this)[size() - 1u];
    }
    void clear() {
        m_shared.reset();
        m_segments.clear();
        m_segmentSize = 0;
        m_owned.clear();
    }
    void reserve(size_t size) { mutableValues().reserve(size); }
    void resize(size_t size) { mutableValues().resize(size); }
    void push_back(const T& value) { mutableValues().push_back(value); }
    void push_back(T&& value) { mutableValues().push_back(std::move(value)); }
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        return mutableValues().emplace_back(std::forward<Args>(args)...);
    }
    void appendOwned(T value) { m_owned.push_back(std::move(value)); }

    void appendSharedSlice(
        const SharedSnapshotVector& source, size_t offset, size_t count) {
        if (count == 0) return;
        flushOwnedLeaves();
        if (!source.m_shared || offset > source.size() ||
            count > source.size() - offset) {
            throw std::logic_error(
                "shared slice requires a sealed in-range source");
        }
        size_t sourceIndex = offset;
        size_t remaining = count;
        auto segmentAt = std::lower_bound(
            source.m_shared->segmentEnds.begin(),
            source.m_shared->segmentEnds.end(), sourceIndex + 1u);
        size_t segmentIndex = static_cast<size_t>(
            segmentAt - source.m_shared->segmentEnds.begin());
        while (remaining != 0) {
            const Segment& sourceSegment =
                source.m_shared->segments[segmentIndex];
            const size_t segmentBegin = segmentIndex == 0
                ? 0u : source.m_shared->segmentEnds[segmentIndex - 1u];
            const size_t within = sourceIndex - segmentBegin;
            const size_t available = sourceSegment.count - within;
            const size_t take = std::min(remaining, available);
            appendSegment({
                .leaf = sourceSegment.leaf,
                .offset = sourceSegment.offset + within,
                .count = take,
            });
            sourceIndex += take;
            remaining -= take;
            ++segmentIndex;
        }
    }

    void appendHandle(const ElementHandle& handle) {
        if (!handle) return;
        flushOwnedLeaves();
        appendSegment({
            .leaf = handle.m_leaf,
            .offset = handle.m_index,
            .count = 1u,
        });
    }

    [[nodiscard]] ElementHandle elementHandle(size_t index) const {
        if (!m_shared || index >= m_shared->directory.size()) {
            throw std::logic_error(
                "element handle requires a sealed in-range source");
        }
        const auto segmentAt = std::lower_bound(
            m_shared->segmentEnds.begin(), m_shared->segmentEnds.end(),
            index + 1u);
        const size_t segmentIndex = static_cast<size_t>(
            segmentAt - m_shared->segmentEnds.begin());
        const size_t segmentBegin = segmentIndex == 0
            ? 0u : m_shared->segmentEnds[segmentIndex - 1u];
        const Segment& segment = m_shared->segments[segmentIndex];
        return ElementHandle{
            segment.leaf, segment.offset + index - segmentBegin};
    }

    void copyTo(Vector& destination) const {
        destination.clear();
        destination.reserve(size());
        for (const T& value : *this) destination.push_back(value);
    }

    [[nodiscard]] Vector toVector() const {
        Vector result;
        copyTo(result);
        return result;
    }

private:
    static constexpr size_t leafElementLimit() noexcept {
        constexpr size_t kTargetLeafBytes = 12u * 1024u;
        const size_t target = sizeof(T) == 0
            ? 64u : kTargetLeafBytes / sizeof(T);
        return std::clamp<size_t>(target, 8u, 64u);
    }

    void appendSegment(Segment segment) {
        if (segment.count == 0) return;
        if (!m_segments.empty()) {
            Segment& tail = m_segments.back();
            if (tail.leaf == segment.leaf &&
                tail.offset + tail.count == segment.offset) {
                tail.count += segment.count;
                m_segmentSize += segment.count;
                return;
            }
        }
        m_segmentSize += segment.count;
        m_segments.push_back(std::move(segment));
    }

    void flushOwnedLeaves() {
        if (m_owned.empty()) return;
        const size_t limit = leafElementLimit();
        size_t first = 0;
        while (first < m_owned.size()) {
            const size_t count = std::min(limit, m_owned.size() - first);
            auto leaf = std::make_shared<Leaf>();
            leaf->values.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                leaf->values.push_back(std::move(m_owned[first + index]));
            }
            appendSegment({
                .leaf = std::move(leaf),
                .offset = 0u,
                .count = count,
            });
            first += count;
        }
        m_owned.clear();
    }

    container::SharedPtr<const Storage> m_shared;
    container::Vector<Segment> m_segments;
    size_t m_segmentSize = 0;
    Vector m_owned;
};

enum class RenderAnimationMode : uint8_t {
    Manual,
    Loop,
    Once,
    LoopPingPong,
    LoopBackwards,
    OnceBackwards,
};

enum class RenderOpacityFadeMode : uint8_t {
    None,
    In,
    Out,
};

enum class RenderTintEnvelopeMode : uint8_t {
    None,
    Attack,
    Release,
    Constant,
};

struct RenderTransform {
    RenderVector position{};
    RenderQuaternion orientation{0.0f, 0.0f, 0.0f, 1.0f};
    RenderVector scale{1.0f, 1.0f, 1.0f};
};

struct RenderSubObjectVisibility final {
    container::String name;
    bool visible = true;
    // Weapon muzzle/hide-show names may denote a contiguous Name01..99
    // family. An ordinal of zero with visible=false hides the complete
    // family; a non-zero ordinal selects one member. Ordinary condition
    // Show/Hide overrides leave this disabled.
    bool nameIsPrefix = false;
    uint32_t nameSequenceOrdinal = 0;
    bool namePrefixFallsBackToBare = false;
};

// Deterministic pose delta applied to one named hierarchy joint after the
// sampled animation delta and before the rest transform. Turret, recoil and
// other gameplay-authored controls share this backend-neutral value path.
struct RenderBoneControl final {
    container::String boneName;
    RenderVector translation{};
    RenderQuaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    bool boneNameIsPrefix = false;
    uint32_t boneNameSequenceOrdinal = 0;
    bool boneNamePrefixFallsBackToBare = false;
};

// Absolute confirmed weapon impulse. Recoil and the one-tick muzzle reveal
// are sampled by the renderer from the current world endpoint, so an
// otherwise stable armed object does not require 512 extraction refreshes.
struct RenderWeaponImpulse final {
    container::String recoilBone;
    container::String muzzleFlash;
    uint64_t fireTick = 0;
    // Renderer-local catch-up origin. Zero means sample from fireTick. If a
    // newest-only handoff skipped the exact logic endpoint containing this
    // impulse, WorldRenderPipeline starts the latest unseen impulse on the
    // first endpoint that can actually be presented instead of discarding its
    // muzzle/recoil edge as already old.
    uint64_t presentationTick = 0;
    uint32_t sequenceOrdinal = 0;
    float initialSpeed = 0.0f;
    float damping = 0.0f;
    float maximumDistance = 0.0f;
    float settleSpeed = 0.0f;
    bool recoilBoneIsPrefix = false;
    bool muzzleFlashIsPrefix = false;
};

struct RenderParticleSystemBone final {
    // Stable authored emitter identity. It includes render instance/channel,
    // resolved phase and declaration ordinal so duplicate bone/system pairs
    // never collapse and state changes stop the old emitter deterministically.
    uint64_t identity = 0;
    container::String boneName;
    container::String particleSystem;
    bool followsAnimatedBone = false;
    // ActiveBody publishes one compact Prefix01... descriptor per damage-FX
    // category. World preparation expands it from the immutable Skeleton;
    // authored ParticleSysBone entries keep the exact-bone default.
    bool numberedBonePrefix = false;
    uint32_t maximumEmitters = 1;
    // Non-zero groups share maximumEmitters across every Draw channel of one
    // Object. This preserves Object::getMultiLogicalBonePosition's object-wide
    // cap without moving W3D catalog queries back into gameplay extraction.
    uint64_t selectionGroup = 0;
};

struct RenderVehicleTreadState final {
    float leftOffset = 0.0f;
    float rightOffset = 0.0f;
    float middleOffset = 0.0f;
    bool enabled = false;
};

struct RenderSupplyBoneState final {
    container::String prefix;
    uint32_t currentSupply = 0;
    uint32_t maximumSupply = 0;
    bool enabled = false;
};

struct RenderPoliceCarState final {
    float animationFrame = 0.0f;
    uint64_t simulationFrame = 0;
    RenderVector diffuseColor{};
    RenderVector ambientColor{};
    float heightOffset = 8.0f;
    float innerRadius = 3.0f;
    float outerRadius = 20.0f;
    bool enabled = false;
};

struct RenderDebrisState final {
    container::String initialAnimation;
    container::String flyingAnimation;
    container::String finalAnimation;
    uint64_t ageFrames = 0;
    uint64_t finalAgeFrames = 0;
    uint32_t logicFramesPerSecond = 30;
    bool finalState = false;
    bool finalStop = false;
    bool enabled = false;
};

// W3D condition-state ShowSubObject/HideSubObject names may be authored as
// either the complete mesh name (`MODEL.PART`) or just its suffix (`PART`).
// Keep this backend-neutral because snapshot probes and every render backend
// must apply exactly the same case-insensitive, last-override-wins rule.
[[nodiscard]] inline bool renderSubObjectNameMatches(
    container::StringView meshName, container::StringView authoredName) noexcept {
    const auto equalsAsciiInsensitive = [](
        container::StringView lhs, container::StringView rhs) noexcept {
        if (lhs.size() != rhs.size()) return false;
        for (size_t index = 0; index < lhs.size(); ++index) {
            const auto fold = [](unsigned char value) noexcept {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value + ('a' - 'A'))
                    : value;
            };
            if (fold(static_cast<unsigned char>(lhs[index])) !=
                fold(static_cast<unsigned char>(rhs[index]))) {
                return false;
            }
        }
        return true;
    };
    const size_t separator = meshName.find_last_of('.');
    const container::StringView suffix = separator == container::StringView::npos
        ? meshName : meshName.substr(separator + 1u);
    return equalsAsciiInsensitive(meshName, authoredName) ||
        equalsAsciiInsensitive(suffix, authoredName);
}

[[nodiscard]] inline std::optional<bool> renderSubObjectVisibilityOverride(
    container::StringView meshName,
    container::Span<const RenderSubObjectVisibility> overrides) noexcept {
    for (auto overrideIt = overrides.rbegin(); overrideIt != overrides.rend();
         ++overrideIt) {
        if (renderSubObjectNameMatches(meshName, overrideIt->name)) {
            return overrideIt->visible;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool isRenderSubObjectVisible(
    container::StringView meshName,
    container::Span<const RenderSubObjectVisibility> overrides) noexcept {
    return renderSubObjectVisibilityOverride(meshName, overrides).value_or(true);
}

struct HeatVisionRenderState final {
    float intensity = 0.0f;
    bool only = false;
};

[[nodiscard]] inline float resolveFriendlyStealthOpacity(
    float explicitOpacity, float minimumOpacity,
    float pulsePhaseRadians, bool mine) noexcept {
    if (mine) return 0.0f;
    const float explicitValue = std::isfinite(explicitOpacity)
        ? std::clamp(explicitOpacity, 0.0f, 1.0f) : 1.0f;
    const float minimum = std::isfinite(minimumOpacity)
        ? std::clamp(minimumOpacity, 0.0f, 1.0f) : 0.5f;
    const float phase = std::isfinite(pulsePhaseRadians)
        ? pulsePhaseRadians : 0.0f;
    const float pulse = 0.5f + std::sin(phase) * 0.5f;
    return explicitValue * (minimum + (1.0f - minimum) * pulse);
}

// Pure observer-relative projection of RefCode's stealth material selection.
// Keeping this decision backend-neutral also makes the original mine/death
// exceptions independently probeable without constructing a D3D device.
[[nodiscard]] constexpr HeatVisionRenderState resolveHeatVisionRenderState(
    bool stealthed, bool detected, bool alliedToObserver,
    bool mine, bool effectivelyDead) noexcept {
    if (!stealthed || !detected || mine || effectivelyDead) return {};
    return {.intensity = 1.0f, .only = !alliedToObserver};
}

// Detached copy of ThingTemplate's authored ShadowTypeInfo. Keep the legacy
// bit values at the game/render boundary so replayed snapshots never have to
// consult mutable INI content to choose between volume and projector paths.
enum class RenderShadowFlag : uint8_t {
    None = 0x00,
    Decal = 0x01,
    Volume = 0x02,
    Projection = 0x04,
    DynamicProjection = 0x08,
    DirectionalProjection = 0x10,
    AlphaDecal = 0x20,
    AdditiveDecal = 0x40,
};

[[nodiscard]] constexpr uint8_t renderShadowBit(RenderShadowFlag flag) noexcept {
    return static_cast<uint8_t>(flag);
}

// Static GameLOD/Options switches gate the two original shadow systems
// independently. Dynamic/directional projection are modifiers of the decal
// path, not a third shadow system, so they leave the snapshot together with
// the other projector bits when 2D shadows are disabled.
[[nodiscard]] constexpr uint8_t filterRenderShadowTypeMask(
    uint8_t typeMask, bool useVolumes, bool useDecals) noexcept {
    if (!useVolumes) {
        typeMask &= static_cast<uint8_t>(
            ~renderShadowBit(RenderShadowFlag::Volume));
    }
    if (!useDecals) {
        constexpr uint8_t decalMask =
            renderShadowBit(RenderShadowFlag::Decal) |
            renderShadowBit(RenderShadowFlag::Projection) |
            renderShadowBit(RenderShadowFlag::DynamicProjection) |
            renderShadowBit(RenderShadowFlag::DirectionalProjection) |
            renderShadowBit(RenderShadowFlag::AlphaDecal) |
            renderShadowBit(RenderShadowFlag::AdditiveDecal);
        typeMask &= static_cast<uint8_t>(~decalMask);
    }
    return typeMask;
}

struct RenderShadowDescriptor final {
    uint8_t typeMask = renderShadowBit(RenderShadowFlag::None);
    container::String textureName;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    [[nodiscard]] constexpr bool has(RenderShadowFlag flag) const noexcept {
        return (typeMask & renderShadowBit(flag)) != 0;
    }
    [[nodiscard]] constexpr bool castsDirectionalShadow() const noexcept {
        return has(RenderShadowFlag::Volume);
    }
    [[nodiscard]] constexpr bool usesGroundProjector() const noexcept {
        if (castsDirectionalShadow()) return false;
        constexpr uint8_t projectorKinds =
            renderShadowBit(RenderShadowFlag::Decal) |
            renderShadowBit(RenderShadowFlag::Projection) |
            renderShadowBit(RenderShadowFlag::AlphaDecal) |
            renderShadowBit(RenderShadowFlag::AdditiveDecal);
        return (typeMask & projectorKinds) != 0;
    }
};

enum class RenderAnimationStartKind : uint8_t {
    Default,
    FirstFrame,
    LastFrame,
    RandomFrame,
    MaintainFraction,
};

enum class RenderAnimationCompletionPhase : uint8_t {
    PresentedSource,
    Transition,
    ActiveState,
};

// Resource admission and natural completion share one ordered feedback lane.
// Pending/Ready only gates the matching generation+phase clock; Completed
// retains the existing phase-retirement contract.
enum class RenderAnimationFeedbackKind : uint8_t {
    EndpointPublished,
    ResourcePending,
    ResourceReady,
    Completed,
    Count,
};

// Compact per-live-channel fact copied independently of mesh visibility,
// LOD and resource residency. The renderer acknowledges this generation only
// after the containing world endpoint has completed preparation and becomes
// the published B endpoint.
struct RenderAnimationEndpointAdmission final {
    RenderEntityId objectId = 0;
    uint32_t channelIndex = 0;
    uint64_t generation = 0;
};

// State-enter policy sealed by the confirmed simulation.  The renderer owns
// immutable clip metadata, so it converts a requested endpoint/fraction to a
// sample time without inventing when or why the animation state changed.
struct RenderAnimationStartDescriptor final {
    RenderAnimationStartKind kind = RenderAnimationStartKind::Default;
    float randomFraction = 0.0f;
    container::String sourceModelAsset;
    container::String sourceAnimationState;
    float sourceTimeSeconds = 0.0f;
    float sourceRate = 1.0f;
    RenderAnimationMode sourceMode = RenderAnimationMode::Once;
    uint64_t generation = 0;
    bool restartWhenComplete = false;
};

struct RenderVisualState {
    // Keep the complete RefCode ModelConditionFlags payload across the
    // game/render boundary.  The original condition set currently has 119
    // named bits, so a legacy uint64_t would silently drop states such as
    // FLOODED and later flags during snapshot extraction.
    container::Array<uint64_t, 2> modelConditionFlags{};
    container::String animationState;
    float animationTimeSeconds = 0.0f;
    // Confirmed tick at which animationTimeSeconds was sampled. Incremental
    // extraction may reuse an otherwise unchanged descriptor in a newer
    // world snapshot; the renderer advances from this absolute endpoint
    // instead of forcing every playing object into RenderExtraction dirty.
    uint64_t animationSampleTick = 0;
    uint64_t animationStateEnterTick = 0;
    RenderAnimationStartDescriptor animationStart;
    // Confirmed game acknowledgement bits for PresentedSource, Transition,
    // and ActiveState completion. The renderer suppresses repeated feedback
    // for phases already consumed in this generation.
    uint8_t animationCompletionMask = 0;
    uint64_t animationResourcePendingGeneration = 0;
    uint8_t animationResourcePendingPhase = UINT8_MAX;
    float animationRate = 1.0f;
    float animationLoopDurationSeconds = 0.0f;
    // W3DModelDraw::adjustAnimSpeedToMovementSpeed(). The authored second
    // token of `Animation = <clip> <distanceCovered>` is the ground distance
    // one full cycle of the clip is supposed to cover, so
    // distanceCovered / speed is the duration the clip must take for the feet
    // (or treads) to stay planted. Extraction publishes only that duration
    // because the authored clip length lives with the renderer asset; the
    // renderer converts it to a frame-rate multiplier exactly like
    // setCurAnimDurationInMsec(). Zero means "no authored distance, or the
    // object is not moving", i.e. keep the authored/random speed factor.
    float animationSpeedSyncDurationSeconds = 0.0f;
    RenderAnimationMode animationMode = RenderAnimationMode::Loop;
    // Only consumed by Manual playback. It remains a frame index until the
    // renderer resolves the immutable clip and its authored frame rate.
    uint32_t animationManualFrame = 0;
    bool animationPaused = false;
    bool animationCompleted = false;
    container::Vector<RenderSubObjectVisibility> subObjectVisibility;
    container::Vector<RenderBoneControl> boneControls;
    container::Vector<RenderWeaponImpulse> weaponImpulses;
    container::Vector<RenderParticleSystemBone> particleSystemBones;
    RenderVehicleTreadState vehicleTreads;
    RenderSupplyBoneState supplyBones;
    RenderPoliceCarState policeCar;
    RenderDebrisState debris;
    // FloatUpdate rebuilds the Drawable matrix from confirmed Z yaw and a
    // legacy frame-phase sway. Incremental extraction keeps the absolute
    // sample tick; renderer advancement is allowed only while the logic-side
    // update was not frozen by Disabled state.
    float floatSwayBaseYawRadians = 0.0f;
    uint64_t floatSwaySampleTick = 0;
    bool floatSwayEnabled = false;
    bool floatSwayRunning = false;
    bool selected = false;
    bool hidden = false;
    // Signed additive Drawable tint sampled from confirmed-tick presentation
    // state. Positive script flashes and legacy status envelopes (including
    // FRENZY's negative green/blue terms) share the same renderer lane.
    // It is intentionally separate from selection and cached W3D assets.
    RenderVector scriptFlashTint{};
    RenderVector scriptFlashBaseTint{};
    RenderVector scriptFlashColor{};
    uint64_t scriptFlashFirstPulseTick = 0;
    uint64_t scriptFlashEndTick = 0;
    uint32_t scriptFlashPulseIntervalTicks = 0;
    uint32_t scriptFlashDecayTicks = 0;
    bool scriptFlashEnabled = false;
    RenderTintEnvelopeMode disabledTintMode =
        RenderTintEnvelopeMode::None;
    uint64_t disabledTintStartTick = 0;
    float disabledTintReleaseStartScale = 0.0f;
    float disabledTintSampleScale = 0.0f;
    RenderTintEnvelopeMode temporaryBonusTintMode =
        RenderTintEnvelopeMode::None;
    uint64_t temporaryBonusTintStartTick = 0;
    float temporaryBonusTintReleaseStartScale = 0.0f;
    float temporaryBonusTintSampleAppliedScale = 0.0f;
    bool temporaryBonusTintInfantry = false;
    // RefCode's heat-vision material is object-local, not a post-process.
    // A detected stealthed enemy suppresses the ordinary material and shows
    // only the orange emissive response; a detected friendly keeps its base
    // material and receives the same response additively. Zero disables it.
    float heatVisionIntensity = 0.0f;
    bool heatVisionOnly = false;
    float friendlyStealthBaseOpacity = 1.0f;
    float friendlyStealthMinimumOpacity = 0.5f;
    float friendlyStealthPulsePhaseRadians = 0.0f;
    bool friendlyStealthPulseEnabled = false;
    bool friendlyStealthPulseRunning = false;
    // Final Drawable alpha after explicit/script opacity and the
    // observer-relative friendly stealth pulse are multiplied. Values below
    // one move ordinary W3D packets to the transparent object layer.
    float objectOpacity = 1.0f;
    RenderOpacityFadeMode opacityFadeMode = RenderOpacityFadeMode::None;
    uint64_t opacityFadeStartTick = 0;
    uint32_t opacityFadeDurationFrames = 0;
    // Ordinary owner/player colour always enters the render instance.
    // NAMED_CUSTOM_COLOR may replace it only when the selected Draw rule
    // opted into OkToChangeModelColor. The renderer applies either value
    // through frame-local packet constants and never mutates cached assets.
    bool hasScriptIndicatorColor = false;
    RenderVector scriptIndicatorColor{};
    // Explicit immutable-module opt-in copied from RenderModelComponent.
    // This is intentionally not inferred from the W3D name: only a legacy
    // SwayClientUpdate recipe can make SET_TREE_SWAY affect an instance.
    bool treeSwayEnabled = false;
    // W3DTreeDraw uses one authored replacement atlas for the buffered tree,
    // independent of the immutable W3D material table.
    container::String treeTextureAsset;
    // Upright push-aside deformation. Direction is world-space; the vertex
    // shader scales displacement by the source vertex's object-space height.
    RenderVector treePushAsideDirection{};
    float treePushAsideAmount = 0.0f;
    float treePushAsideDistanceFactor = 0.0f;
    float treePushAsideDarkeningFactor = 0.0f;
    bool receivesDynamicLights = true;
    // Enemy/fog-memory drawables consume the observer's shroud luminance.
    // Friendly drawables leave this false, while the independent map-border
    // gate is applied to every ordinary world object by packet preparation.
    bool receivesLocalVisibility = false;
};

// A TransitionState is sampled first, then deterministically hands off to
// this normal ConditionState after the transition Once clip reaches its last
// authored frame. Both phases are sealed value data: the renderer may choose
// which phase to sample, but never writes the result back into game state.
struct RenderAnimationCompletionTarget final {
    container::String modelAsset;
    container::String animationState;
    float animationRate = 1.0f;
    // Per-phase counterpart of RenderVisualState's field. A TransitionState
    // inherits the DefaultConditionState animation list, so a handoff phase can
    // carry its own authored distanceCovered.
    float animationSpeedSyncDurationSeconds = 0.0f;
    RenderAnimationMode animationMode = RenderAnimationMode::Loop;
    uint32_t animationManualFrame = 0;
    RenderAnimationStartDescriptor animationStart;
    container::Vector<RenderSubObjectVisibility> subObjectVisibility;
    container::Vector<RenderBoneControl> boneControls;
    container::Vector<RenderParticleSystemBone> particleSystemBones;
    container::Array<container::String, kRenderWeaponSlotCount> weaponLaunchBones;
    container::Array<uint32_t, kRenderWeaponSlotCount>
        weaponLaunchBoneSequenceOrdinals{};
};

// A model/animation phase that is not necessarily active in this world
// endpoint but may be selected by a later ConditionState or TransitionState.
// Loading snapshots carry the deduplicated phases of their live archetypes so
// the renderer can make state changes resident before dismissing the loader.
struct RenderModelPhaseDependency final {
    container::String modelAsset;
    container::String animationState;
};

// Renderer-owned clip metadata determines the exact authored completion
// boundary, but only the confirmed game state may retire a wait/transition or
// choose a new idle/restart candidate. Return a detached, generation-stamped
// fact; the game rejects stale session/object/channel generations before
// changing its presentation clock.
struct RenderAnimationCompletionFeedback final {
    uint64_t presentationEpoch = 0;
    uint64_t simulationFrame = 0;
    RenderEntityId objectId = 0;
    uint32_t channelIndex = 0;
    uint64_t generation = 0;
    RenderAnimationCompletionPhase phase =
        RenderAnimationCompletionPhase::ActiveState;
    RenderAnimationFeedbackKind kind =
        RenderAnimationFeedbackKind::Completed;
    float completedDurationSeconds = 0.0f;
};

enum class LocalVisibilityRenderCellState : uint8_t {
    Shrouded = 0,
    Explored = 1,
    Visible = 2,
};

// RefCode has two deliberately different object-memory paths. Ordinary
// drawables remain live for a short grace period after leaving clear sight;
// eligible immobile structures instead freeze their last clear W3D state
// while the footprint is merely explored. Keep that classification in the
// detached snapshot so the renderer never re-opens ThingTemplate/ECS data.
enum class RenderLocalVisibilityMemoryPolicy : uint8_t {
    None,
    Timed,
    StaticGhost,
    // Active map boundary: no grace, no ghost and no prior-memory reuse.
    HardHidden,
};


} // namespace engine::render
