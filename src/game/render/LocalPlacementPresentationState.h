#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/command/CommandButtonStore.h"
#include "game/command/GameCommand.h"
#include "core/ecs/ObjectId.h"
#include "game/render/TerrainBibPresentation.h"

#include <cstdint>

namespace engine::selection {

enum class LocalPlacementCommandKind : uint8_t {
    None,
    Begin,
    Cancel,
};

struct LocalPlacementCommandIntent final {
    LocalPlacementCommandKind kind = LocalPlacementCommandKind::None;
    // View into the frozen CommandButton. Activation consumes it immediately,
    // so the click path does not allocate a duplicate product name.
    container::StringView productType;
};

// Resolves only the two local placement commands. UNIT_BUILD remains a
// production-queue command and must never open a ground-placement cursor.
[[nodiscard]] LocalPlacementCommandIntent resolveLocalPlacementCommand(
    const game::CommandButtonTemplate& button);

enum class LocalPlacementLegality : uint8_t {
    Unchecked,
    Legal,
    Illegal,
};

// The placement cursor is shared presentation machinery, but confirmation
// has exactly one authoritative owner.  Keep that choice typed while the
// cursor is active so a SpecialPower construct can never fall through to the
// ordinary builder transaction merely because both preview the same object.
enum class LocalPlacementBackendKind : uint8_t {
    Build,
    SpecialPowerConstruct,
};

enum class LocalPlacementPreviewFeedback : uint8_t {
    Cursor,
    Queued,
    Rejected,
};

// Local construction routes share the placement render domain, but must not
// reuse either the live cursor allocator's low ordinals or authoritative
// ObjectIds. The renderer groups durable channels by both id and objectId, so
// an identity collision can retire unrelated world drawables for a frame.
[[nodiscard]] constexpr render::RenderEntityId
localConstructionRoutePreviewIdentity(uint64_t ordinal) noexcept {
    constexpr uint64_t kPlacementDomain = 0xc000000000000000ull;
    constexpr uint64_t kRouteOrdinalDomain = 0x0000800000000000ull;
    constexpr uint64_t kOrdinalMask = 0x00003fffffffffffull;
    return kPlacementDomain | kRouteOrdinalDomain |
        (ordinal & kOrdinalMask);
}

[[nodiscard]] constexpr render::RenderEntityId
localConstructionRouteAnchorIdentity(ObjectId builder) noexcept {
    constexpr uint64_t kAnchorSubdomain = 0x0000400000000000ull;
    return localConstructionRoutePreviewIdentity(builder.value) |
        kAnchorSubdomain;
}

struct LocalPlacementPreviewSnapshot final {
    uint64_t presentationEpoch = 0;
    // Confirmed presentation time at which the client-only Drawable was
    // created. RefCode starts a placement icon's animations at newDrawable(),
    // not at level tick zero.
    uint64_t animationStartTick = 0;
    render::RenderEntityId previewIdentity = 0;
    ObjectId sourceObject = INVALID_OBJECT_ID;
    // Confirmed order correlation for queued construction nodes. Cursor-only
    // previews leave this zero; authority revalidates builder+sequence before
    // removing a selected node.
    uint32_t sourceSequence = 0;
    container::String objectType;
    CommandPosition fixedPosition;
    math::q32_32 fixedYawRadians{};
    render::RenderVector position{};
    float yawRadians = 0.0f;
    // Main-thread selection uses this already-frozen world-space radius for
    // client-local route ghosts.  It is derived while the logic thread has
    // the immutable product geometry; input must not inspect content or ECS.
    float selectionRadius = 0.0f;
    // Present only for an anchored LINEBUILD preview. The first tile's
    // position is the command start; this retains the original cursor end so
    // network/replay authority can reconstruct the line instead of trusting
    // the visible tile list.
    CommandPosition fixedLineEndPosition;
    render::RenderVector lineEndPosition{};
    bool hasPose = false;
    bool hasLineEndPosition = false;
    LocalPlacementLegality legality = LocalPlacementLegality::Unchecked;
    LocalPlacementBackendKind backend = LocalPlacementBackendKind::Build;
    LocalPlacementPreviewFeedback feedback =
        LocalPlacementPreviewFeedback::Cursor;
    // A confirmed construction site can remain the first point of a queued
    // builder route after its Build order has moved out of the external order
    // queue and into ObjectBuilder's active task.  Keep that authoritative
    // point for route assembly without drawing a second preview over the live
    // construction site.
    bool routeAnchorOnly = false;
    // Client-local construction-route node. These snapshots are mirrored into
    // the existing placement presentation stream solely to draw previews and
    // links; no ObjectOrderQueue entry exists until arrival publishes Build.
    bool routeLocalOnly = false;
    CommandActivationContext activation;
};

struct TimedLocalPlacementPreview final {
    LocalPlacementPreviewSnapshot placement;
    uint64_t expiresAfterTick = 0;
};

// One caller-planned place icon. Obstructions are copied only for an explicit
// Illegal result; the presentation state never discovers blockers itself.
struct LocalPlacementPreviewTileInput final {
    CommandPosition position;
    math::q32_32 yawRadians{};
    LocalPlacementLegality legality = LocalPlacementLegality::Unchecked;
    container::Span<const render::TerrainBibFootprintInput> obstructions{};
};

// Session-owned local UI state. It is deliberately outside ECS, lockstep,
// replay commands and save data: only the final Build click becomes a
// GameCommand. A future legality controller supplies its explicit result and
// obstruction footprints; this class never scans live STRUCTURE entities.
class LocalPlacementPresentationState final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;
    [[nodiscard]] bool begin(
        uint64_t presentationEpoch, uint64_t animationStartTick,
        ObjectId sourceObject,
        const game::ThingTemplate& product,
        LocalPlacementBackendKind backend = LocalPlacementBackendKind::Build,
        CommandActivationContext activation = {});
    [[nodiscard]] bool updatePose(
        render::RenderVector position, math::q32_32 yawRadians) noexcept;
    [[nodiscard]] bool publishLegality(
        LocalPlacementLegality legality,
        container::Span<const render::TerrainBibFootprintInput> obstructions);
    // Atomically replaces the active planned poses. Renderer-local identities
    // are stable by tile ordinal for this placement even when the sequence
    // changes 1 -> N -> 1 -> N. An empty sequence is rejected so the legacy
    // single-point snapshot remains available while placement is active.
    [[nodiscard]] bool replaceTiles(
        container::Span<const LocalPlacementPreviewTileInput> tiles);
    [[nodiscard]] bool replaceLineTiles(
        container::Span<const LocalPlacementPreviewTileInput> tiles,
        CommandPosition lineEndPosition);
    // A non-zero identity turns cancellation into a generation-checked
    // operation. Main-thread input may be one projection behind the logic
    // owner; an Escape meant for placement A must not cancel replacement B.
    [[nodiscard]] bool cancel(
        render::RenderEntityId expectedPreviewIdentity = 0) noexcept;

    void appendTerrainBibs(
        container::Vector<render::TerrainBibRenderData>& output) const;

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] LocalPlacementPreviewSnapshot snapshot() const;
    [[nodiscard]] container::Vector<LocalPlacementPreviewSnapshot>
        snapshots() const;

private:
    struct TileState final {
        render::RenderEntityId previewIdentity = 0;
        CommandPosition position;
        math::q32_32 yawRadians{};
        bool hasPose = false;
        LocalPlacementLegality legality = LocalPlacementLegality::Unchecked;
        container::Vector<render::TerrainBibFootprintInput> obstructions;
    };

    [[nodiscard]] render::RenderEntityId allocatePreviewIdentity() noexcept;
    [[nodiscard]] LocalPlacementPreviewSnapshot snapshot(
        const TileState& tile) const;
    [[nodiscard]] bool replaceTilesImpl(
        container::Span<const LocalPlacementPreviewTileInput> tiles,
        bool hasLineEndPosition, CommandPosition lineEndPosition);

    uint64_t m_presentationEpoch = 0;
    uint64_t m_animationStartTick = 0;
    uint64_t m_nextPreviewOrdinal = 0;
    ObjectId m_sourceObject = INVALID_OBJECT_ID;
    container::String m_objectType;
    float m_geometryMajorRadius = 0.0f;
    float m_geometryMinorRadius = 0.0f;
    bool m_geometryIsBox = false;
    float m_factoryExitWidth = 0.0f;
    float m_factoryExtraBibWidth = 0.0f;
    bool m_active = false;
    LocalPlacementBackendKind m_backend = LocalPlacementBackendKind::Build;
    container::Vector<TileState> m_tiles;
    CommandPosition m_lineEndPosition;
    bool m_hasLineEndPosition = false;
    CommandActivationContext m_activation;
    // Identity slots survive sequence shrink and are cleared only when the
    // placement, reset, or presentation epoch ends.
    container::Vector<render::RenderEntityId> m_tileIdentities;
};

} // namespace engine::selection
