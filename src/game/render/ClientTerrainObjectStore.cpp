#include "ClientTerrainObjectStore.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] bool equalsInsensitive(container::StringView left,
                                     container::StringView right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const container::String* moduleValue(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto value = module.values.rbegin(); value != module.values.rend(); ++value) {
        if (equalsInsensitive(value->first, key)) return &value->second;
    }
    const auto property = std::find_if(
        module.properties.begin(), module.properties.end(),
        [key](const auto& pair) { return equalsInsensitive(pair.first, key); });
    return property != module.properties.end() ? &property->second : nullptr;
}

[[nodiscard]] bool parseBool(container::StringView value,
                             bool fallback) noexcept {
    if (equalsInsensitive(value, "yes") || equalsInsensitive(value, "true") ||
        value == "1") return true;
    if (equalsInsensitive(value, "no") || equalsInsensitive(value, "false") ||
        value == "0") return false;
    return fallback;
}

[[nodiscard]] std::optional<float> parseFiniteFloat(
    container::StringView value) noexcept {
    return game::parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "ClientUpdate",
        .field = "Real"});
}

[[nodiscard]] float parsePercentOr(
    container::StringView value, float fallback) noexcept {
    const std::optional<float> parsed = parseFiniteFloat(value);
    if (!parsed) return fallback;
    return value.find('%') != container::StringView::npos
        ? *parsed * 0.01f : *parsed;
}

[[nodiscard]] uint32_t parseDurationFramesOr(
    container::StringView value, uint32_t fallback) noexcept {
    const std::optional<float> parsed = parseFiniteFloat(value);
    if (!parsed || *parsed < 0.0f) {
        return fallback;
    }
    constexpr double kLogicFramesPerMillisecond = 30.0 / 1000.0;
    const double frames = std::ceil(
        static_cast<double>(*parsed) * kLogicFramesPerMillisecond);
    if (frames > static_cast<double>(
            std::numeric_limits<uint32_t>::max())) {
        return fallback;
    }
    return static_cast<uint32_t>(frames);
}

[[nodiscard]] bool finiteDefinition(
    const ClientTerrainObjectDefinition& value) noexcept {
    return std::isfinite(value.position.x()) &&
        std::isfinite(value.position.y()) &&
        std::isfinite(value.position.z()) &&
        std::isfinite(value.yawRadians) &&
        std::isfinite(value.scale) && value.scale > 0.0f &&
        std::isfinite(value.boundingRadius) && value.boundingRadius >= 0.0f &&
        std::isfinite(value.stumpScale) && value.stumpScale > 0.0f &&
        std::isfinite(value.stumpBoundingRadius) &&
        value.stumpBoundingRadius >= 0.0f &&
        std::isfinite(value.constructionClearRadius) &&
        value.constructionClearRadius >= 0.0f &&
        std::isfinite(value.constructionClearHeight) &&
        value.constructionClearHeight >= 0.0f &&
        std::isfinite(value.sinkDistance) && value.sinkDistance >= 0.0f &&
        std::isfinite(value.moveOutwardDistanceFactor) &&
        value.moveOutwardDistanceFactor >= 0.0f &&
        std::isfinite(value.darkeningFactor) &&
        std::isfinite(value.initialVelocityPercent) &&
        std::isfinite(value.initialAccelerationPercent) &&
        std::isfinite(value.bounceVelocityPercent) &&
        std::isfinite(value.minimumToppleSpeed) &&
        value.minimumToppleSpeed >= 0.0f;
}

[[nodiscard]] bool constructionIntersects(
    const ClientTerrainObject& object,
    const ClientTerrainConstructionFootprint& footprint) noexcept {
    const float dx = object.position.x() - footprint.center.x();
    const float dy = object.position.y() - footprint.center.y();
    const float objectTop = object.position.z() +
        std::max(0.0f, object.constructionClearHeight);
    const float footprintTop = footprint.center.z() +
        std::max(0.0f, footprint.height);
    if (footprintTop < object.position.z() ||
        footprint.center.z() > objectTop) {
        return false;
    }
    const float objectRadius = std::max(0.0f, object.constructionClearRadius);
    if (!footprint.orientedBox) {
        const float admitted = std::max(0.0f, footprint.radius) + objectRadius;
        return dx * dx + dy * dy <= admitted * admitted;
    }
    const float sine = std::sin(-footprint.yawRadians);
    const float cosine = std::cos(-footprint.yawRadians);
    const float localX = cosine * dx - sine * dy;
    const float localY = sine * dx + cosine * dy;
    const float nearestX = std::clamp(
        localX, -std::max(0.0f, footprint.halfExtentX),
        std::max(0.0f, footprint.halfExtentX));
    const float nearestY = std::clamp(
        localY, -std::max(0.0f, footprint.halfExtentY),
        std::max(0.0f, footprint.halfExtentY));
    const float separatedX = localX - nearestX;
    const float separatedY = localY - nearestY;
    return separatedX * separatedX + separatedY * separatedY <=
        objectRadius * objectRadius;
}

[[nodiscard]] ClientTerrainVisualChannel compileVisual(
    const game::ModelDrawVisualChannel& source,
    uint32_t sourceChannelIndex,
    game::ModelConditionMask conditions) {
    ClientTerrainVisualChannel result{
        .sourceChannelIndex = sourceChannelIndex,
        .modelAsset = source.defaultModel,
        .receivesDynamicLights = source.receivesDynamicLights,
    };
    if (!source.conditionVisuals.empty()) {
        const size_t selected = game::selectModelConditionVisualRuleIndex(
            source, conditions);
        if (selected < source.conditionVisuals.size()) {
            const game::ModelConditionVisualRule& rule =
                source.conditionVisuals[selected];
            result.modelAsset = rule.model;
            result.animationState = rule.animation;
            result.animationMode = rule.animationMode;
            result.subObjectVisibility = rule.subObjectVisibility;
        }
    }
    return result;
}

} // namespace

uint64_t clientTerrainContentIdentity(
    uint32_t mapCrc, uint32_t mapSize,
    uint64_t simulationContentFingerprint,
    uint64_t frozenTemplateIdentity,
    game::ModelConditionMask initialConditions,
    const ClientTerrainImportPolicy& policy) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) noexcept {
        for (size_t index = 0; index < sizeof(value); ++index) {
            hash ^= (value >> (index * 8u)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix(mapCrc);
    mix(mapSize);
    mix(simulationContentFingerprint);
    mix(frozenTemplateIdentity);
    for (const uint64_t word : initialConditions.words) mix(word);
    mix(policy.showTrees ? 1u : 0u);
    mix(policy.multiplayer ? 1u : 0u);
    return hash != 0 ? hash : 1u;
}

ClientTerrainImportDisposition classifyClientTerrainObject(
    const game::ObjectKindOfMask& kindOfMask, math::q32_32 fenceWidthFixed,
    const ClientTerrainImportPolicy& policy) noexcept {
    const bool shrubbery = game::objectHasKind(
        kindOfMask, game::ObjectKindOf::Shrubbery);
    const bool optimizedTree = game::objectHasKind(
        kindOfMask, game::ObjectKindOf::OptimizedTree);
    if (shrubbery && !policy.showTrees && !policy.multiplayer) {
        return ClientTerrainImportDisposition::DisabledDecoration;
    }
    if (optimizedTree) {
        return ClientTerrainImportDisposition::ClientTerrainObject;
    }
    if (game::objectHasKind(kindOfMask, game::ObjectKindOf::Prop)) {
        return ClientTerrainImportDisposition::ClientTerrainObject;
    }
    const bool fluff = game::objectHasKind(
        kindOfMask, game::ObjectKindOf::ClearedByBuild) &&
        fenceWidthFixed == math::q32_32{};
    if (!fluff) return ClientTerrainImportDisposition::AuthoritativeObject;

    return policy.multiplayer
        ? ClientTerrainImportDisposition::ClientTerrainObject
        : ClientTerrainImportDisposition::AuthoritativeObject;
}

std::optional<ClientTerrainObjectKind> clientTerrainObjectKind(
    const game::ObjectKindOfMask& kindOfMask,
    math::q32_32 fenceWidthFixed) noexcept {
    if (game::objectHasKind(kindOfMask, game::ObjectKindOf::OptimizedTree)) {
        return ClientTerrainObjectKind::OptimizedTree;
    }
    if (game::objectHasKind(kindOfMask, game::ObjectKindOf::Prop)) {
        return ClientTerrainObjectKind::Prop;
    }
    if (game::objectHasKind(kindOfMask, game::ObjectKindOf::ClearedByBuild) &&
        fenceWidthFixed == math::q32_32{}) {
        return ClientTerrainObjectKind::FluffProp;
    }
    return std::nullopt;
}

void ClientTerrainObjectStore::beginMapRebuild(
    uint64_t presentationEpoch, uint64_t contentIdentity) noexcept {
    m_presentationEpoch = presentationEpoch;
    m_contentIdentity = contentIdentity;
    m_nextId = 1;
    m_objects.clear();
    m_fxEvents.clear();
    changed();
}

bool ClientTerrainObjectStore::add(ClientTerrainObjectDefinition definition) {
    if (m_nextId == 0 || definition.templateName.empty() ||
        definition.visuals.empty() || !finiteDefinition(definition)) {
        return false;
    }
    for (size_t channelIndex = 0;
         channelIndex < definition.visuals.size(); ++channelIndex) {
        const uint32_t sourceChannelIndex =
            definition.visuals[channelIndex].sourceChannelIndex;
        if (sourceChannelIndex > kClientTerrainMaximumChannelIndex) {
            return false;
        }
        const auto duplicateChannel = std::find_if(
            definition.visuals.begin(),
            definition.visuals.begin() +
                static_cast<std::ptrdiff_t>(channelIndex),
            [sourceChannelIndex](const ClientTerrainVisualChannel& visual) {
                return visual.sourceChannelIndex == sourceChannelIndex;
            });
        if (duplicateChannel != definition.visuals.begin() +
                static_cast<std::ptrdiff_t>(channelIndex)) {
            return false;
        }
    }
    const auto duplicate = std::find_if(
        m_objects.begin(), m_objects.end(),
        [&definition](const ClientTerrainObject& value) {
            return value.sourceRecordIndex == definition.sourceRecordIndex;
        });
    if (duplicate != m_objects.end()) return false;
    ClientTerrainObject value;
    static_cast<ClientTerrainObjectDefinition&>(value) = std::move(definition);
    value.id = m_nextId++;
    m_objects.push_back(std::move(value));
    changed();
    return true;
}

void ClientTerrainObjectStore::clear() noexcept {
    m_presentationEpoch = 0;
    m_contentIdentity = 0;
    m_nextId = 1;
    m_objects.clear();
    m_fxEvents.clear();
    changed();
}

size_t ClientTerrainObjectStore::removeForConstruction(
    const ClientTerrainConstructionFootprint& footprint) noexcept {
    if (!std::isfinite(footprint.center.x()) ||
        !std::isfinite(footprint.center.y()) ||
        !std::isfinite(footprint.center.z()) ||
        !std::isfinite(footprint.height) || footprint.height < 0.0f ||
        !std::isfinite(footprint.yawRadians)) return 0;
    size_t removed = 0;
    for (ClientTerrainObject& object : m_objects) {
        if (object.treeState == ClientTerrainTreeState::Removed ||
            !constructionIntersects(object, footprint)) continue;
        object.treeState = ClientTerrainTreeState::Removed;
        ++removed;
    }
    if (removed != 0) changed();
    return removed;
}

bool ClientTerrainObjectStore::beginTreeTopple(
    uint32_t id, math::vec3 direction,
    float authoredToppleSpeed) noexcept {
    ClientTerrainObject* object = find(id);
    return object &&
        beginTreeToppleOn(*object, direction, authoredToppleSpeed);
}

bool ClientTerrainObjectStore::beginTreeToppleOn(
    ClientTerrainObject& object, math::vec3 direction,
    float authoredToppleSpeed) noexcept {
    if (object.kind != ClientTerrainObjectKind::OptimizedTree ||
        !object.treeCanTopple ||
        object.treeState != ClientTerrainTreeState::Upright ||
        !std::isfinite(direction.x()) || !std::isfinite(direction.y()) ||
        !std::isfinite(authoredToppleSpeed)) {
        return false;
    }
    const float lengthSquared = direction.x() * direction.x() +
        direction.y() * direction.y();
    if (lengthSquared <= std::numeric_limits<float>::epsilon()) return false;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    object.toppleDirection = {
        direction.x() * inverseLength, direction.y() * inverseLength, 0.0f};
    object.toppleRadians = 0.0f;
    // RefCode W3DTreeBuffer::applyTopplingForce raises any speed below the
    // template MinimumToppleSpeed, because an exactly-zero speed would leave
    // the tree upright forever with the sway update already dead.
    const float toppleSpeed = std::max(
        std::max(0.0f, authoredToppleSpeed), object.minimumToppleSpeed);
    object.angularVelocity =
        toppleSpeed * object.initialVelocityPercent;
    object.angularAcceleration =
        toppleSpeed * object.initialAccelerationPercent;
    object.sinkOffset = 0.0f;
    object.sinkElapsedFrames = 0.0f;
    object.pushAsideDirection = {};
    object.pushAsideAmount = 0.0f;
    object.pushAsideDeltaPerFrame = 0.0f;
    object.pushAsideSource = 0;
    object.toppleFrozenByFog = false;
    object.treeState = ClientTerrainTreeState::Falling;
    if (!object.toppleFxList.empty()) {
        m_fxEvents.push_back({
            .kind = ClientTerrainFxEventKind::Topple,
            .sourceRecordIndex = object.sourceRecordIndex,
            .fxListName = object.toppleFxList,
            .position = object.position,
        });
    }
    changed();
    return true;
}

size_t ClientTerrainObjectStore::unitMoved(
    const ClientTerrainMovingUnit& unit,
    uint64_t confirmedFrame) noexcept {
    constexpr float kApproximateTreeRadius = 7.0f;
    if (unit.source == 0 || confirmedFrame == 0 ||
        !std::isfinite(unit.position.x()) ||
        !std::isfinite(unit.position.y()) ||
        !std::isfinite(unit.position.z()) ||
        !std::isfinite(unit.forward.x()) ||
        !std::isfinite(unit.forward.y()) ||
        !std::isfinite(unit.collisionRadius) ||
        unit.collisionRadius < 0.0f) {
        return 0;
    }
    const float forwardLengthSquared =
        unit.forward.x() * unit.forward.x() +
        unit.forward.y() * unit.forward.y();
    if (forwardLengthSquared <= std::numeric_limits<float>::epsilon()) {
        return 0;
    }
    const float inverseForwardLength = 1.0f /
        std::sqrt(forwardLengthSquared);
    const float forwardX = unit.forward.x() * inverseForwardLength;
    const float forwardY = unit.forward.y() * inverseForwardLength;
    const float admittedRadius =
        unit.collisionRadius + kApproximateTreeRadius;
    const float admittedRadiusSquared = admittedRadius * admittedRadius;
    size_t affected = 0;
    for (ClientTerrainObject& object : m_objects) {
        if (object.kind != ClientTerrainObjectKind::OptimizedTree ||
            object.treeState != ClientTerrainTreeState::Upright) {
            continue;
        }
        const math::vec3 delta = object.position - unit.position;
        if (delta.length_sq() >= admittedRadiusSquared) continue;
        if (unit.crusherLevel > 1 && object.treeCanTopple) {
            // Parity: RefCode W3DTreeBuffer::unitMoved passes a literal 0
            // topple speed for the optimized tree buffer
            // (W3DTreeBuffer.cpp:1198), which applyTopplingForce then raises
            // to the template MinimumToppleSpeed, so every crushed tree does
            // fall at the minimum speed in the original as well. Crusher-speed
            // scaling (|physics velocity| per logic frame) belongs to
            // ToppleUpdate::onCollide, i.e. the logic-side topple of real
            // Objects, which lives in ObjectSimulationPhysics and stays in
            // fixed point. Spell the 0 out rather than leaning on the default
            // so the choice is not mistaken for an oversight.
            constexpr float kCrushToppleSpeed = 0.0f;
            if (beginTreeToppleOn(object, delta, kCrushToppleSpeed)) {
                ++affected;
            }
            continue;
        }
        if (object.moveOutwardFrames <= 1) continue;
        const uint64_t previousFrame = object.pushAsideLastFrame;
        object.pushAsideLastFrame = confirmedFrame;
        if (object.pushAsideSource == unit.source &&
            confirmedFrame >= previousFrame &&
            confirmedFrame - previousFrame < 3) {
            continue;
        }
        if (object.pushAsideAmount != 0.0f ||
            object.pushAsideDeltaPerFrame != 0.0f) {
            continue;
        }
        object.pushAsideSource = unit.source;
        const float side = forwardX * delta.y() - forwardY * delta.x();
        object.pushAsideDirection = side > 0.0f
            ? math::vec3{-forwardY, forwardX, 0.0f}
            : math::vec3{forwardY, -forwardX, 0.0f};
        object.pushAsideDeltaPerFrame =
            1.0f / static_cast<float>(object.moveOutwardFrames);
        ++affected;
    }
    if (affected != 0) changed();
    return affected;
}

size_t ClientTerrainObjectStore::applyWaveFront(
    const ClientTerrainWaveFront& wave) noexcept {
    if (!std::isfinite(wave.center.x()) ||
        !std::isfinite(wave.center.y()) ||
        !std::isfinite(wave.yawRadians) ||
        !std::isfinite(wave.ySize) ||
        !std::isfinite(wave.bendMagnitude) ||
        !std::isfinite(wave.damageRadius) ||
        !std::isfinite(wave.toppleForce) ||
        !std::isfinite(wave.preferredHeight) || wave.ySize <= 0.0f ||
        wave.damageRadius < 0.0f || wave.toppleForce <= 0.0f) {
        return 0;
    }
    struct PendingTopple final {
        uint32_t id = 0;
        math::vec3 direction{};
    };
    container::Vector<PendingTopple> pending;
    const float sine = std::sin(wave.yawRadians);
    const float cosine = std::cos(wave.yawRadians);
    const float halfY = wave.ySize * 0.5f;
    for (const ClientTerrainObject& object : m_objects) {
        if (object.kind != ClientTerrainObjectKind::OptimizedTree ||
            !object.treeCanTopple ||
            object.treeState != ClientTerrainTreeState::Upright ||
            object.position.z() > wave.preferredHeight) {
            continue;
        }
        const float dx = object.position.x() - wave.center.x();
        const float dy = object.position.y() - wave.center.y();
        const float localX = dx * cosine + dy * sine;
        const float localY = -dx * sine + dy * cosine;
        if (std::abs(localY) > halfY) continue;
        const float shapeX = std::abs(wave.bendMagnitude) > 0.0001f
            ? -(localY * localY) / wave.bendMagnitude : 0.0f;
        if (localX > shapeX ||
            shapeX - localX > wave.damageRadius) continue;
        const float nearestX = wave.center.x() +
            shapeX * cosine - localY * sine;
        const float nearestY = wave.center.y() +
            shapeX * sine + localY * cosine;
        pending.push_back({
            .id = object.id,
            .direction = {
                object.position.x() - nearestX,
                object.position.y() - nearestY, 0.0f},
        });
    }
    size_t affected = 0;
    for (const PendingTopple& topple : pending) {
        if (beginTreeTopple(
                topple.id, topple.direction, wave.toppleForce)) {
            ++affected;
        }
    }
    return affected;
}

bool ClientTerrainObjectStore::setTreeFogged(
    uint32_t id, bool fogged) noexcept {
    ClientTerrainObject* object = find(id);
    if (!object || object->kind != ClientTerrainObjectKind::OptimizedTree ||
        object->currentlyFogged == fogged) {
        return false;
    }
    object->currentlyFogged = fogged;
    // Visibility itself is derived local presentation state. Publish a
    // revision only while it can alter the falling lifecycle.
    if (object->treeState == ClientTerrainTreeState::Falling ||
        object->toppleFrozenByFog) {
        changed();
    }
    return true;
}

bool ClientTerrainObjectStore::updateTreeTopple(
    uint32_t id, float radians, bool settled, float sinkOffset) noexcept {
    ClientTerrainObject* object = find(id);
    if (!object || object->treeState != ClientTerrainTreeState::Falling ||
        !std::isfinite(radians) || !std::isfinite(sinkOffset)) return false;
    object->toppleRadians = std::clamp(radians, 0.0f, math::PI * 0.5f);
    object->sinkOffset = std::clamp(
        sinkOffset, 0.0f, object->sinkDistance);
    if (settled) {
        object->treeState = ClientTerrainTreeState::Down;
        object->angularVelocity = 0.0f;
        object->angularAcceleration = 0.0f;
        object->sinkElapsedFrames = 0.0f;
    }
    changed();
    return true;
}

void ClientTerrainObjectStore::advanceTreeLifecycles(
    float logicFrames) noexcept {
    if (!std::isfinite(logicFrames) || logicFrames < 0.0f) return;
    constexpr float kAngularLimit = math::PI * 0.5f - math::PI / 64.0f;
    constexpr float kBounceStopVelocity = 0.01f;
    bool mutated = false;
    for (ClientTerrainObject& object : m_objects) {
        if (object.treeState == ClientTerrainTreeState::Falling &&
            logicFrames > 0.0f) {
            if (object.currentlyFogged) {
                if (!object.toppleFrozenByFog) {
                    object.toppleFrozenByFog = true;
                    mutated = true;
                }
                continue;
            }
            if (object.toppleFrozenByFog) {
                // W3DTreeBuffer snaps a tree which finished unseen directly
                // to its angular limit. KillWhenFinishedToppling then removes
                // it on the following confirmed update rather than replaying
                // the hidden fall when the cell becomes clear again.
                object.toppleFrozenByFog = false;
                object.toppleRadians = kAngularLimit;
                object.angularVelocity = 0.0f;
                object.angularAcceleration = 0.0f;
                object.treeState = ClientTerrainTreeState::Down;
                if (object.killTreeWhenToppled) {
                    object.sinkElapsedFrames =
                        static_cast<float>(object.sinkFrames);
                }
                mutated = true;
                continue;
            }
            float angularStep = object.angularVelocity * logicFrames;
            if (object.toppleRadians + angularStep > kAngularLimit) {
                angularStep = kAngularLimit - object.toppleRadians;
            }
            object.toppleRadians = std::clamp(
                object.toppleRadians + angularStep, 0.0f, kAngularLimit);
            if (object.toppleRadians >= kAngularLimit &&
                object.angularVelocity > 0.0f) {
                object.angularVelocity *= -object.bounceVelocityPercent;
                if (std::abs(object.angularVelocity) <
                    kBounceStopVelocity) {
                    object.angularVelocity = 0.0f;
                    object.angularAcceleration = 0.0f;
                    object.treeState = ClientTerrainTreeState::Down;
                    object.sinkElapsedFrames = 0.0f;
                } else if (std::abs(object.angularVelocity) >= 0.03f &&
                           !object.bounceFxList.empty()) {
                    const float bounceHeight = 3.0f *
                        std::max(0.0f, object.boundingRadius);
                    m_fxEvents.push_back({
                        .kind = ClientTerrainFxEventKind::Bounce,
                        .sourceRecordIndex = object.sourceRecordIndex,
                        .fxListName = object.bounceFxList,
                        .position = object.position +
                            object.toppleDirection * bounceHeight,
                    });
                }
            } else {
                object.angularVelocity +=
                    object.angularAcceleration * logicFrames;
            }
            mutated = true;
            // RefCode's TOPPLE_FALLING branch is an else-if predecessor of
            // TOPPLE_DOWN. A tree which settles this frame cannot begin
            // sinking until the following confirmed frame.
            continue;
        }
        if (object.treeState == ClientTerrainTreeState::Upright &&
            object.pushAsideDeltaPerFrame != 0.0f && logicFrames > 0.0f) {
            object.pushAsideAmount +=
                object.pushAsideDeltaPerFrame * logicFrames;
            if (object.pushAsideAmount >= 1.0f) {
                object.pushAsideAmount = 1.0f;
                const uint32_t inwardFrames =
                    std::max<uint32_t>(1u, object.moveInwardFrames);
                object.pushAsideDeltaPerFrame =
                    -1.0f / static_cast<float>(inwardFrames);
            } else if (object.pushAsideAmount <= 0.0f) {
                object.pushAsideAmount = 0.0f;
                object.pushAsideDeltaPerFrame = 0.0f;
                object.pushAsideDirection = {};
                object.pushAsideSource = 0;
            }
            mutated = true;
            continue;
        }
        if (object.treeState != ClientTerrainTreeState::Down ||
            !object.killTreeWhenToppled) {
            continue;
        }
        const ClientTerrainTreeState previousState = object.treeState;
        const float previousSinkOffset = object.sinkOffset;
        const float previousSinkElapsed = object.sinkElapsedFrames;
        if (object.sinkElapsedFrames >=
            static_cast<float>(object.sinkFrames)) {
            object.sinkOffset = object.sinkDistance;
            object.treeState = ClientTerrainTreeState::Removed;
        } else if (logicFrames > 0.0f) {
            object.sinkElapsedFrames = std::min(
                static_cast<float>(object.sinkFrames),
                object.sinkElapsedFrames + logicFrames);
            object.sinkOffset = object.sinkFrames == 0
                ? object.sinkDistance
                : object.sinkDistance *
                    (object.sinkElapsedFrames /
                     static_cast<float>(object.sinkFrames));
        }
        mutated = mutated || object.treeState != previousState ||
            object.sinkOffset != previousSinkOffset ||
            object.sinkElapsedFrames != previousSinkElapsed;
    }
    if (mutated) changed();
}

bool ClientTerrainObjectStore::replaceTreeWithStump(uint32_t id) noexcept {
    ClientTerrainObject* object = find(id);
    if (!object || object->kind != ClientTerrainObjectKind::OptimizedTree ||
        object->stumpModelAsset.empty() ||
        object->treeState == ClientTerrainTreeState::Removed) return false;
    object->treeState = ClientTerrainTreeState::Stump;
    object->toppleRadians = 0.0f;
    object->angularVelocity = 0.0f;
    object->angularAcceleration = 0.0f;
    object->sinkOffset = 0.0f;
    object->sinkElapsedFrames = 0.0f;
    object->pushAsideDirection = {};
    object->pushAsideAmount = 0.0f;
    object->pushAsideDeltaPerFrame = 0.0f;
    object->pushAsideSource = 0;
    object->toppleFrozenByFog = false;
    changed();
    return true;
}

bool ClientTerrainObjectStore::remove(uint32_t id) noexcept {
    ClientTerrainObject* object = find(id);
    if (!object || object->treeState == ClientTerrainTreeState::Removed) {
        return false;
    }
    object->treeState = ClientTerrainTreeState::Removed;
    changed();
    return true;
}

ClientTerrainObjectPersistentState
ClientTerrainObjectStore::capturePersistentState() const {
    ClientTerrainObjectPersistentState state;
    state.contentIdentity = m_contentIdentity;
    for (const ClientTerrainObject& object : m_objects) {
        if (object.treeState == ClientTerrainTreeState::Upright &&
            object.pushAsideAmount == 0.0f &&
            object.pushAsideDeltaPerFrame == 0.0f) continue;
        state.mutations.push_back({
            .sourceRecordIndex = object.sourceRecordIndex,
            .treeState = object.treeState,
            .toppleDirection = object.toppleDirection,
            .toppleRadians = object.toppleRadians,
            .angularVelocity = object.angularVelocity,
            .angularAcceleration = object.angularAcceleration,
            .sinkOffset = object.sinkOffset,
            .sinkElapsedFrames = object.sinkElapsedFrames,
            .pushAsideDirection = object.pushAsideDirection,
            .pushAsideAmount = object.pushAsideAmount,
            .pushAsideDeltaPerFrame = object.pushAsideDeltaPerFrame,
            .toppleFrozenByFog = object.toppleFrozenByFog,
        });
    }
    return state;
}

bool ClientTerrainObjectStore::restorePersistentState(
    const ClientTerrainObjectPersistentState& state) noexcept {
    if (state.version != ClientTerrainObjectPersistentState::kVersion ||
        state.contentIdentity == 0 ||
        state.contentIdentity != m_contentIdentity) return false;
    for (size_t mutationIndex = 0;
         mutationIndex < state.mutations.size(); ++mutationIndex) {
        const ClientTerrainObjectMutation& mutation =
            state.mutations[mutationIndex];
        const auto found = std::find_if(
            m_objects.begin(), m_objects.end(),
            [&mutation](const ClientTerrainObject& value) {
                return value.sourceRecordIndex == mutation.sourceRecordIndex;
            });
        const auto duplicate = std::find_if(
            state.mutations.begin(),
            state.mutations.begin() + static_cast<std::ptrdiff_t>(mutationIndex),
            [&mutation](const ClientTerrainObjectMutation& previous) {
                return previous.sourceRecordIndex == mutation.sourceRecordIndex;
            });
        const uint8_t rawState = static_cast<uint8_t>(mutation.treeState);
        const bool treeOnlyState =
            mutation.treeState == ClientTerrainTreeState::Falling ||
            mutation.treeState == ClientTerrainTreeState::Down ||
            mutation.treeState == ClientTerrainTreeState::Stump;
        const bool hasPushAsideMutation =
            mutation.pushAsideAmount != 0.0f ||
            mutation.pushAsideDeltaPerFrame != 0.0f;
        const float toppleDirectionLengthSquared =
            mutation.toppleDirection.x() * mutation.toppleDirection.x() +
            mutation.toppleDirection.y() * mutation.toppleDirection.y();
        const float pushAsideDirectionLengthSquared =
            mutation.pushAsideDirection.x() *
                mutation.pushAsideDirection.x() +
            mutation.pushAsideDirection.y() *
                mutation.pushAsideDirection.y();
        if (found == m_objects.end() ||
            duplicate != state.mutations.begin() +
                static_cast<std::ptrdiff_t>(mutationIndex) ||
            rawState > static_cast<uint8_t>(ClientTerrainTreeState::Removed) ||
            (mutation.treeState == ClientTerrainTreeState::Upright &&
             mutation.pushAsideAmount == 0.0f &&
             mutation.pushAsideDeltaPerFrame == 0.0f) ||
            (treeOnlyState &&
             found->kind != ClientTerrainObjectKind::OptimizedTree) ||
            (hasPushAsideMutation &&
             found->kind != ClientTerrainObjectKind::OptimizedTree) ||
            (mutation.treeState == ClientTerrainTreeState::Stump &&
             found->stumpModelAsset.empty()) ||
            !std::isfinite(mutation.toppleDirection.x()) ||
            !std::isfinite(mutation.toppleDirection.y()) ||
            !std::isfinite(mutation.toppleDirection.z()) ||
            ((mutation.treeState == ClientTerrainTreeState::Falling ||
              mutation.treeState == ClientTerrainTreeState::Down) &&
             (!std::isfinite(toppleDirectionLengthSquared) ||
              toppleDirectionLengthSquared <=
                  std::numeric_limits<float>::epsilon())) ||
            !std::isfinite(mutation.toppleRadians) ||
            mutation.toppleRadians < 0.0f ||
            mutation.toppleRadians > math::PI * 0.5f ||
            !std::isfinite(mutation.angularVelocity) ||
            !std::isfinite(mutation.angularAcceleration) ||
            !std::isfinite(mutation.sinkOffset) ||
            mutation.sinkOffset < 0.0f ||
            mutation.sinkOffset > found->sinkDistance ||
            !std::isfinite(mutation.sinkElapsedFrames) ||
            mutation.sinkElapsedFrames < 0.0f ||
            mutation.sinkElapsedFrames >
                static_cast<float>(found->sinkFrames) ||
            !std::isfinite(mutation.pushAsideDirection.x()) ||
            !std::isfinite(mutation.pushAsideDirection.y()) ||
            !std::isfinite(mutation.pushAsideDirection.z()) ||
            !std::isfinite(mutation.pushAsideAmount) ||
            mutation.pushAsideAmount < 0.0f ||
            mutation.pushAsideAmount > 1.0f ||
            !std::isfinite(mutation.pushAsideDeltaPerFrame) ||
            (hasPushAsideMutation &&
             (!std::isfinite(pushAsideDirectionLengthSquared) ||
              pushAsideDirectionLengthSquared <=
                  std::numeric_limits<float>::epsilon())) ||
            (mutation.treeState != ClientTerrainTreeState::Upright &&
             (mutation.pushAsideAmount != 0.0f ||
              mutation.pushAsideDeltaPerFrame != 0.0f)) ||
            (mutation.toppleFrozenByFog &&
             mutation.treeState != ClientTerrainTreeState::Falling)) {
            return false;
        }
    }
    for (ClientTerrainObject& object : m_objects) {
        object.treeState = ClientTerrainTreeState::Upright;
        object.toppleDirection = {1.0f, 0.0f, 0.0f};
        object.toppleRadians = 0.0f;
        object.angularVelocity = 0.0f;
        object.angularAcceleration = 0.0f;
        object.sinkOffset = 0.0f;
        object.sinkElapsedFrames = 0.0f;
        object.pushAsideDirection = {};
        object.pushAsideAmount = 0.0f;
        object.pushAsideDeltaPerFrame = 0.0f;
        object.pushAsideSource = 0;
        object.pushAsideLastFrame = 0;
        object.toppleFrozenByFog = false;
    }
    // Restoring a checkpoint is a state overlay, not a replay of presentation
    // events which were already consumed before the checkpoint was captured.
    m_fxEvents.clear();
    for (const ClientTerrainObjectMutation& mutation : state.mutations) {
        auto found = std::find_if(
            m_objects.begin(), m_objects.end(),
            [&mutation](const ClientTerrainObject& value) {
                return value.sourceRecordIndex == mutation.sourceRecordIndex;
            });
        found->treeState = mutation.treeState;
        found->toppleDirection = mutation.toppleDirection;
        found->toppleRadians = mutation.toppleRadians;
        found->angularVelocity = mutation.angularVelocity;
        found->angularAcceleration = mutation.angularAcceleration;
        found->sinkOffset = mutation.sinkOffset;
        found->sinkElapsedFrames = mutation.sinkElapsedFrames;
        found->pushAsideDirection = mutation.pushAsideDirection;
        found->pushAsideAmount = mutation.pushAsideAmount;
        found->pushAsideDeltaPerFrame = mutation.pushAsideDeltaPerFrame;
        found->toppleFrozenByFog = mutation.toppleFrozenByFog;
    }
    changed();
    return true;
}

container::Vector<ClientTerrainFxEvent>
ClientTerrainObjectStore::takeFxEvents() {
    container::Vector<ClientTerrainFxEvent> output = std::move(m_fxEvents);
    m_fxEvents.clear();
    return output;
}

ClientTerrainObject* ClientTerrainObjectStore::find(uint32_t id) noexcept {
    const auto found = std::find_if(
        m_objects.begin(), m_objects.end(),
        [id](const ClientTerrainObject& value) { return value.id == id; });
    return found != m_objects.end() ? &*found : nullptr;
}

void ClientTerrainObjectStore::changed() noexcept {
    ++m_revision;
    if (m_revision == 0) ++m_revision;
}

std::optional<ClientTerrainObjectDefinition>
compileClientTerrainObjectDefinition(
    uint64_t sourceRecordIndex,
    const game::ThingTemplate& templateData,
    ClientTerrainObjectKind kind,
    math::vec3 position,
    float yawRadians,
    game::ModelConditionMask conditions,
    std::optional<ClientTerrainModelBounds> modelBounds) {
    const float scale = templateData.assetScale.to_float();
    const float objectSpaceRadius = modelBounds &&
            std::isfinite(modelBounds->boundingRadius) &&
            modelBounds->boundingRadius > 0.0f
        ? modelBounds->boundingRadius
        : std::max(1.0f, templateData.geometry
            .boundingSphereRadiusFixed.to_float());
    const float renderRadius = objectSpaceRadius * std::max(0.0f, scale);
    ClientTerrainObjectDefinition result{
        .sourceRecordIndex = sourceRecordIndex,
        .templateName = templateData.name,
        .kind = kind,
        .position = position,
        .yawRadians = yawRadians,
        .scale = scale,
        .boundingRadius = renderRadius,
        // W3DPropBuffer creates a temporary cylinder from the model sphere:
        // radius=2R and height=5R. Apply the same model/template-derived
        // dimensions to optimized trees instead of one map-wide guess.
        .constructionClearRadius = 2.0f * renderRadius,
        .constructionClearHeight = 5.0f * renderRadius,
        .modelConditions = conditions,
        .shadow = templateData.shadow,
        .stumpTemplateName = clientTerrainStumpTemplateName(templateData),
        .stumpScale = scale,
        .stumpBoundingRadius = std::max(
            1.0f, templateData.geometry.boundingSphereRadiusFixed.to_float()) *
            std::max(0.0f, scale),
        .treeSwayEnabled = kind == ClientTerrainObjectKind::OptimizedTree,
    };
    for (size_t channelIndex = 0;
         channelIndex < templateData.drawVisualChannels.size(); ++channelIndex) {
        if (channelIndex > kClientTerrainMaximumChannelIndex) {
            return std::nullopt;
        }
        const game::ModelDrawVisualChannel& channel =
            templateData.drawVisualChannels[channelIndex];
        ClientTerrainVisualChannel visual = compileVisual(
            channel, static_cast<uint32_t>(channelIndex), conditions);
        // A channel with Model=None is still a live presentation channel.
        // Keeping it mirrors A01/ObjectLifecycle and permits a later
        // condition/replacement to activate it without recreating an
        // authoritative Object merely to regain visual identity.
        result.visuals.push_back(std::move(visual));
    }
    // Compatibility for manually assembled/focused templates which predate
    // final Draw-channel compilation. Normal frozen content takes the channel
    // path above, including W3DTreeDraw::ModelName.
    if (result.visuals.empty() && !templateData.defaultW3dModel.empty()) {
        result.visuals.push_back({.modelAsset = templateData.defaultW3dModel});
    }
    for (const game::ModuleData& module : templateData.modules) {
        if (!equalsInsensitive(module.moduleClass, "W3DTreeDraw")) continue;
        if (const container::String* value = moduleValue(module, "ToppleFX")) {
            result.toppleFxList = *value;
        }
        if (const container::String* value = moduleValue(module, "BounceFX")) {
            result.bounceFxList = *value;
        }
        if (const container::String* value = moduleValue(module, "TextureName")) {
            if (!equalsInsensitive(*value, "none")) {
                result.treeTextureAsset = *value;
            }
        }
        if (const container::String* value = moduleValue(
                module, "MoveOutwardTime")) {
            result.moveOutwardFrames = parseDurationFramesOr(
                *value, result.moveOutwardFrames);
        }
        if (const container::String* value = moduleValue(
                module, "MoveInwardTime")) {
            result.moveInwardFrames = parseDurationFramesOr(
                *value, result.moveInwardFrames);
        }
        if (const container::String* value = moduleValue(
                module, "MoveOutwardDistanceFactor")) {
            result.moveOutwardDistanceFactor = std::max(
                0.0f, parseFiniteFloat(*value).value_or(
                    result.moveOutwardDistanceFactor));
        }
        if (const container::String* value = moduleValue(
                module, "DarkeningFactor")) {
            result.darkeningFactor = parseFiniteFloat(*value).value_or(
                result.darkeningFactor);
        }
        if (const container::String* value = moduleValue(module, "StumpName")) {
            if (!equalsInsensitive(*value, "none")) {
                result.stumpTemplateName = *value;
                // W3DTreeDraw's authored field is an asset name. Import may
                // replace this with a Thing-template default model when a mod
                // uses the same token as a template name.
                result.stumpModelAsset = *value;
            }
        }
        if (const container::String* value = moduleValue(module, "DoTopple")) {
            result.treeCanTopple = parseBool(*value, false);
        }
        if (const container::String* value = moduleValue(
                module, "KillWhenFinishedToppling")) {
            result.killTreeWhenToppled = parseBool(*value, true);
        }
        if (const container::String* value = moduleValue(module, "DoShadow")) {
            result.treeShadowEnabled = parseBool(*value, false);
        }
        if (const container::String* value = moduleValue(module, "SinkDistance")) {
            result.sinkDistance = std::max(
                0.0f, parseFiniteFloat(*value).value_or(result.sinkDistance));
        }
        if (const container::String* value = moduleValue(module, "SinkTime")) {
            result.sinkFrames = parseDurationFramesOr(
                *value, result.sinkFrames);
        }
        if (const container::String* value = moduleValue(
                module, "InitialVelocityPercent")) {
            result.initialVelocityPercent = parsePercentOr(
                *value, result.initialVelocityPercent);
        }
        if (const container::String* value = moduleValue(
                module, "InitialAccelPercent")) {
            result.initialAccelerationPercent = parsePercentOr(
                *value, result.initialAccelerationPercent);
        }
        if (const container::String* value = moduleValue(
                module, "BounceVelocityPercent")) {
            result.bounceVelocityPercent = parsePercentOr(
                *value, result.bounceVelocityPercent);
        }
        if (const container::String* value = moduleValue(
                module, "MinimumToppleSpeed")) {
            result.minimumToppleSpeed = std::max(
                0.0f, parseFiniteFloat(*value).value_or(
                    result.minimumToppleSpeed));
        }
        break;
    }
    if (kind == ClientTerrainObjectKind::OptimizedTree) {
        if (result.treeShadowEnabled) {
            // W3DTreeBuffer derives each tree type's square shadow size from
            // the model's horizontal object-space extents. GeometryInfo is
            // the frozen per-template bounds available at this boundary; use
            // its scaled X/Y diameters rather than the previous fixed 20x20.
            float shadowSize = modelBounds &&
                    std::isfinite(modelBounds->shadowSize) &&
                    modelBounds->shadowSize > 0.0f
                ? modelBounds->shadowSize * std::max(0.0f, scale)
                : (templateData.geometry.majorRadiusFixed.to_float() +
                   templateData.geometry.minorRadiusFixed.to_float()) *
                    std::max(0.0f, scale);
            if (!std::isfinite(shadowSize) || shadowSize <= 0.0f) {
                shadowSize = 2.0f * renderRadius;
            }
            result.shadow = {
                .typeMask = game::thingShadowBit(
                    game::ThingShadowFlag::Decal),
                .sizeX = shadowSize,
                .sizeY = shadowSize,
            };
        } else {
            result.shadow = {};
        }
    }
    if (result.visuals.empty() || !finiteDefinition(result)) return std::nullopt;
    return result;
}

container::String clientTerrainStumpTemplateName(
    const game::ThingTemplate& templateData) {
    for (const game::ModuleData& module : templateData.modules) {
        if (!equalsInsensitive(module.moduleClass, "W3DTreeDraw")) continue;
        if (const container::String* value = moduleValue(module, "StumpName")) {
            if (!equalsInsensitive(*value, "none")) return *value;
        }
    }
    return {};
}

container::String clientTerrainPrimaryModel(
    const game::ThingTemplate& templateData,
    game::ModelConditionMask conditions) {
    for (size_t channelIndex = 0;
         channelIndex < templateData.drawVisualChannels.size(); ++channelIndex) {
        ClientTerrainVisualChannel visual = compileVisual(
            templateData.drawVisualChannels[channelIndex],
            static_cast<uint32_t>(channelIndex), conditions);
        if (!visual.modelAsset.empty()) return visual.modelAsset;
    }
    return templateData.defaultW3dModel;
}

} // namespace engine
