#pragma once

#include "core/container/container_types.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ObjectKindOf.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {

// Client terrain render IDs reserve bit 63 for their namespace, bits 32..62
// for a Draw channel and bits 0..31 for the store-local object ID.
inline constexpr uint32_t kClientTerrainMaximumChannelIndex = 0x7fffffffu;

// Map-authored decoration which RefCode deliberately does not materialize as
// an Object.  The session owns these value records; TerrainVisual/DX12 owns
// only resources prepared from the detached render snapshot.
enum class ClientTerrainObjectKind : uint8_t {
    OptimizedTree,
    Prop,
    FluffProp,
};

enum class ClientTerrainImportDisposition : uint8_t {
    AuthoritativeObject,
    ClientTerrainObject,
    DisabledDecoration,
};

struct ClientTerrainImportPolicy final {
    bool showTrees = true;
    bool multiplayer = false;
};

// Pure version of GameLogic's map-import split.  PROP is always client-side;
// optimized trees are client-side when enabled (and forced on in MP); a
// non-fence CLEARED_BY_BUILD object becomes client fluff in multiplayer.
// Single-player import is full-detail and never changes state ownership from
// a quality/LOD option.
[[nodiscard]] ClientTerrainImportDisposition classifyClientTerrainObject(
    const game::ObjectKindOfMask& kindOfMask,
    math::q32_32 fenceWidthFixed,
    const ClientTerrainImportPolicy& policy) noexcept;
[[nodiscard]] std::optional<ClientTerrainObjectKind>
clientTerrainObjectKind(const game::ObjectKindOfMask& kindOfMask,
                        math::q32_32 fenceWidthFixed) noexcept;

struct ClientTerrainVisualChannel final {
    uint32_t sourceChannelIndex = 0;
    container::String modelAsset;
    container::String animationState;
    game::ModelAnimationMode animationMode = game::ModelAnimationMode::Loop;
    container::Vector<game::ModelSubObjectVisibility> subObjectVisibility;
    bool receivesDynamicLights = true;
};

// Detached W3D object-space bounds resolved once during map import. The store
// never retains an asset/cache pointer; scale is applied while compiling the
// value-only object definition.
struct ClientTerrainModelBounds final {
    float boundingRadius = 0.0f;
    float shadowSize = 0.0f;
};

enum class ClientTerrainTreeState : uint8_t {
    Upright,
    Falling,
    Down,
    Stump,
    Removed,
};

struct ClientTerrainObjectDefinition {
    uint64_t sourceRecordIndex = 0;
    container::String templateName;
    ClientTerrainObjectKind kind = ClientTerrainObjectKind::Prop;
    math::vec3 position{};
    float yawRadians = 0.0f;
    float scale = 1.0f;
    float boundingRadius = 1.0f;
    // RefCode removes buffered props/trees using a deliberately inflated
    // cylinder around their render bound. Preserve that admission radius in
    // the client owner instead of adding these decorations to gameplay broad
    // phase merely to support construction clearing.
    float constructionClearRadius = 5.0f;
    // GeometryInfo collision first compares the two base-Z intervals.  This
    // is the model/template-derived height of the temporary client-side
    // cylinder used by construction clearing, not a gameplay broad-phase
    // component.
    float constructionClearHeight = 5.0f;
    game::ModelConditionMask modelConditions;
    game::ThingShadowTemplate shadow;
    container::Vector<ClientTerrainVisualChannel> visuals;
    container::String stumpTemplateName;
    container::String stumpModelAsset;
    float stumpScale = 1.0f;
    float stumpBoundingRadius = 1.0f;
    // W3DTreeDraw owns a replacement texture atlas independently from the
    // W3D model name.  Keep it value-only even though the ordinary static W3D
    // path currently resolves the model's material first; a future tree
    // batching backend must not need to reopen ModuleData.
    container::String treeTextureAsset;
    container::String toppleFxList;
    container::String bounceFxList;
    // INI::parseDurationUnsignedInt converts milliseconds to integral 30 Hz
    // logic frames with ceil. Keep the compiled value in that native unit so
    // sub-frame authored values never acquire wall-clock-dependent rounding.
    uint32_t moveOutwardFrames = 1;
    uint32_t moveInwardFrames = 1;
    float moveOutwardDistanceFactor = 1.0f;
    float darkeningFactor = 0.0f;
    float sinkDistance = 20.0f;
    uint32_t sinkFrames = 300;
    float initialVelocityPercent = 0.2f;
    float initialAccelerationPercent = 0.01f;
    float bounceVelocityPercent = 0.3f;
    float minimumToppleSpeed = 0.5f;
    bool treeSwayEnabled = false;
    bool treeCanTopple = false;
    bool killTreeWhenToppled = true;
    bool treeShadowEnabled = false;
};

struct ClientTerrainConstructionFootprint final {
    math::vec3 center{};
    float yawRadians = 0.0f;
    float halfExtentX = 0.0f;
    float halfExtentY = 0.0f;
    float radius = 0.0f;
    // GeometryInfo positions are at the base of their Z interval.
    float height = 0.0f;
    bool orientedBox = false;
};

struct ClientTerrainObject final : ClientTerrainObjectDefinition {
    uint32_t id = 0;
    ClientTerrainTreeState treeState = ClientTerrainTreeState::Upright;
    math::vec3 toppleDirection{1.0f, 0.0f, 0.0f};
    float toppleRadians = 0.0f;
    float angularVelocity = 0.0f;
    float angularAcceleration = 0.0f;
    float sinkOffset = 0.0f;
    float sinkElapsedFrames = 0.0f;
    // W3DTreeBuffer bends an upright tree away from a moving unit. The
    // amount follows its authored outward/inward frame ramp; direction is
    // world-space so model yaw does not accidentally rotate the push twice.
    math::vec3 pushAsideDirection{};
    float pushAsideAmount = 0.0f;
    float pushAsideDeltaPerFrame = 0.0f;
    uint64_t pushAsideSource = 0;
    uint64_t pushAsideLastFrame = 0;
    // OBJECTSHROUD_FOGGED freezes a falling optimized tree. Fully shrouded
    // does not: it may finish out of sight and be down when revealed.
    bool toppleFrozenByFog = false;
    bool currentlyFogged = false;
};

// Only mutable overlay data belongs in a save/replay checkpoint. Static model,
// transform and classification are deterministically rebuilt from the frozen
// map/content/LOD policy for the new presentation epoch, then this overlay is
// applied by sourceRecordIndex. No ObjectId, ECS entity or GPU handle appears
// in the format.
struct ClientTerrainObjectMutation final {
    uint64_t sourceRecordIndex = 0;
    ClientTerrainTreeState treeState = ClientTerrainTreeState::Upright;
    math::vec3 toppleDirection{1.0f, 0.0f, 0.0f};
    float toppleRadians = 0.0f;
    float angularVelocity = 0.0f;
    float angularAcceleration = 0.0f;
    float sinkOffset = 0.0f;
    float sinkElapsedFrames = 0.0f;
    math::vec3 pushAsideDirection{};
    float pushAsideAmount = 0.0f;
    float pushAsideDeltaPerFrame = 0.0f;
    bool toppleFrozenByFog = false;
};

struct ClientTerrainObjectPersistentState final {
    static constexpr uint32_t kVersion = 6;
    uint32_t version = kVersion;
    // Stable across independent sessions which loaded the same map bytes,
    // frozen content/templates, initial conditions and LOD policy. Runtime
    // TerrainMap layout revisions are deliberately excluded.
    uint64_t contentIdentity = 0;
    container::Vector<ClientTerrainObjectMutation> mutations;
};

enum class ClientTerrainFxEventKind : uint8_t {
    Topple,
    Bounce,
};

struct ClientTerrainFxEvent final {
    ClientTerrainFxEventKind kind = ClientTerrainFxEventKind::Topple;
    uint64_t sourceRecordIndex = 0;
    container::String fxListName;
    math::vec3 position{};
};

// Value-only equivalent of W3DTreeBuffer::unitMoved. GameSession produces
// this only after an authoritative transform changed during the confirmed
// tick. Geometry and CrusherLevel are frozen template/component values; no
// ECS entity, Object pointer or renderer handle crosses into the store.
struct ClientTerrainMovingUnit final {
    uint64_t source = 0;
    math::vec3 position{};
    math::vec3 forward{1.0f, 0.0f, 0.0f};
    float collisionRadius = 0.0f;
    uint8_t crusherLevel = 0;
};

struct ClientTerrainWaveFront final {
    math::vec3 center{};
    float yawRadians = 0.0f;
    float ySize = 0.0f;
    float bendMagnitude = 0.0f;
    float damageRadius = 0.0f;
    float toppleForce = 0.0f;
    float preferredHeight = 0.0f;
};

class ClientTerrainObjectStore final {
public:
    void beginMapRebuild(uint64_t presentationEpoch,
                         uint64_t contentIdentity) noexcept;
    [[nodiscard]] bool add(ClientTerrainObjectDefinition definition);
    void clear() noexcept;

    [[nodiscard]] uint64_t presentationEpoch() const noexcept {
        return m_presentationEpoch;
    }
    [[nodiscard]] uint64_t contentIdentity() const noexcept {
        return m_contentIdentity;
    }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }
    [[nodiscard]] container::Span<const ClientTerrainObject> objects() const noexcept {
        return m_objects;
    }

    [[nodiscard]] size_t removeForConstruction(
        const ClientTerrainConstructionFootprint& footprint) noexcept;
    [[nodiscard]] bool beginTreeTopple(
        uint32_t id, math::vec3 direction,
        float authoredToppleSpeed = 0.0f) noexcept;
    // Returns the number of trees which began toppling or push-aside. The
    // original tree buffer uses an approximate seven-unit tree radius.
    [[nodiscard]] size_t unitMoved(
        const ClientTerrainMovingUnit& unit,
        uint64_t confirmedFrame) noexcept;
    [[nodiscard]] size_t applyWaveFront(
        const ClientTerrainWaveFront& wave) noexcept;
    [[nodiscard]] bool updateTreeTopple(uint32_t id, float radians,
                                        bool settled, float sinkOffset = 0.0f) noexcept;
    [[nodiscard]] bool setTreeFogged(uint32_t id, bool fogged) noexcept;
    // Advances the renderer-owned W3DTreeDraw state at the confirmed game
    // cadence. All angular and sink timing remains in original 30 Hz frames.
    void advanceTreeLifecycles(float logicFrames) noexcept;
    [[nodiscard]] container::Vector<ClientTerrainFxEvent> takeFxEvents();
    [[nodiscard]] bool replaceTreeWithStump(uint32_t id) noexcept;
    [[nodiscard]] bool remove(uint32_t id) noexcept;

    [[nodiscard]] ClientTerrainObjectPersistentState capturePersistentState() const;
    // Restore always resets live mutations to the rebuilt map baseline first.
    // A mismatched content identity/version is rejected atomically, making a stale
    // save unable to mutate a replacement map/epoch. Ordinary replay does not
    // restore this overlay and instead rebuilds the map baseline at tick zero.
    [[nodiscard]] bool restorePersistentState(
        const ClientTerrainObjectPersistentState& state) noexcept;

private:
    [[nodiscard]] ClientTerrainObject* find(uint32_t id) noexcept;
    // Same contract as beginTreeTopple, on an already-resolved object. Callers
    // sweeping m_objects use this so a crush wave does not rescan every object
    // once per toppled tree.
    [[nodiscard]] bool beginTreeToppleOn(ClientTerrainObject& object,
                                        math::vec3 direction,
                                        float authoredToppleSpeed) noexcept;
    void changed() noexcept;

    uint64_t m_presentationEpoch = 0;
    uint64_t m_contentIdentity = 0;
    uint64_t m_revision = 0;
    uint32_t m_nextId = 1;
    container::Vector<ClientTerrainObject> m_objects;
    container::Vector<ClientTerrainFxEvent> m_fxEvents;
};

[[nodiscard]] uint64_t clientTerrainContentIdentity(
    uint32_t mapCrc, uint32_t mapSize,
    uint64_t simulationContentFingerprint,
    uint64_t frozenTemplateIdentity,
    game::ModelConditionMask initialConditions,
    const ClientTerrainImportPolicy& policy) noexcept;

[[nodiscard]] std::optional<ClientTerrainObjectDefinition>
compileClientTerrainObjectDefinition(
    uint64_t sourceRecordIndex,
    const game::ThingTemplate& templateData,
    ClientTerrainObjectKind kind,
    math::vec3 position,
    float yawRadians,
    game::ModelConditionMask conditions,
    std::optional<ClientTerrainModelBounds> modelBounds = std::nullopt);

[[nodiscard]] container::String clientTerrainStumpTemplateName(
    const game::ThingTemplate& templateData);

[[nodiscard]] container::String clientTerrainPrimaryModel(
    const game::ThingTemplate& templateData,
    game::ModelConditionMask conditions);

} // namespace engine
