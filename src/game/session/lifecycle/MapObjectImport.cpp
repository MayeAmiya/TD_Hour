#include "core/container/container_types.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/lifecycle/MapObjectImport.h"

#include "game/session/core/GameSession.h"
#include "game/session/lifecycle/GameSessionMapImportPort.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "debug/debug.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cmath>
#include <algorithm>
#include <optional>
#include <utility>

namespace engine {
namespace {

const container::String* stringProperty(const game::terrain::MapPropertyDict& properties,
                                  container::StringView key) {
    const auto found = properties.find(container::String(key));
    if (found == properties.end()) return nullptr;
    return std::get_if<container::String>(&found->second);
}

bool hasProperty(const game::terrain::MapPropertyDict& properties,
                 container::StringView key) {
    return properties.contains(container::String(key));
}

std::optional<float> numericProperty(const game::terrain::MapPropertyDict& properties,
                                     container::StringView key) noexcept {
    const auto found = properties.find(container::String(key));
    if (found == properties.end()) return std::nullopt;
    if (const int32_t* value = std::get_if<int32_t>(&found->second)) {
        return static_cast<float>(*value);
    }
    if (const float* value = std::get_if<float>(&found->second)) {
        return std::isfinite(*value) ? std::optional<float>{*value} : std::nullopt;
    }
    return std::nullopt;
}

std::optional<int32_t> integerProperty(
    const game::terrain::MapPropertyDict& properties,
    container::StringView key) noexcept {
    const auto found = properties.find(container::String(key));
    if (found == properties.end()) return std::nullopt;
    if (const int32_t* value = std::get_if<int32_t>(&found->second)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<bool> booleanProperty(
    const game::terrain::MapPropertyDict& properties,
    container::StringView key) noexcept {
    const auto found = properties.find(container::String(key));
    if (found == properties.end()) return std::nullopt;
    if (const bool* value = std::get_if<bool>(&found->second)) return *value;
    if (const int32_t* value = std::get_if<int32_t>(&found->second)) {
        return *value != 0;
    }
    return std::nullopt;
}

std::optional<MapObjectInstanceOverrides> mapObjectInstanceOverrides(
    const game::terrain::MapObjectRecord& record) {
    MapObjectInstanceOverrides value;
    bool authored = false;
    if (record.maximumHealthOverrideRaw) {
        value.maximumHealth = math::q32_32::from_raw(
            *record.maximumHealthOverrideRaw);
        authored = true;
    }
    if (record.initialHealthFractionRaw) {
        value.initialHealthFraction = math::q32_32::from_raw(
            *record.initialHealthFractionRaw);
        authored = true;
    }
    if (const std::optional<int32_t> veterancy =
            integerProperty(record.properties, "objectVeterancy");
        veterancy && *veterancy >= 0 && *veterancy <=
            static_cast<int32_t>(game::ObjectVeterancyLevel::Heroic)) {
        value.veterancy = static_cast<game::ObjectVeterancyLevel>(
            *veterancy);
        authored = true;
    }
    for (uint32_t index = 0;; ++index) {
        const container::String key =
            "objectGrantUpgrade" + std::to_string(index);
        const container::String* upgrade =
            stringProperty(record.properties, key);
        if (!upgrade || upgrade->empty()) break;
        value.grantedUpgrades.push_back(*upgrade);
        authored = true;
    }
    const auto capture = [&](container::StringView key,
                             std::optional<bool>& output) {
        output = booleanProperty(record.properties, key);
        authored = authored || output.has_value();
    };
    capture("objectEnabled", value.enabled);
    capture("objectPowered", value.powered);
    capture("objectIndestructible", value.indestructible);
    capture("objectUnsellable", value.unsellable);
    capture("objectSelectable", value.selectable);
    capture("objectRecruitableAI", value.aiRecruitable);
    capture("objectTargetable", value.playerTargetable);
    if (const std::optional<int32_t> attitude =
            integerProperty(record.properties, "objectAggressiveness");
        attitude && *attitude >=
                static_cast<int32_t>(ObjectAIAttitude::Sleep) &&
            *attitude <=
                static_cast<int32_t>(ObjectAIAttitude::Aggressive)) {
        value.aggressiveness = static_cast<ObjectAIAttitude>(*attitude);
        authored = true;
    }
    const auto captureNonNegativeDistance = [&record, &authored](
            container::StringView key,
            std::optional<math::q32_32>& output,
            float minimum) {
        const std::optional<float> source = numericProperty(
            record.properties, key);
        if (!source || *source < minimum) return;
        output = math::q32_32{*source};
        authored = true;
    };
    captureNonNegativeDistance(
        "objectStoppingDistance", value.stoppingDistance, 0.5f);
    captureNonNegativeDistance(
        "objectVisualRange", value.visionRange, 0.0f);
    captureNonNegativeDistance(
        "objectShroudClearingDistance", value.shroudClearingRange, 0.0f);
    if (const std::optional<int32_t> time =
            integerProperty(record.properties, "objectTime");
        time && (*time == 1 || *time == 2)) {
        value.night = *time == 2;
        authored = true;
    }
    if (const std::optional<int32_t> weather =
            integerProperty(record.properties, "objectWeather");
        weather && (*weather == 1 || *weather == 2)) {
        value.snow = *weather == 2;
        authored = true;
    }
    if (const container::String* ambient =
            stringProperty(record.properties, "objectSoundAmbient")) {
        value.ambientSound = *ambient;
        authored = true;
    }
    value.ambientSoundEnabled = booleanProperty(
        record.properties, "objectSoundAmbientEnabled");
    authored = authored || value.ambientSoundEnabled.has_value();
    const bool ambientCustomized = booleanProperty(
        record.properties, "objectSoundAmbientCustomized").value_or(false);
    if (ambientCustomized) {
        authored = true;
        value.ambientSoundLooping = booleanProperty(
            record.properties, "objectSoundAmbientLooping");
        if (const std::optional<int32_t> loopCount = integerProperty(
                record.properties, "objectSoundAmbientLoopCount");
            loopCount && *loopCount >= 0) {
            value.ambientSoundLoopCount = *loopCount;
        }
        const auto captureFinite = [&record](
                container::StringView key,
                std::optional<float>& output) {
            output = numericProperty(record.properties, key);
        };
        captureFinite(
            "objectSoundAmbientMinVolume",
            value.ambientSoundMinVolume);
        captureFinite(
            "objectSoundAmbientVolume",
            value.ambientSoundVolume);
        captureFinite(
            "objectSoundAmbientMinRange",
            value.ambientSoundMinRange);
        captureFinite(
            "objectSoundAmbientMaxRange",
            value.ambientSoundMaxRange);
        if (const std::optional<int32_t> priority = integerProperty(
                record.properties, "objectSoundAmbientPriority");
            priority && *priority >= 0 && *priority <= 4) {
            value.ambientSoundPriority = static_cast<uint8_t>(*priority);
        }
    }
    return authored
        ? std::optional<MapObjectInstanceOverrides>{std::move(value)}
        : std::nullopt;
}

[[nodiscard]] container::StringView resolvedMapObjectTemplateName(
    container::StringView sourceName) noexcept {
    // ZH preserves these two exact historical WorldBuilder names while
    // loading the ObjectList.  The comparison is intentionally
    // case-sensitive, matching GameLogic::handleNameChange().
    if (sourceName == "AmericaTankLeopard") return "AmericaTankCrusader";
    if (sourceName == "AmericaVehicleHumVee") return "AmericaVehicleHumvee";
    return sourceName;
}

MapImportOwnerResolution resolveOwner(
    GameSessionMapImportPort& access,
    const game::terrain::MapObjectRecord& record) {
    const container::String* owner =
        stringProperty(record.properties, "originalOwner");
    return access.resolveOwner(
        owner ? container::StringView{*owner} : container::StringView{});
}

bool hasFiniteTransform(const game::terrain::MapObjectRecord& record) noexcept {
    return record.fixedTransformValid;
}

void mixClientTerrainIdentity(uint64_t& hash, uint64_t value) noexcept {
    constexpr uint64_t kPrime = 1099511628211ull;
    for (size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<uint8_t>(value >> (index * 8u));
        hash *= kPrime;
    }
}

uint64_t frozenClientTerrainTemplateIdentity(
    const GameSessionMapImportPort& access,
    const game::terrain::TerrainHeightfieldData& source,
    const ClientTerrainImportPolicy& policy) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t recordIndex = 0;
         recordIndex < source.objects.size(); ++recordIndex) {
        const game::terrain::MapObjectRecord& record =
            source.objects[recordIndex];
        constexpr int32_t kRoadFlags = 0x00000006;
        constexpr int32_t kBridgeFlags = 0x00000030;
        if ((record.flags & (kRoadFlags | kBridgeFlags)) != 0 ||
            record.waypointId || record.name.empty() ||
            !hasFiniteTransform(record) ||
            hasProperty(record.properties, "lightHeightAboveTerrain") ||
            hasProperty(record.properties, "scorchType")) {
            continue;
        }
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            access.findObjectArchetype(
                resolvedMapObjectTemplateName(record.name));
        if (!archetype ||
            classifyClientTerrainObject(
                archetype->kindOfMask,
                archetype->templateData.fenceWidthFixed, policy) !=
                ClientTerrainImportDisposition::ClientTerrainObject) {
            continue;
        }
        mixClientTerrainIdentity(hash, recordIndex);
        mixClientTerrainIdentity(hash, archetype->recipeFingerprint);
        const container::String stumpName =
            clientTerrainStumpTemplateName(archetype->templateData);
        container::SharedPtr<const game::ObjectArchetype> stump;
        if (!stumpName.empty()) {
            stump = access.findObjectArchetype(stumpName);
        }
        mixClientTerrainIdentity(
            hash, stump ? stump->recipeFingerprint : 0u);
    }
    return hash != 0 ? hash : 1u;
}

} // namespace

MapObjectSpawnReport MapObjectImport::import(
    GameSession& session,
    const game::terrain::TerrainHeightfieldData& source) {
    MapObjectSpawnReport report;
    GameSessionMapImportPort access = session.mapImportPort();
    struct HeightMutationBatch final {
        GameSessionMapImportPort& access;
        explicit HeightMutationBatch(
            GameSessionMapImportPort& value) noexcept : access(value) {
            access.beginTerrainHeightMutationBatch();
        }
        ~HeightMutationBatch() {
            access.endTerrainHeightMutationBatch();
        }
    } heightMutationBatch{access};
    report.sourceRecordCount = source.objects.size();
    const ClientTerrainImportPolicy clientPolicy =
        access.clientTerrainPolicy();
    const game::ModelConditionMask clientConditions =
        access.initialModelConditions();
    const game::MapContentIdentity& mapIdentity =
        access.mapIdentity();
    const ResolvedMatchSetup* match = session.resolvedMatchSetup();
    const uint64_t simulationContentFingerprint = match
        ? match->simulationContentFingerprint
        : access.simulationContentFingerprint();
    const uint64_t frozenTemplateIdentity =
        frozenClientTerrainTemplateIdentity(access, source, clientPolicy);
    session.clientTerrainObjects().beginMapRebuild(
        access.presentationEpoch(),
        clientTerrainContentIdentity(
            mapIdentity.crc, mapIdentity.size,
            simulationContentFingerprint, frozenTemplateIdentity,
            clientConditions, clientPolicy));

    for (size_t recordIndex = 0; recordIndex < source.objects.size(); ++recordIndex) {
        const game::terrain::MapObjectRecord& record = source.objects[recordIndex];
        constexpr int32_t kRoadFlags = 0x00000006;
        constexpr int32_t kBridgeFlags = 0x00000030;
        constexpr int32_t kBridgePoint1 = 0x00000010;
        constexpr int32_t kBridgePoint2 = 0x00000020;
        if ((record.flags & kBridgePoint1) != 0 &&
            recordIndex + 1u < source.objects.size() &&
            (source.objects[recordIndex + 1u].flags & kBridgePoint2) != 0) {
            const game::terrain::MapObjectRecord& second =
                source.objects[recordIndex + 1u];
            report.skippedRoadOrBridgeCount += 2u;
            if (!hasFiniteTransform(record) || !hasFiniteTransform(second)) {
                ++report.terrainBridgeAuthorityFailureCount;
                ++recordIndex;
                continue;
            }
            const container::SharedPtr<const game::ObjectArchetype> bridgeArchetype =
                access.findObjectArchetype("GenericBridge");
            if (!bridgeArchetype) {
                ++report.terrainBridgeAuthorityFailureCount;
                TD_LOG_WARN(
                    "[MapObjectImport] GenericBridge authority template is unavailable for terrain bridge '{}'",
                    record.name);
                ++recordIndex;
                continue;
            }
            const MapImportOwnerResolution owner = resolveOwner(
                access, record);
            if (owner.usedFallback) ++report.neutralOwnerFallbackCount;
            if (owner.scenarioTeamUnresolved) {
                ++report.scenarioTeamResolutionFailureCount;
                ++report.terrainBridgeAuthorityFailureCount;
                ++recordIndex;
                continue;
            }
            const math::q32_32 startX =
                math::q32_32::from_raw(record.positionRaw[0]);
            const math::q32_32 startY =
                math::q32_32::from_raw(record.positionRaw[1]);
            const math::q32_32 endX =
                math::q32_32::from_raw(second.positionRaw[0]);
            const math::q32_32 endY =
                math::q32_32::from_raw(second.positionRaw[1]);
            const math::q32_32 heightBias =
                math::q32_32::from_fraction(1, 4);
            const math::q32_32 startZ = math::q32_32::from_raw(
                access.groundHeightRaw(
                    startX.raw(), startY.raw())) + heightBias;
            const math::q32_32 endZ = math::q32_32::from_raw(
                access.groundHeightRaw(
                    endX.raw(), endY.raw())) + heightBias;
            const LogicFixedVec3 bridgePosition{
                (startX + endX) / math::q32_32{int32_t{2}},
                (startY + endY) / math::q32_32{int32_t{2}},
                (startZ + endZ) / math::q32_32{int32_t{2}},
            };
            const math::q32_32 bridgeYaw =
                math::fixed_atan2(endY - startY, endX - startX);
            MapObjectProvenanceComponent provenance;
            provenance.sourceRecordIndex = static_cast<uint64_t>(recordIndex);
            provenance.sourceName = record.name;
            provenance.mapFlags = record.flags;
            provenance.properties = record.properties;
            ObjectSpawnRequest request;
            request.templateName = "GenericBridge";
            request.owner = owner.player;
            request.primaryTeam = owner.primaryTeam;
            request.origin = ObjectCreationOrigin::Map;
            request.transform = ObjectFixedTransformComponent{
                .position = bridgePosition,
                .yawRadians = bridgeYaw,
                .authoritative = true,
            };
            request.mapProvenance = std::move(provenance);
            request.mapInstanceOverrides =
                mapObjectInstanceOverrides(record);
            if (session.objectLifecycleTransactions().spawnObject(
                    std::move(request))) {
                ++report.terrainBridgeAuthorityCount;
            } else {
                ++report.terrainBridgeAuthorityFailureCount;
                TD_LOG_WARN(
                    "[MapObjectImport] Could not create GenericBridge authority for terrain bridge '{}' at source record {}",
                    record.name, recordIndex);
            }
            ++recordIndex;
            continue;
        }
        if ((record.flags & (kRoadFlags | kBridgeFlags)) != 0) {
            ++report.skippedRoadOrBridgeCount;
            continue;
        }
        // Waypoints are retained by TerrainLogic, not duplicated as visual
        // Thing entities merely because an editor record happens to have a
        // display name.
        if (record.waypointId) {
            ++report.skippedWaypointCount;
            continue;
        }
        // These are client-terrain records in RefCode, not Thing instances.
        // Keep them in the immutable map source, but do not create a gameplay
        // entity even when an editor happened to assign a matching name.
        if (hasProperty(record.properties, "lightHeightAboveTerrain") ||
            hasProperty(record.properties, "scorchType")) {
            ++report.skippedClientMetadataCount;
            continue;
        }
        if (record.name.empty()) {
            ++report.skippedEmptyNameCount;
            continue;
        }
        if (!hasFiniteTransform(record)) {
            ++report.skippedInvalidTransformCount;
            continue;
        }

        // GameDataRegistry routes through the authoritative ThingFactory in a
        // real session.  Unknown records (lights, scorch marks, old editor
        // objects and unimplemented special cases) remain safely metadata-only.
        const container::StringView resolvedTemplateName =
            resolvedMapObjectTemplateName(record.name);
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            access.findObjectArchetype(
                resolvedTemplateName);
        if (!archetype) {
            ++report.skippedUnknownTemplateCount;
            continue;
        }
        const game::ThingTemplate* templateData = &archetype->templateData;

        const ClientTerrainImportDisposition disposition =
            classifyClientTerrainObject(
                archetype->kindOfMask, templateData->fenceWidthFixed,
                clientPolicy);
        if (disposition ==
            ClientTerrainImportDisposition::DisabledDecoration) {
            ++report.disabledClientDecorationCount;
            continue;
        }
        if (disposition ==
            ClientTerrainImportDisposition::ClientTerrainObject) {
            const std::optional<ClientTerrainObjectKind> kind =
                clientTerrainObjectKind(
                    archetype->kindOfMask, templateData->fenceWidthFixed);
            const math::vec3 position{
                record.position.x(), record.position.y(),
                record.position.z() + access.groundHeight(
                    record.position.x(), record.position.y())};
            std::optional<ClientTerrainObjectDefinition> compiled = kind
                ? compileClientTerrainObjectDefinition(
                      static_cast<uint64_t>(recordIndex), *templateData,
                      *kind, position, math::normalize_angle(record.angle),
                      clientConditions)
                : std::nullopt;
            if (compiled && !compiled->stumpTemplateName.empty()) {
                if (const container::SharedPtr<const game::ObjectArchetype>
                        stumpArchetype = access.findObjectArchetype(
                            compiled->stumpTemplateName)) {
                    const game::ThingTemplate* stumpTemplate =
                        &stumpArchetype->templateData;
                    const container::String resolved =
                        clientTerrainPrimaryModel(
                            *stumpTemplate, clientConditions);
                    if (!resolved.empty()) {
                        compiled->stumpModelAsset = resolved;
                        const float stumpScale =
                            stumpTemplate->assetScale.to_float();
                        if (std::isfinite(stumpScale) &&
                            stumpScale > 0.0f) {
                            compiled->stumpScale = stumpScale;
                            compiled->stumpBoundingRadius = std::max(
                                1.0f,
                                stumpTemplate->geometry
                                    .boundingSphereRadiusFixed.to_float()) *
                                stumpScale;
                        }
                    }
                }
            }
            if (!compiled ||
                !session.clientTerrainObjects().add(
                    std::move(*compiled))) {
                ++report.clientTerrainVisualFailureCount;
            } else {
                ++report.clientTerrainObjectCount;
            }
            continue;
        }

        const MapImportOwnerResolution owner = resolveOwner(
            access, record);
        if (owner.usedFallback) ++report.neutralOwnerFallbackCount;
        if (owner.scenarioTeamUnresolved) {
            ++report.scenarioTeamResolutionFailureCount;
            ++report.creationFailureCount;
            TD_LOG_WARN("[MapObjectImport] originalOwner on '{}' references an unavailable Scenario Team",
                        record.name);
            continue;
        }

        MapObjectProvenanceComponent provenance;
        provenance.sourceRecordIndex = static_cast<uint64_t>(recordIndex);
        provenance.sourceName = record.name;
        provenance.mapFlags = record.flags;
        provenance.properties = record.properties;

        ObjectSpawnRequest request;
        request.templateName = container::String{resolvedTemplateName};
        request.owner = owner.player;
        request.primaryTeam = owner.primaryTeam;
        request.origin = ObjectCreationOrigin::Map;
        const math::q32_32 objectX =
            math::q32_32::from_raw(record.positionRaw[0]);
        const math::q32_32 objectY =
            math::q32_32::from_raw(record.positionRaw[1]);
        const math::q32_32 objectZ =
            math::q32_32::from_raw(record.positionRaw[2]) +
            math::q32_32::from_raw(access.groundHeightRaw(
                objectX.raw(), objectY.raw()));
        const math::q32_32 objectYaw =
            math::q32_32::from_raw(record.angleRaw);
        request.transform = ObjectFixedTransformComponent{
            .position = {objectX, objectY, objectZ},
            .yawRadians = objectYaw,
            .authoritative = true,
        };
        // ZH imports WorldBuilder objects onto the authored heightfield and
        // does not flatten terrain here.  Flattening is an explicit placement
        // transaction used by Dozer/Worker construction and LIKE_EXISTING
        // OCL creation.  Repeating it for map-authored structures destroys
        // authored mountain profiles and makes the result depend on object
        // import order.
        request.mapProvenance = std::move(provenance);
        // WorldBuilder Object Panel values are a post-create transaction in
        // ZH. Keep them distinct from generic construction-time spawn
        // overrides so modules initialize from pristine template state.
        request.mapInstanceOverrides = mapObjectInstanceOverrides(record);
        if (const container::String* scriptName = stringProperty(record.properties, "objectName");
            scriptName && !scriptName->empty()) {
            request.scriptName = *scriptName;
        }

        const GameSessionObjectSpawnResult spawned =
            session.objectLifecycleTransactions().spawnObject(
                std::move(request));
        if (!spawned) {
            ++report.creationFailureCount;
            continue;
        }
        // GameLogic.cpp calls Team::setActive() only after ThingFactory has
        // returned a live map Object. This turns an inactive singleton or
        // lazily materialized ordinary prototype into an active Team and
        // exposes its one-frame TEAM_CREATED pulse at the first confirmed
        // ScriptEngine update. Do not activate it if object creation failed.
        if (owner.usesScenarioTeam &&
            !access.activateScenarioTeam(owner.primaryTeam)) {
            TD_LOG_ERROR("[MapObjectImport] Could not activate Scenario Team {} after spawning '{}'",
                         owner.primaryTeam.value, record.name);
        }
        if (owner.usesScenarioTeam) ++report.scenarioTeamMemberCount;
        // RefCode's Object::updateObjValuesFromMapProperties consumes
        // `objectName` as the per-instance script name. It is not the map
        // record/Thing template name above. The lifecycle-backed session spawn
        // binds it only after the entity has a stable ObjectId, so future
        // scripts never retain an ECS entity through map import.
        if (spawned.scriptNameRequested) {
            if (spawned.scriptNameBound) {
                ++report.scriptNameBoundCount;
            } else {
                ++report.scriptNameConflictCount;
            }
        }
        ++report.spawnedCount;
    }

    return report;
}

} // namespace engine
