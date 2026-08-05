#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::String upperAscii(container::StringView value) {
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(
            character >= 'a' && character <= 'z'
                ? character - ('a' - 'A') : character));
    }
    return result;
}

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(container::StringView value) noexcept {
    value = trimAsciiView(value);
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<int32_t> parseSigned(container::StringView value) noexcept {
    value = trimAsciiView(value);
    if (value.empty()) return std::nullopt;
    int32_t result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size()
        ? std::optional<int32_t>{result} : std::nullopt;
}


[[nodiscard]] std::optional<bool> parseBoolean(container::StringView value) noexcept {
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "yes") || asciiEqualIgnoreCase(value, "true") ||
        value == "1") return true;
    if (asciiEqualIgnoreCase(value, "no") || asciiEqualIgnoreCase(value, "false") ||
        value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] std::optional<math::q32_32> parsePercentFixed(
    container::StringView value) noexcept {
    value = trimAsciiView(value);
    bool percent = false;
    if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
        percent = true;
    }
    const std::optional<float> parsed = parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "Containment",
        .field = "Percent", .fallback = 0.0f});
    if (!parsed) return std::nullopt;
    const double normalized = percent
        ? static_cast<double>(*parsed) / 100.0
        : static_cast<double>(*parsed);
    if (!std::isfinite(normalized)) return std::nullopt;
    return math::q32_32{normalized};
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value) noexcept {
    const std::optional<float> parsed = parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "Containment",
        .field = "FixedReal", .fallback = 0.0f});
    if (!parsed) return std::nullopt;
    return math::q32_32{static_cast<double>(*parsed)};
}

struct ParsedFixedVector final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

[[nodiscard]] std::optional<ParsedFixedVector> parseFixedVector(
    container::StringView value) noexcept {
    ParsedFixedVector result;
    bool any = false;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        const container::StringView token = value.substr(cursor, end - cursor);
        const size_t colon = token.find(':');
        if (colon == container::StringView::npos || colon == 0) return std::nullopt;
        const std::optional<math::q32_32> parsed = parseFixed(token.substr(colon + 1));
        if (!parsed) return std::nullopt;
        switch (static_cast<unsigned char>(std::toupper(
            static_cast<unsigned char>(token.front())))) {
        case 'X': result.x = *parsed; break;
        case 'Y': result.y = *parsed; break;
        case 'Z': result.z = *parsed; break;
        default: return std::nullopt;
        }
        any = true;
        cursor = end;
    }
    return any ? std::optional<ParsedFixedVector>{result} : std::nullopt;
}

[[nodiscard]] container::Vector<container::String>
parseTokenList(container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,+", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,+", cursor);
        const container::StringView token = value.substr(cursor, end - cursor);
        if (!token.empty() && !asciiEqualIgnoreCase(token, "NONE"))
            result.push_back(upperAscii(token));
        cursor = end;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] container::Vector<container::String>
parseOrderedTokens(container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t", cursor);
        result.emplace_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return result;
}

[[nodiscard]] uint32_t parseDecalShadowTypeMask(
    container::StringView value, uint32_t fallback) {
    uint32_t result = 0;
    bool found = false;
    for (const container::String& token : parseTokenList(value)) {
        uint32_t bit = 0;
        if (token == "SHADOW_NONE") return 0;
        if (token == "SHADOW_DECAL") bit = 0x01u;
        else if (token == "SHADOW_VOLUME") bit = 0x02u;
        else if (token == "SHADOW_PROJECTION") bit = 0x04u;
        else if (token == "SHADOW_DYNAMIC_PROJECTION") bit = 0x08u;
        else if (token == "SHADOW_DIRECTIONAL_PROJECTION") bit = 0x10u;
        else if (token == "SHADOW_ALPHA_DECAL") bit = 0x20u;
        else if (token == "SHADOW_ADDITIVE_DECAL") bit = 0x40u;
        else continue;
        result |= bit;
        found = true;
    }
    return found ? result : fallback;
}

[[nodiscard]] std::optional<container::Array<uint8_t, 4>> parseDecalColor(
    container::StringView value) {
    container::Array<uint8_t, 4> result{0, 0, 0, 255};
    bool any = false;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        const container::StringView token = value.substr(cursor, end - cursor);
        const size_t colon = token.find(':');
        if (colon != 1 || token.size() <= 2) return std::nullopt;
        const std::optional<uint32_t> channel = parseUnsigned(
            token.substr(colon + 1));
        if (!channel || *channel > 255) return std::nullopt;
        size_t index = 0;
        switch (static_cast<unsigned char>(std::toupper(
            static_cast<unsigned char>(token.front())))) {
        case 'R': index = 0; break;
        case 'G': index = 1; break;
        case 'B': index = 2; break;
        case 'A': index = 3; break;
        default: return std::nullopt;
        }
        result[index] = static_cast<uint8_t>(*channel);
        any = true;
        cursor = end;
    }
    return any ? std::optional{result} : std::nullopt;
}

[[nodiscard]] std::optional<engine::ObjectContainmentKind>
containmentKind(container::StringView name) noexcept {
    using engine::ObjectContainmentKind;
    if (asciiEqualIgnoreCase(name, "OpenContain")) return ObjectContainmentKind::Open;
    if (asciiEqualIgnoreCase(name, "MobNexusContain")) return ObjectContainmentKind::MobNexus;
    if (asciiEqualIgnoreCase(name, "CaveContain")) return ObjectContainmentKind::Cave;
    if (asciiEqualIgnoreCase(name, "HealContain")) return ObjectContainmentKind::Heal;
    if (asciiEqualIgnoreCase(name, "GarrisonContain")) return ObjectContainmentKind::Garrison;
    if (asciiEqualIgnoreCase(name, "TransportContain")) return ObjectContainmentKind::Transport;
    // Both legacy specializations inherit TransportContain and therefore must
    // materialize the shared typed containment rule before their dedicated
    // Economy/Bridge runtimes can observe a stable containment edge.
    if (asciiEqualIgnoreCase(name, "InternetHackContain")) return ObjectContainmentKind::Transport;
    if (asciiEqualIgnoreCase(name, "RailedTransportContain")) return ObjectContainmentKind::Transport;
    if (asciiEqualIgnoreCase(name, "RiderChangeContain")) return ObjectContainmentKind::RiderChange;
    if (asciiEqualIgnoreCase(name, "TunnelContain")) return ObjectContainmentKind::Tunnel;
    if (asciiEqualIgnoreCase(name, "OverlordContain")) return ObjectContainmentKind::Overlord;
    if (asciiEqualIgnoreCase(name, "HelixContain")) return ObjectContainmentKind::Helix;
    if (asciiEqualIgnoreCase(name, "ParachuteContain")) return ObjectContainmentKind::Parachute;
    return std::nullopt;
}

[[nodiscard]] std::optional<engine::ObjectTransportBehaviorKind>
transportBehaviorKind(container::StringView name) noexcept {
    using engine::ObjectTransportBehaviorKind;
    if (asciiEqualIgnoreCase(name, "BunkerBusterBehavior"))
        return ObjectTransportBehaviorKind::BunkerBuster;
    if (asciiEqualIgnoreCase(name, "BattleBusSlowDeathBehavior"))
        return ObjectTransportBehaviorKind::BattleBusSlowDeath;
    if (asciiEqualIgnoreCase(name, "AssaultTransportAIUpdate"))
        return ObjectTransportBehaviorKind::AssaultTransportAI;
    if (asciiEqualIgnoreCase(name, "DeliverPayloadAIUpdate"))
        return ObjectTransportBehaviorKind::DeliverPayloadAI;
    if (asciiEqualIgnoreCase(name, "PilotFindVehicleUpdate"))
        return ObjectTransportBehaviorKind::PilotFindVehicle;
    if (asciiEqualIgnoreCase(name, "HijackerUpdate"))
        return ObjectTransportBehaviorKind::Hijacker;
    if (asciiEqualIgnoreCase(name, "TransportAIUpdate"))
        return ObjectTransportBehaviorKind::TransportAI;
    return std::nullopt;
}

void appendDiagnostic(engine::ObjectContainmentPlan& plan,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const engine::ObjectContainmentPlan>
compileObjectContainmentPlan(const ThingTemplate& templateData,
                             const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<engine::ObjectContainmentPlan>();
    for (const ModuleData& module : templateData.modules) {
        const std::optional<engine::ObjectContainmentKind> kind =
            containmentKind(moduleClass(module));
        const std::optional<engine::ObjectTransportBehaviorKind> behaviorKind =
            transportBehaviorKind(moduleClass(module));
        if (!kind && !behaviorKind) continue;

        if (behaviorKind) {
            engine::ObjectTransportBehaviorRule behavior;
            behavior.kind = *behaviorKind;
            behavior.authoredOrder = module.authoredOrder;
            if (*behaviorKind ==
                engine::ObjectTransportBehaviorKind::PilotFindVehicle)
                behavior.minimumHealthFraction = math::q32_32{0.5};
            if (*behaviorKind ==
                engine::ObjectTransportBehaviorKind::AssaultTransportAI)
                behavior.clearRange = math::q32_32{50};
            if (*behaviorKind ==
                engine::ObjectTransportBehaviorKind::BunkerBuster) {
                behavior.seismicRadius = math::q32_32{140};
                behavior.seismicMagnitude = math::q32_32{6};
            }
            const auto copy = [&](container::StringView key,
                                  container::String& destination) {
                if (const container::String* value = moduleValueLast(module, key))
                    destination = *value;
            };
            const auto fixed = [&](container::StringView key,
                                   math::q32_32& destination, bool percent = false) {
                if (const container::String* value = moduleValueLast(module, key)) {
                    const std::optional<math::q32_32> parsed =
                        percent ? parsePercentFixed(*value) : parseFixed(*value);
                    if (parsed) destination = *parsed;
                    else appendDiagnostic(*plan, module,
                                          "invalid " + container::String{key});
                }
            };
            const auto milliseconds = [&](container::StringView key,
                                          uint32_t& destination) {
                if (const container::String* value = moduleValueLast(module, key)) {
                    if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                        destination = *parsed;
                    else appendDiagnostic(*plan, module,
                                          "invalid " + container::String{key});
                }
            };
            copy("UpgradeRequired", behavior.upgradeRequired);
            if (upgradeCatalog && !behavior.upgradeRequired.empty()) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(behavior.upgradeRequired)) {
                    behavior.upgradeRequiredId = definition->id;
                }
            }
            copy("FXStartUndeath", behavior.fxStart);
            copy("OCLStartUndeath", behavior.oclStart);
            copy("FXHitGround", behavior.fxFinish);
            copy("OCLHitGround", behavior.oclFinish);
            copy("DetonationFX", behavior.fxStart);
            copy("CrashThroughBunkerFX", behavior.fxFinish);
            copy("ShockwaveWeaponTemplate", behavior.shockwaveWeapon);
            copy("OccupantDamageWeaponTemplate", behavior.occupantDamageWeapon);
            copy("AttachToTargetBone", behavior.attachBone);
            copy("ParachuteName", behavior.parachuteTemplate);
            copy("VisibleDropBoneBaseName",
                 behavior.visibleDropBoneBaseName);
            copy("VisibleSubObjectBaseName",
                 behavior.visibleSubObjectBaseName);
            copy("VisiblePayloadTemplateName",
                 behavior.visiblePayloadTemplate);
            copy("VisiblePayloadWeaponTemplate",
                 behavior.visiblePayloadWeapon);
            copy("StrafingWeaponSlot", behavior.strafingWeaponSlot);
            copy("StrafeWeaponFX", behavior.strafeWeaponFx);
            copy("DeliveryDecal", behavior.deliveryDecal);
            fixed("PercentDamageToPassengers", behavior.passengerDamageFraction, true);
            fixed("MinHealth", behavior.minimumHealthFraction, true);
            fixed("MembersGetHealedAtLifeRatio", behavior.healMembersAtLifeFraction, true);
            fixed("SeismicEffectRadius", behavior.seismicRadius);
            fixed("SeismicEffectMagnitude", behavior.seismicMagnitude);
            fixed("ScanRange", behavior.scanRange);
            fixed("ClearRangeRequiredToContinueAttackMove", behavior.clearRange);
            fixed("DeliveryDistance", behavior.deliveryDistance);
            fixed("ExitPitchRate", behavior.exitPitchRate);
            fixed("DiveStartDistance", behavior.diveStartDistance);
            fixed("DiveEndDistance", behavior.diveEndDistance);
            fixed("StrafeLength", behavior.strafeLength);
            fixed("DeliveryDecalRadius", behavior.deliveryDecalRadius);
            fixed("ThrowForce", behavior.throwForce);
            milliseconds("EmptyHulkDestructionDelay",
                         behavior.emptyDestructionDelayMilliseconds);
            milliseconds("ScanRate", behavior.scanRateMilliseconds);
            milliseconds("DoorDelay", behavior.doorDelayMilliseconds);
            milliseconds("DropDelay", behavior.dropDelayMilliseconds);
            milliseconds("MaxAttempts", behavior.maximumAttempts);
            milliseconds("CrashThroughBunkerFXFrequency",
                         behavior.crashThroughBunkerFxFrequency);
            if (const container::String* value = moduleValueLast(
                    module, "VisibleItemsDroppedPerInterval")) {
                if (const auto parsed = parseUnsigned(*value))
                    behavior.visibleItemsDroppedPerInterval = *parsed;
                else appendDiagnostic(
                    *plan, module,
                    "invalid VisibleItemsDroppedPerInterval");
            }
            if (const container::String* value = moduleValueLast(
                    module, "VisibleNumBones")) {
                if (const auto parsed = parseUnsigned(*value))
                    behavior.visiblePayloadCount = *parsed;
                else appendDiagnostic(*plan, module,
                                      "invalid VisibleNumBones");
            }
            const auto behaviorBoolean = [&](container::StringView key,
                                             bool& destination) {
                if (const container::String* value = moduleValueLast(
                        module, key)) {
                    if (const auto parsed = parseBoolean(*value))
                        destination = *parsed;
                    else appendDiagnostic(
                        *plan, module,
                        "invalid " + container::String{key});
                }
            };
            behaviorBoolean("InheritTransportVelocity",
                            behavior.inheritTransportVelocity);
            behaviorBoolean("ParachuteDirectly",
                            behavior.parachuteDirectly);
            behaviorBoolean("SelfDestructObject",
                            behavior.selfDestructAfterDelivery);
            behaviorBoolean("FireWeapon", behavior.fireWeaponPayload);
            for (const ModuleData& child : module.children) {
                if (!asciiEqualIgnoreCase(
                        moduleClass(child), "DeliveryDecal")) continue;
                if (const container::String* value = moduleValueLast(
                        child, "Texture")) {
                    behavior.deliveryDecal = *value;
                }
                if (const container::String* value = moduleValueLast(
                        child, "Style")) {
                    behavior.deliveryDecalShadowTypeMask =
                        parseDecalShadowTypeMask(
                            *value, behavior.deliveryDecalShadowTypeMask);
                }
                if (const container::String* value = moduleValueLast(
                        child, "OpacityMin")) {
                    if (const auto parsed = parsePercentFixed(*value))
                        behavior.deliveryDecalMinimumOpacity = *parsed;
                }
                if (const container::String* value = moduleValueLast(
                        child, "OpacityMax")) {
                    if (const auto parsed = parsePercentFixed(*value))
                        behavior.deliveryDecalMaximumOpacity = *parsed;
                }
                behavior.deliveryDecalMaximumOpacity = std::max(
                    behavior.deliveryDecalMinimumOpacity,
                    behavior.deliveryDecalMaximumOpacity);
                if (const container::String* value = moduleValueLast(
                        child, "OpacityThrobTime")) {
                    if (const auto milliseconds = parseUnsigned(*value)) {
                        behavior.deliveryDecalOpacityThrobMilliseconds =
                            *milliseconds;
                    }
                }
                if (const container::String* value = moduleValueLast(
                        child, "Color")) {
                    if (const auto color = parseDecalColor(*value)) {
                        behavior.deliveryDecalColor = *color;
                        behavior.deliveryDecalUsesPlayerColor = std::all_of(
                            color->begin(), color->end(),
                            [](uint8_t channel) { return channel == 0; });
                    }
                }
                if (const container::String* value = moduleValueLast(
                        child, "OnlyVisibleToOwningPlayer")) {
                    if (const auto parsed = parseBoolean(*value)) {
                        behavior.deliveryDecalOnlyVisibleToOwningPlayer =
                            *parsed;
                    }
                }
            }
            if (const container::String* value = moduleValueLast(module, "PutInContainer")) {
                behavior.putInContainer = !trimAsciiView(*value).empty() &&
                    !asciiEqualIgnoreCase(trimAsciiView(*value), "No") &&
                    !asciiEqualIgnoreCase(trimAsciiView(*value), "None");
                if (behavior.putInContainer) behavior.payloadTemplate = *value;
            }
            if (const container::String* value = moduleValueLast(module, "DropOffset")) {
                if (const std::optional<ParsedFixedVector> parsed = parseFixedVector(*value)) {
                    behavior.dropOffsetX = parsed->x;
                    behavior.dropOffsetY = parsed->y;
                    behavior.dropOffsetZ = parsed->z;
                } else appendDiagnostic(*plan, module, "invalid DropOffset");
            }
            if (const container::String* value = moduleValueLast(module, "DropVariance")) {
                if (const std::optional<ParsedFixedVector> parsed = parseFixedVector(*value)) {
                    behavior.dropVarianceX = parsed->x;
                    behavior.dropVarianceY = parsed->y;
                    behavior.dropVarianceZ = parsed->z;
                } else appendDiagnostic(*plan, module, "invalid DropVariance");
            }
            plan->behaviorRules.push_back(std::move(behavior));
        }
        if (!kind) continue;

        engine::ObjectContainmentRule rule;
        rule.kind = *kind;
        rule.authoredOrder = module.authoredOrder;
        if (*kind == engine::ObjectContainmentKind::Open ||
            *kind == engine::ObjectContainmentKind::MobNexus ||
            *kind == engine::ObjectContainmentKind::Heal ||
            *kind == engine::ObjectContainmentKind::Garrison) {
            rule.containMax = std::numeric_limits<uint32_t>::max();
        } else if (*kind == engine::ObjectContainmentKind::Cave ||
                   *kind == engine::ObjectContainmentKind::Tunnel) {
            // RefCode's CaveSystem and each player's TunnelTracker use the
            // GlobalData default MaxTunnelCapacity (10), not OpenContain's
            // per-entry count.
            rule.containMax = 10;
            game::setObjectKind(rule.forbidInsideKindOf,
                                game::ObjectKindOf::Aircraft);
            if (*kind == engine::ObjectContainmentKind::Tunnel) {
                // TunnelContainModuleData defaults to one logic frame.  One
                // millisecond still canonicalizes to exactly one confirmed
                // tick at every supported session rate.
                rule.timeForFullHealMilliseconds = 1;
            }
        } else if (*kind == engine::ObjectContainmentKind::Parachute) {
            rule.containMax = 1;
            // ParachuteContain visibly carries its rider in the world.  It is
            // a special zero-slot container, not a fully enclosing transport.
            rule.enclosingContainer = false;
        } else {
            // TransportContainModuleData::m_slotCapacity defaults to zero.
            rule.containMax = 0;
            game::setObjectKind(rule.allowInsideKindOf,
                                game::ObjectKindOf::Infantry);
        }
        if (*kind == engine::ObjectContainmentKind::Garrison) {
            game::setObjectKind(rule.allowInsideKindOf,
                                game::ObjectKindOf::Infantry);
            // GarrisonContain does not inherit OpenContain's random nearby
            // scatter. Its default evacuation policy is BurstFromCenter and
            // it owns a dedicated one-point pathfind handoff.
            rule.scatterNearbyOnExit = false;
        }
        if (*kind == engine::ObjectContainmentKind::MobNexus) {
            // MobNexusContain extends OpenContain structurally, but its
            // constructor narrows the default payload to infantry and its
            // admission rule is deliberately reversed: the rider must regard
            // the nexus as an ally.  Keep that distinction in the immutable
            // recipe instead of treating every OpenContain as a mob nexus.
            game::setObjectKind(rule.allowInsideKindOf,
                                game::ObjectKindOf::Infantry);
            rule.allowEnemiesInside = false;
            rule.allowNeutralInside = false;
        }
        rule.railedDockOwnsExit = asciiEqualIgnoreCase(
            moduleClass(module), "RailedTransportContain");
        rule.destroyPassengersWithContainer =
            *kind == engine::ObjectContainmentKind::RiderChange;
        rule.followsContainerTransform =
            *kind != engine::ObjectContainmentKind::Open &&
            *kind != engine::ObjectContainmentKind::MobNexus &&
            *kind != engine::ObjectContainmentKind::Garrison &&
            *kind != engine::ObjectContainmentKind::Tunnel;
        rule.passengersAllowedToFire =
            *kind == engine::ObjectContainmentKind::Garrison ||
            *kind == engine::ObjectContainmentKind::Open ||
            *kind == engine::ObjectContainmentKind::Overlord ||
            *kind == engine::ObjectContainmentKind::Helix;

        if (*kind == engine::ObjectContainmentKind::Cave) {
            if (const container::String* value = moduleValueLast(module, "CaveIndex")) {
                if (const std::optional<int32_t> parsed = parseSigned(*value)) {
                    rule.caveIndex = *parsed;
                } else {
                    appendDiagnostic(*plan, module, "invalid CaveIndex");
                }
            }
        }

        const bool authoredContainMax =
            moduleValueLast(module, "ContainMax") != nullptr;
        const container::String* containMax =
            moduleValueLast(module, "ContainMax");
        if (!containMax) containMax = moduleValueLast(module, "Slots");
        if (containMax) {
            if (const std::optional<int32_t> parsed = parseSigned(*containMax);
                parsed && (*parsed >= 0 || authoredContainMax)) {
                rule.containMax = *parsed < 0
                    ? std::numeric_limits<uint32_t>::max()
                    : static_cast<uint32_t>(*parsed);
            } else {
                appendDiagnostic(*plan, module, "invalid ContainMax");
            }
        }
        if (const container::String* value = moduleValueLast(module, "AllowInsideKindOf"))
            static_cast<void>(game::compileObjectKindOfMask(
                *value, rule.allowInsideKindOf));
        if (const container::String* value = moduleValueLast(module, "ForbidInsideKindOf"))
            static_cast<void>(game::compileObjectKindOfMask(
                *value, rule.forbidInsideKindOf));
        if ((*kind == engine::ObjectContainmentKind::Cave ||
             *kind == engine::ObjectContainmentKind::Tunnel) &&
            !game::objectHasKind(rule.forbidInsideKindOf,
                                 game::ObjectKindOf::Aircraft)) {
            game::setObjectKind(rule.forbidInsideKindOf,
                                game::ObjectKindOf::Aircraft);
        }
        if (const container::String* value = moduleValueLast(module, "PayloadTemplateName"))
            rule.payloadTemplateNames = parseOrderedTokens(*value);
        const auto appendInitialPayload = [&](container::StringView key) {
            const container::String* authoredValue =
                moduleValueLast(module, key);
            if (!authoredValue) return;
            const container::Vector<container::String> fields =
                parseOrderedTokens(*authoredValue);
            if (fields.empty()) return;
            uint32_t count = 1;
            if (fields.size() > 1) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(fields[1])) count = *parsed;
                else appendDiagnostic(
                    *plan, module, asciiEqualIgnoreCase(key, "InitialPayload")
                        ? "invalid InitialPayload count"
                        : "invalid InitialRoster count");
            }
            count = std::min<uint32_t>(count, 1024u);
            for (uint32_t index = 0; index < count; ++index)
                rule.payloadTemplateNames.push_back(fields.front());
        };
        // RefCode stores one {template,count} value for each of these
        // fields. INI inheritance/override therefore uses the last value;
        // accumulating every historical value would duplicate old rosters.
        appendInitialPayload("InitialPayload");
        appendInitialPayload("InitialRoster");
        if (const container::String* value = moduleValueLast(module, "EnterSound"))
            rule.enterSound = *value;
        if (const container::String* value = moduleValueLast(module, "ExitSound"))
            rule.exitSound = *value;
        if (const container::String* value = moduleValueLast(module, "ExitBone"))
            rule.exitBone = *value;
        if (const container::String* value = moduleValueLast(module, "ParachuteOpenSound"))
            rule.parachuteOpenSound = *value;
        if (const container::String* value = moduleValueLast(module, "DamagePercentToUnits")) {
            if (const std::optional<math::q32_32> parsed = parsePercentFixed(*value)) {
                rule.damagePercentToUnits = *parsed;
            } else {
                appendDiagnostic(*plan, module, "invalid DamagePercentToUnits");
            }
        }
        if (const container::String* value = moduleValueLast(module, "TimeForFullHeal")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                rule.timeForFullHealMilliseconds = *parsed;
            } else {
                appendDiagnostic(*plan, module, "invalid TimeForFullHeal");
            }
        }
        if (const container::String* value = moduleValueLast(module, "ExitDelay")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                rule.exitDelayMilliseconds = *parsed;
            } else {
                appendDiagnostic(*plan, module, "invalid ExitDelay");
            }
        }
        if (const container::String* value = moduleValueLast(module, "DoorOpenTime")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                rule.doorOpenTimeMilliseconds = *parsed;
            } else {
                appendDiagnostic(*plan, module, "invalid DoorOpenTime");
            }
        }
        if (const container::String* value = moduleValueLast(module, "NumberOfExitPaths")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                // Explicit zero disables the art route; omission retains the
                // OpenContain constructor default of one.
                rule.numberOfExitPaths = *parsed;
            } else {
                appendDiagnostic(*plan, module, "invalid NumberOfExitPaths");
            }
        }
        if (const container::String* value = moduleValueLast(module, "HealAmountPerSecond")) {
            if (const std::optional<float> parsed =
                    parseContentFloat(*value, {
                        .source = __FILE__, .block = "Object",
                        .module = "Containment", .field = "Real"})) {
                rule.healAmountPerSecond = math::q32_32{std::max(0.0f, *parsed)};
            } else {
                appendDiagnostic(*plan, module, "invalid HealAmountPerSecond");
            }
        }
        if (const container::String* value = moduleValueLast(module, "HealthRegen%PerSec")) {
            container::StringView authored = trimAsciiView(*value);
            const bool hasPercentSuffix =
                !authored.empty() && authored.back() == '%';
            const std::optional<math::q32_32> parsed =
                hasPercentSuffix ? parsePercentFixed(authored)
                                 : parseFixed(authored);
            if (parsed) {
                // Percentage regeneration is resolved against each passenger's
                // maximum Body health in the fixed-tick update below. RefCode
                // parses this misleadingly named field as a plain Real and
                // divides by 100 at use time; accept an explicit modern '%'
                // suffix as the already-normalized spelling too.
                rule.healAmountPerSecond = hasPercentSuffix
                    ? -*parsed
                    : -(*parsed / math::q32_32{100});
            } else appendDiagnostic(*plan, module, "invalid HealthRegen%PerSec");
        }
        if (const container::String* value = moduleValueLast(module, "PassengersAllowedToFire")) {
            if (const std::optional<bool> parsed = parseBoolean(*value))
                rule.passengersAllowedToFire = *parsed;
            else
                appendDiagnostic(*plan, module, "invalid PassengersAllowedToFire");
        }
        if (const container::String* value = moduleValueLast(module, "ShouldDrawPips")) {
            if (const std::optional<bool> parsed = parseBoolean(*value))
                rule.shouldDrawPips = *parsed;
        }
        const auto parseTransportBoolean = [&](container::StringView key,
                                               bool& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<bool> parsed = parseBoolean(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module,
                                     "invalid " + container::String{key});
                }
            }
        };
        parseTransportBoolean("PassengersInTurret", rule.passengersInTurret);
        parseTransportBoolean("WeaponBonusPassedToPassengers",
                              rule.weaponBonusPassedToPassengers);
        parseTransportBoolean("AllowAlliesInside", rule.allowAlliesInside);
        parseTransportBoolean("AllowEnemiesInside", rule.allowEnemiesInside);
        parseTransportBoolean("AllowNeutralInside", rule.allowNeutralInside);
        parseTransportBoolean("BurnedDeathToUnits", rule.burnedDeathToUnits);
        parseTransportBoolean("ArmedRidersUpgradeMyWeaponSet",
                              rule.armedRidersUpgradeMyWeaponSet);
        parseTransportBoolean("MobileGarrison", rule.mobileGarrison);
        parseTransportBoolean("HealObjects", rule.healGarrisonObjects);
        parseTransportBoolean("IsEnclosingContainer",
                              rule.enclosingContainer);
        parseTransportBoolean("ScatterNearbyOnExit", rule.scatterNearbyOnExit);
        parseTransportBoolean("OrientLikeContainerOnExit",
                              rule.orientLikeContainerOnExit);
        parseTransportBoolean("KeepContainerVelocityOnExit",
                              rule.keepContainerVelocityOnExit);
        parseTransportBoolean("GoAggressiveOnExit", rule.goAggressiveOnExit);
        parseTransportBoolean("ResetMoodCheckTimeOnExit",
                              rule.resetMoodCheckTimeOnExit);
        parseTransportBoolean("DestroyRidersWhoAreNotFreeToExit",
                              rule.destroyRidersWhoAreNotFreeToExit);
        parseTransportBoolean("DelayExitInAir", rule.delayExitInAir);
        parseTransportBoolean("ImmuneToClearBuildingAttacks",
                              rule.immuneToClearBuildingAttacks);
        const auto parseFixedField = [&](container::StringView key,
                                         math::q32_32& destination,
                                         bool percent = false) {
            if (const container::String* value = moduleValueLast(module, key)) {
                const std::optional<math::q32_32> parsed = percent
                    ? parsePercentFixed(*value) : parseFixed(*value);
                if (parsed) destination = *parsed;
                else appendDiagnostic(*plan, module,
                                      "invalid " + container::String{key});
            }
        };
        parseFixedField("ExitPitchRate", rule.exitPitchRate);
        parseFixedField("PitchRateMax", rule.pitchRateMax);
        parseFixedField("RollRateMax", rule.rollRateMax);
        parseFixedField("LowAltitudeDamping", rule.lowAltitudeDamping);
        parseFixedField("ParachuteOpenDist", rule.parachuteOpenDistance);
        parseFixedField("FreeFallDamagePercent",
                        rule.freeFallDamageFraction, true);
        parseFixedField("KillWhenLandingInWaterSlop",
                        rule.killWhenLandingInWaterSlop);
        if (*kind == engine::ObjectContainmentKind::RiderChange) {
            for (uint32_t riderIndex = 1; riderIndex <= 8; ++riderIndex) {
                const container::String key = "Rider" +
                    std::to_string(riderIndex);
                const container::String* value = moduleValueLast(module, key);
                if (!value) continue;
                const container::Vector<container::String> fields =
                    parseOrderedTokens(*value);
                if (fields.size() != 6) {
                    appendDiagnostic(*plan, module,
                                     key + " requires six fields");
                    continue;
                }
                rule.riders.push_back({fields[0], fields[1], fields[2],
                                       fields[3], fields[4], fields[5]});
            }
            if (const container::String* value =
                    moduleValueLast(module, "ScuttleDelay")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.scuttleDelayMilliseconds = *parsed;
                } else appendDiagnostic(*plan, module,
                                        "invalid ScuttleDelay");
            }
            if (const container::String* value =
                    moduleValueLast(module, "ScuttleStatus"))
                rule.scuttleStatus = *value;
        }
        if (const container::String* value = moduleValueLast(module, "ExperienceSinkForRider")) {
            if (const std::optional<bool> parsed = parseBoolean(*value))
                rule.experienceSinkForRider = *parsed;
        }
        if (*kind == engine::ObjectContainmentKind::Garrison) {
            // A stationary enclosing garrison snaps entrants to its centre
            // once, but only MobileGarrison keeps doing transform projection.
            // Station-style (non-enclosing) occupants remain world objects and
            // must always track the host-side station transform.
            rule.followsContainerTransform =
                rule.mobileGarrison || !rule.enclosingContainer;
        } else if (*kind == engine::ObjectContainmentKind::Parachute) {
            // These are virtual ParachuteContain invariants in RefCode, not
            // tunable OpenContain module-data fields.
            rule.containMax = 1;
            rule.enclosingContainer = false;
        }
        plan->kindMask |= engine::objectContainmentKindBit(*kind);
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty() && plan->behaviorRules.empty()) return {};
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const engine::ObjectContainmentRule& left,
                 const engine::ObjectContainmentRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->behaviorRules.begin(), plan->behaviorRules.end(),
              [](const engine::ObjectTransportBehaviorRule& left,
                 const engine::ObjectTransportBehaviorRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
