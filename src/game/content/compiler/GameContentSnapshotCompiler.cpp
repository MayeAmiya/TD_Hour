#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "core/io/VFS.h"

#include "game/content/loader/GameDataRegistry.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/RankInfoCatalog.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/plan/economy/ObjectAutoDepositPlanTypes.h"
#include "game/object/plan/status/ObjectAutoHealPlanTypes.h"
#include "game/object/plan/combat/ObjectBoneFxPlanTypes.h"
#include "game/object/plan/lifecycle/ObjectCreatePlanTypes.h"
#include "game/object/plan/status/ObjectCrateCollidePlanTypes.h"
#include "game/object/plan/combat/ObjectCountermeasuresPlanTypes.h"
#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"
#include "game/object/plan/economy/ObjectEconomyPlanTypes.h"
#include "game/object/plan/structure/ObjectAirfieldPlanTypes.h"
#include "game/object/plan/structure/ObjectMinefieldPlanTypes.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"
#include "game/object/plan/combat/ObjectFireWeaponCollidePlanTypes.h"
#include "game/object/plan/combat/ObjectFireWeaponUpdatePlanTypes.h"
#include "game/object/plan/combat/ObjectFireUpdatesPlanTypes.h"
#include "game/object/plan/special/ObjectSpecialPowerPlanTypes.h"
#include "game/object/plan/world/ObjectSpyVisionPlanTypes.h"
#include "game/object/plan/combat/ObjectTacticalPlanTypes.h"
#include "game/object/plan/combat/ObjectStickyBombPlanTypes.h"
#include "game/object/plan/structure/ObjectMissileLauncherBuildingPlanTypes.h"
#include "game/object/plan/structure/ObjectParticleUplinkCannonPlanTypes.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/plan/combat/ObjectTransitionDamageFxPlanTypes.h"
#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"
#include "presentation/fx/runtime/LegacyBeamTemplate.h"
#include "presentation/fx/content/FxListCatalog.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include <variant>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;
[[nodiscard]] const container::String* moduleValue(
    const game::ModuleData& module, container::StringView key) noexcept;

void appendReferencedLocomotors(const game::ThingTemplate& templateData,
                                 container::HashSet<container::String>& names) {
    const auto append = [&names](const container::String& name) {
        if (!name.empty() && !asciiEqualIgnoreCase(name, "None")) {
            names.insert(name);
        }
    };
    append(templateData.locomotor);
    for (const container::String& name : templateData.locomotors) {
        append(name);
    }
    for (const game::LocomotorSetDefinition& set : templateData.locomotorSets) {
        for (const container::String& name : set.templates) {
            append(name);
        }
    }
}

void appendReferencedCommandSets(const game::ObjectArchetype& archetype,
                                container::HashSet<container::String>& names) {
    const game::ThingTemplate& templateData = archetype.templateData;
    if (!templateData.commandSet.empty()) names.insert(templateData.commandSet);
    // Rider command sets are optional runtime substitutions.  Shipped ZH
    // cinematic CombatBike objects name several sets which are intentionally
    // absent from CommandSet.ini; the original simply leaves those riders
    // without a command bar.  They therefore are not required references for
    // freezing the session-wide CommandSetStore.
    if (!archetype.objectUpgradePlan) return;
    for (const game::ObjectUpgradeRule& rule : archetype.objectUpgradePlan->rules) {
        if (rule.operation != game::ObjectUpgradeOperation::CommandSet) continue;
        if (!rule.commandSet.empty()) names.insert(rule.commandSet);
        if (!rule.commandSetAlt.empty()) names.insert(rule.commandSetAlt);
    }
}

void appendReferencedCombatTemplates(const game::CombatProfile& profile,
                                    container::HashSet<container::String>& weapons,
                                    container::HashSet<container::String>& armors) {
    for (const game::WeaponSetProfile& set : profile.weaponSets()) {
        for (const game::WeaponSlotProfile& slot : set.slots) {
            if (!slot.weaponTemplateName.empty()) weapons.insert(slot.weaponTemplateName);
        }
    }
    for (const game::ArmorSetProfile& set : profile.armorSets()) {
        if (!set.armorTemplateName.empty()) armors.insert(set.armorTemplateName);
    }
}

void appendReferencedBehaviorWeapons(
    const game::ObjectArchetype& archetype,
    container::HashSet<container::String>& weapons) {
    const auto append = [&weapons](const container::String& name) {
        if (name.empty() || asciiEqualIgnoreCase(name, "None")) return;
        weapons.insert(name);
    };
    if (archetype.physicsPlan) {
        if (!archetype.physicsPlan->crashIntoBuildingWeapon.empty()) {
            weapons.insert(
                archetype.physicsPlan->crashIntoBuildingWeapon);
        }
        if (!archetype.physicsPlan->crashIntoNonBuildingWeapon.empty()) {
            weapons.insert(
                archetype.physicsPlan->crashIntoNonBuildingWeapon);
        }
    }
    if (archetype.fireWeaponWhenDamagedPlan) {
        for (const game::ObjectFireWeaponWhenDamagedParameters& rule :
             archetype.fireWeaponWhenDamagedPlan->rules) {
            for (const container::String& name : rule.reactionWeapons) {
                if (!name.empty()) weapons.insert(name);
            }
            for (const container::String& name : rule.continuousWeapons) {
                if (!name.empty()) weapons.insert(name);
            }
        }
    }
    if (archetype.fireWeaponUpdatePlan) {
        for (const game::ObjectFireWeaponUpdateParameters& rule :
             archetype.fireWeaponUpdatePlan->rules) {
            if (!rule.weapon.empty()) weapons.insert(rule.weapon);
        }
    }
    if (archetype.fireWeaponCollidePlan) {
        for (const game::ObjectFireWeaponCollideRule& rule :
             archetype.fireWeaponCollidePlan->rules) {
            if (!rule.collideWeapon.empty()) {
                weapons.insert(rule.collideWeapon);
            }
        }
    }
    if (archetype.minefieldPlan) {
        for (const game::ObjectMinefieldRule& rule :
             archetype.minefieldPlan->mines) {
            if (!rule.detonationWeapon.empty()) {
                weapons.insert(rule.detonationWeapon);
            }
        }
        for (const game::ObjectDemoTrapRule& rule :
             archetype.minefieldPlan->demoTraps) {
            if (!rule.detonationWeapon.empty()) {
                weapons.insert(rule.detonationWeapon);
            }
        }
    }
    if (archetype.stickyBombPlan) {
        for (const game::ObjectStickyBombRule& rule :
             archetype.stickyBombPlan->rules) {
            if (!rule.geometryBasedDamageWeapon.empty()) {
                weapons.insert(rule.geometryBasedDamageWeapon);
            }
        }
    }
    for (const game::ModuleData& module : archetype.templateData.modules) {
        if (!asciiEqualIgnoreCase(
                module.moduleClass, "PointDefenseLaserUpdate")) continue;
        if (const container::String* weapon =
                moduleValue(module, "WeaponTemplate");
            weapon && !weapon->empty()) {
            weapons.insert(*weapon);
        }
    }
    // BunkerBusterBehavior names its two damage payloads only here.  Shipped
    // ZH authors StealthJetMissile with
    // ShockwaveWeaponTemplate = BunkerBusterShockwaveWeaponSmall and
    // OccupantDamageWeaponTemplate =
    // BunkerBusterAntiTunnelGarrisonWeaponWithABigName; `grep` over all
    // shipped INI finds each name exactly twice - its own Weapon.ini block
    // and this module - so no ObjectArchetype route can reach them.
    // DeliverPayloadAIUpdate's own VisiblePayloadWeaponTemplate is the
    // object-side twin of the OCL nugget field collected below; shipped data
    // only uses the nugget form, but the module route must not be a hole.
    if (archetype.containmentPlan) {
        for (const ObjectTransportBehaviorRule& rule :
             archetype.containmentPlan->behaviorRules) {
            append(rule.shockwaveWeapon);
            append(rule.occupantDamageWeapon);
            append(rule.visiblePayloadWeapon);
        }
    }
    // SpectreGunshipUpdate fires HowitzerWeaponTemplate as a transient weapon
    // through findWeaponId().  Shipped ZH also lists SpectreHowitzerGun in the
    // gunship's WeaponSet, so this is currently reachable by accident; the
    // collector must not depend on that coincidence.
    if (archetype.airfieldPlan) {
        for (const game::ObjectSpectreGunshipRule& rule :
             archetype.airfieldPlan->spectreGunships) {
            append(rule.howitzerWeaponTemplate);
        }
    }
    if (!archetype.deathReactionPlan) return;
    for (const game::ObjectDeathReactionRule& rule :
         archetype.deathReactionPlan->rules) {
        if (rule.kind ==
                game::ObjectDeathReactionKind::FireWeaponWhenDead &&
            rule.fireWeaponWhenDead &&
            !rule.fireWeaponWhenDead->deathWeapon.empty()) {
            weapons.insert(rule.fireWeaponWhenDead->deathWeapon);
        }
        if (rule.kind == game::ObjectDeathReactionKind::StructureTopple &&
            rule.structureTopple &&
            !rule.structureTopple->crushingWeapon.empty()) {
            weapons.insert(rule.structureTopple->crushingWeapon);
        }
        // SlowDeathBehavior's `Weapon = <PHASE> <name>` and
        // InstantDeathBehavior's `Weapon = <name>` payloads leave
        // ObjectSimulationDeath as names and are resolved by
        // GameSessionWeaponEventDrain::appendDeathPayloadWork through
        // findWeaponId().  For most shipped detonation weapons that field is
        // the *only* authored reference anywhere: GLADemoTrap /
        // Demo_GLADemoTrap / GC_Chem_GLADemoTrap (DemoTrapDetonationWeapon,
        // Demo_DemoTrapDetonationWeapon), every Fake* structure
        // (FakeStructureDetonationWeapon), RemoteC4Charge / TimedC4Charge
        // (BurtonC4ChargeWeapon), TNTStickyBomb (TNTDetonationWeapon),
        // MOABGas, DaisyCutterGas, SupW_AuroraFuelAirGas, AirF_AuroraBombGas,
        // ConvoyTruckArmedWithNuke and Barrel.  Omitting them left every one
        // of those detonations dealing no damage at all.
        if (rule.slowDeath) {
            for (const container::Vector<container::String>& phase :
                 rule.slowDeath->weapons) {
                for (const container::String& name : phase) append(name);
            }
        }
        if (rule.instantDeath) {
            for (const container::String& name : rule.instantDeath->weapons) {
                append(name);
            }
        }
    }
}

// A weapon named only by ObjectCreationList data has no ObjectArchetype
// reference anywhere, so the archetype walk above cannot see it.  In shipped ZH
// content the FireWeapon nugget (NeutronMissileWeapon,
// SupW_NeutronMissileWeapon, CruiseMissileWeapon) and DeliverPayload's
// VisiblePayloadWeaponTemplate (A10ThunderboltMissileWeapon,
// ArtilleryBarrageDamageWeapon) are the *only* authored references to those
// weapons.  Omitting them leaves the frozen weapon table without an entry, so
// every consumer's findWeaponId() returns INVALID and the whole superweapon
// chain (missile object, its flight, its slow-death detonation) silently
// becomes a no-op.
void appendReferencedObjectCreationListWeapons(
    const game::ObjectCreationListCatalog* objectCreationLists,
    container::HashSet<container::String>& weapons) {
    if (!objectCreationLists || !objectCreationLists->isLoaded()) return;
    const auto append = [&weapons](const container::String& name) {
        if (name.empty() || asciiEqualIgnoreCase(name, "None")) return;
        weapons.insert(name);
    };
    for (const game::ObjectCreationListDefinition& definition :
         objectCreationLists->all()) {
        for (const game::ObjectCreationNugget& nugget : definition.nuggets) {
            if (const game::ObjectCreationFireWeaponNugget* fire =
                    std::get_if<game::ObjectCreationFireWeaponNugget>(
                        &nugget)) {
                append(fire->weapon);
            } else if (const game::ObjectCreationDeliverPayloadNugget*
                           delivery =
                    std::get_if<game::ObjectCreationDeliverPayloadNugget>(
                        &nugget)) {
                append(delivery->visiblePayloadWeaponTemplate);
            }
        }
    }
}

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
}

void warnDegradedContent(container::String reason,
                         container::String definition = {},
                         container::String module = {},
                         container::String field = {},
                         container::String rawValue = {},
                         container::String adoptedValue = "disabled/no-op") {
    game::processContentDiagnostics().warn({
        .source = "GameContentSnapshot",
        .block = "FrozenContent",
        .definition = std::move(definition),
        .module = std::move(module),
        .field = std::move(field),
        .rawValue = std::move(rawValue),
        .adoptedValue = std::move(adoptedValue),
        .reason = std::move(reason),
    });
}

template <typename Validator>
void validateOrWarn(Validator&& validator, container::String module) {
    container::String diagnostic;
    if (validator(&diagnostic)) return;
    warnDegradedContent(
        diagnostic.empty() ? "content reference validation failed"
                           : std::move(diagnostic),
        {}, std::move(module));
}

class ReferenceValidationBatch final {
public:
    void warn(container::String reason, container::String definition,
              container::String module, container::String field,
              container::String rawValue) {
        warnDegradedContent(std::move(reason), std::move(definition),
                            std::move(module), std::move(field),
                            std::move(rawValue));
        ++m_warningCount;
    }

    [[nodiscard]] bool finish(container::String* error) const {
        if (m_warningCount == 0) return true;
        setError(error, std::to_string(m_warningCount) +
            " invalid content reference(s) were disabled/no-op; see individual warnings");
        return false;
    }

private:
    size_t m_warningCount = 0;
};

[[nodiscard]] const container::String* moduleValue(const game::ModuleData& module,
                                             container::StringView key) noexcept {
    for (const auto& [candidate, value] : module.values) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] float parseFiniteFloat(const container::String* source,
                                     float fallback) noexcept {
    if (!source || source->empty()) return fallback;
    return game::parseContentFloatOr(*source, {
        .source = __FILE__, .block = "GameContentSnapshot",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] uint32_t parseUnsigned(const container::String* source,
                                     uint32_t fallback) noexcept {
    const float value = parseFiniteFloat(source, static_cast<float>(fallback));
    if (value <= 0.0f) return 0;
    if (value >= static_cast<float>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] container::Array<float, 4> parseStreamColor(
    const container::String* source) noexcept {
    container::Array<float, 4> output{1.0f, 1.0f, 1.0f, 1.0f};
    if (!source) return output;
    size_t component = 0;
    for (size_t cursor = 0; cursor < source->size() && component < output.size();) {
        const size_t colon = source->find(':', cursor);
        if (colon == container::String::npos) break;
        char* end = nullptr;
        errno = 0;
        const float value = std::strtof(source->c_str() + colon + 1u, &end);
        if (end == source->c_str() + colon + 1u || errno == ERANGE ||
            !std::isfinite(value)) {
            warnDegradedContent(
                "ProjectileStream color component is not a finite Real; retained defaults for this and remaining components",
                {}, "ProjectileStream", "Color", *source,
                "remaining color components = 1.0");
            break;
        }
        if (value < 0.0f || value > 1020.0f) {
            warnDegradedContent(
                "ProjectileStream color component is outside the supported presentation range and was clamped",
                {}, "ProjectileStream", "Color", *source,
                std::to_string(std::clamp(value / 255.0f, 0.0f, 4.0f)));
        }
        output[component++] = std::clamp(value / 255.0f, 0.0f, 4.0f);
        cursor = static_cast<size_t>(end - source->c_str());
    }
    return output;
}

[[nodiscard]] game::ProjectileStreamBlendMode parseStreamBlend(
    const container::String* source) noexcept {
    if (!source || asciiEqualIgnoreCase(*source, "ADDITIVE")) {
        return game::ProjectileStreamBlendMode::Additive;
    }
    if (asciiEqualIgnoreCase(*source, "ALPHA")) {
        return game::ProjectileStreamBlendMode::Alpha;
    }
    if (asciiEqualIgnoreCase(*source, "MULTIPLY")) {
        return game::ProjectileStreamBlendMode::Multiply;
    }
    if (asciiEqualIgnoreCase(*source, "OPAQUE")) {
        return game::ProjectileStreamBlendMode::Opaque;
    }
    return game::ProjectileStreamBlendMode::Additive;
}

[[nodiscard]] game::ProjectileStreamDepthMode parseStreamDepth(
    const container::String* source) noexcept {
    if (!source || asciiEqualIgnoreCase(*source, "TEST_NO_WRITE") ||
        asciiEqualIgnoreCase(*source, "READ_ONLY")) {
        return game::ProjectileStreamDepthMode::TestNoWrite;
    }
    if (asciiEqualIgnoreCase(*source, "TEST_WRITE") ||
        asciiEqualIgnoreCase(*source, "READ_WRITE")) {
        return game::ProjectileStreamDepthMode::TestWrite;
    }
    if (asciiEqualIgnoreCase(*source, "DISABLED") ||
        asciiEqualIgnoreCase(*source, "NONE")) {
        return game::ProjectileStreamDepthMode::Disabled;
    }
    return game::ProjectileStreamDepthMode::TestNoWrite;
}

[[nodiscard]] bool compileProjectileStreamObjectDescriptor(
    const game::ThingTemplate& streamObject,
    game::ProjectileStreamRenderDescriptor& output,
    container::String* error) {
    output = {};
    const game::ModuleData* draw = nullptr;
    bool hasUpdate = false;
    for (const game::ModuleData& module : streamObject.modules) {
        if (asciiEqualIgnoreCase(module.moduleClass, "W3DProjectileStreamDraw")) {
            draw = &module;
        } else if (asciiEqualIgnoreCase(module.moduleClass, "ProjectileStreamUpdate")) {
            hasUpdate = true;
        }
    }
    if (!draw || !hasUpdate) {
        setError(error, "ProjectileStream Object '" + streamObject.name +
                        "' must provide W3DProjectileStreamDraw and ProjectileStreamUpdate");
        return false;
    }

    game::ProjectileStreamRenderDescriptor descriptor;
    if (const container::String* texture = moduleValue(*draw, "Texture")) {
        descriptor.texture = *texture;
    }
    descriptor.width = std::max(0.0f, parseFiniteFloat(moduleValue(*draw, "Width"), 0.0f));
    descriptor.tileFactor = std::max(
        0.0f, parseFiniteFloat(moduleValue(*draw, "TileFactor"), 1.0f));
    descriptor.scrollRate = parseFiniteFloat(moduleValue(*draw, "ScrollRate"), 0.0f);
    descriptor.maximumSegments = parseUnsigned(moduleValue(*draw, "MaxSegments"), 0);
    descriptor.segmentLifetimeSeconds = std::clamp(
        parseFiniteFloat(moduleValue(*draw, "SegmentLifetime"), 0.0f),
        0.0f, 10.0f);
    descriptor.color = parseStreamColor(moduleValue(*draw, "Color"));
    descriptor.blend = parseStreamBlend(moduleValue(*draw, "Blend"));
    descriptor.depth = parseStreamDepth(moduleValue(*draw, "Depth"));
    descriptor.enabled = !descriptor.texture.empty() && descriptor.width > 0.0f;
    if (!descriptor.enabled) {
        setError(error, "ProjectileStream Object '" + streamObject.name +
                        "' has no usable Texture/Width descriptor");
        return false;
    }
    output = std::move(descriptor);
    return true;
}

[[nodiscard]] bool compileProjectileStreamDescriptor(
    game::WeaponTemplate& weapon,
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    container::String* error) {
    weapon.projectileStream = {};
    if (weapon.projectileStreamName.empty()) return true;
    const auto streamObject = archetypes.find(weapon.projectileStreamName);
    if (streamObject == archetypes.end() || !streamObject->second) {
        setError(error, "Weapon '" + weapon.name + "' references missing ProjectileStream '" +
                        weapon.projectileStreamName + "'");
        return false;
    }
    container::String descriptorError;
    if (!compileProjectileStreamObjectDescriptor(
            streamObject->second->templateData, weapon.projectileStream,
            &descriptorError)) {
        setError(error, "Weapon '" + weapon.name + "': " + descriptorError);
        return false;
    }
    return true;
}

[[nodiscard]] bool validateObjectCreateReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const ScienceCatalog* sciences, const UpgradeCatalog* upgrades,
    container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->createPlan) continue;
        for (const game::ObjectCreateRule& rule : archetype->createPlan->rules) {
            if (const auto* grant =
                    std::get_if<game::ObjectGrantUpgradeCreate>(&rule.payload)) {
                if (!upgrades || !upgrades->find(grant->upgrade)) {
                    batch.warn(
                        "GrantUpgradeCreate references a missing Upgrade",
                        objectName, "GrantUpgradeCreate", "Upgrade",
                        grant->upgrade);
                }
                continue;
            }
            if (const auto* veterancy =
                    std::get_if<game::ObjectVeterancyGainCreate>(&rule.payload);
                veterancy && !veterancy->scienceRequired.empty() &&
                (!sciences || !sciences->find(veterancy->scienceRequired))) {
                batch.warn(
                    "VeterancyGainCreate references a missing Science",
                    objectName, "VeterancyGainCreate", "ScienceRequired",
                    veterancy->scienceRequired);
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectContainmentReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const UpgradeCatalog* upgrades, container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->containmentPlan) continue;
        for (const ObjectTransportBehaviorRule& rule :
             archetype->containmentPlan->behaviorRules) {
            if (rule.upgradeRequired.empty()) continue;
            if (!upgrades || !upgrades->find(rule.upgradeRequired)) {
                batch.warn(
                    "transport behavior references a missing Upgrade; behavior remains disabled",
                    objectName, "ObjectContainment", "UpgradeRequired",
                    rule.upgradeRequired);
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectDeathReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const UpgradeCatalog* upgrades,
    const ScienceCatalog* sciences,
    const game::CrateTemplateCatalog* crateTemplates,
    const game::ObjectCreationListCatalog* ocls,
    const SpecialPowerCatalog* specialPowers,
    container::String* error) {
    const bool strictCrateReferences = crateTemplates != nullptr;
    const bool strictSpecialPowerReferences = specialPowers != nullptr;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->deathReactionPlan) continue;
        for (const game::ObjectDeathReactionRule& rule :
             archetype->deathReactionPlan->rules) {
            const auto validateOclLists = [&](const auto& lists,
                                              container::StringView module) {
                if (!ocls) return;
                for (const auto& list : lists) {
                    for (const container::String& name : list) {
                        if (name.empty() || ocls->findId(name)) continue;
                        batch.warn(
                            "death reaction references a missing ObjectCreationList",
                            objectName, container::String{module}, "OCL", name);
                    }
                }
            };
            if (rule.structureTopple) {
                validateOclLists(rule.structureTopple->ocls,
                                 "StructureToppleUpdate");
            }
            if (rule.structureCollapse) {
                validateOclLists(rule.structureCollapse->ocls,
                                 "StructureCollapseUpdate");
            }
            if (rule.kind ==
                    game::ObjectDeathReactionKind::SpecialPowerCompletion &&
                rule.specialPowerCompletionDie) {
                const container::String& name =
                    rule.specialPowerCompletionDie->specialPowerTemplate;
                if (strictSpecialPowerReferences &&
                    (name.empty() || !specialPowers->find(name))) {
                    batch.warn(
                        "SpecialPowerCompletionDie references a missing SpecialPower",
                        objectName, "SpecialPowerCompletionDie",
                        "SpecialPowerTemplate", name);
                }
                continue;
            }
            if (rule.kind == game::ObjectDeathReactionKind::CreateCrate &&
                rule.createCrateDie && strictCrateReferences) {
                for (const container::String& crateData :
                     rule.createCrateDie->crateData) {
                    if (!crateTemplates->find(crateData)) {
                        batch.warn(
                            "CreateCrateDie references missing CrateData",
                            objectName, "CreateCrateDie", "CrateData",
                            crateData);
                    }
                }
                continue;
            }
            if (rule.kind != game::ObjectDeathReactionKind::Upgrade ||
                !rule.upgradeDie) continue;
            const UpgradeDefinition* definition = upgrades
                ? upgrades->find(rule.upgradeDie->upgradeToRemove)
                : nullptr;
            if (!definition) {
                batch.warn("UpgradeDie references a missing Upgrade",
                           objectName, "UpgradeDie", "UpgradeToRemove",
                           rule.upgradeDie->upgradeToRemove);
                continue;
            }
            if (definition->type != UpgradeDefinitionType::Object) {
                batch.warn("UpgradeDie must reference an OBJECT Upgrade",
                           objectName, "UpgradeDie", "UpgradeToRemove",
                           definition->name);
            }
        }
    }
    if (!strictCrateReferences) return batch.finish(error);
    for (const game::CrateTemplateDefinition& crate :
         crateTemplates->definitions()) {
        if (!crate.killerScience.empty() &&
            (!sciences || !sciences->find(crate.killerScience))) {
            batch.warn("CrateData references a missing KillerScience",
                       crate.name, "CrateData", "KillerScience",
                       crate.killerScience);
        }
        for (const game::CrateObjectChoice& choice : crate.possibleCrates) {
            if (choice.objectTemplate.empty() ||
                archetypes.find(choice.objectTemplate) == archetypes.end()) {
                batch.warn("CrateData references a missing CrateObject",
                           crate.name, "CrateData", "CrateObject",
                           choice.objectTemplate);
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectAutoDepositReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const UpgradeCatalog* upgrades, container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->autoDepositPlan) continue;
        for (const game::ObjectAutoDepositParameters& rule :
             archetype->autoDepositPlan->rules) {
            for (const game::ObjectAutoDepositBoost& boost :
                 rule.upgradedBoosts) {
                const UpgradeDefinition* definition = upgrades
                    ? upgrades->find(boost.upgrade)
                    : nullptr;
                if (!definition) {
                    batch.warn("AutoDepositUpdate references a missing Upgrade",
                               objectName, "AutoDepositUpdate", "Upgrade",
                               boost.upgrade);
                    continue;
                }
                if (definition->type != UpgradeDefinitionType::Player) {
                    batch.warn(
                        "AutoDepositUpdate UpgradedBoost must reference a PLAYER Upgrade",
                        objectName, "AutoDepositUpdate", "UpgradedBoost",
                        definition->name);
                }
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectCountermeasureReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->countermeasuresPlan) continue;
        for (const game::ObjectCountermeasuresRule& rule :
             archetype->countermeasuresPlan->rules) {
            if (!rule.flareTemplate.empty() &&
                archetypes.find(rule.flareTemplate) != archetypes.end()) {
                continue;
            }
            batch.warn("CountermeasuresBehavior references a missing flare object",
                       objectName, "CountermeasuresBehavior", "FlareTemplate",
                       rule.flareTemplate);
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectMinefieldReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const game::ObjectCreationListCatalog* creationLists,
    container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->minefieldPlan) continue;
        for (const game::ObjectGenerateMinefieldRule& rule :
             archetype->minefieldPlan->generators) {
            if (rule.mineName.empty() ||
                archetypes.find(rule.mineName) == archetypes.end()) {
                batch.warn("GenerateMinefieldBehavior references a missing mine object",
                           objectName, "GenerateMinefieldBehavior", "MineName",
                           rule.mineName);
            }
            if (rule.upgradable &&
                (rule.upgradedMineName.empty() ||
                 archetypes.find(rule.upgradedMineName) == archetypes.end())) {
                batch.warn(
                    "GenerateMinefieldBehavior references a missing upgraded mine object",
                    objectName, "GenerateMinefieldBehavior", "UpgradedMineName",
                    rule.upgradedMineName);
            }
        }
        for (const game::ObjectMinefieldRule& rule :
             archetype->minefieldPlan->mines) {
            if (!rule.creationList.empty() &&
                (!creationLists || !creationLists->find(rule.creationList))) {
                batch.warn("MinefieldBehavior references a missing ObjectCreationList",
                           objectName, "MinefieldBehavior", "ObjectCreationList",
                           rule.creationList);
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectCrateCollideReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const ScienceCatalog* sciences, const UpgradeCatalog* upgrades,
    container::String* error) {
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->crateCollidePlan) continue;
        for (const game::ObjectCrateCollideRule& rule :
             archetype->crateCollidePlan->rules) {
            if (!rule.pickupScience.empty() &&
                (!sciences || !sciences->find(rule.pickupScience))) {
                batch.warn("CrateCollide references a missing Science",
                           objectName, "CrateCollide", "Science",
                           rule.pickupScience);
            }
            for (const game::ObjectCrateUpgradeBoost& boost : rule.upgradedBoosts) {
                const UpgradeDefinition* definition = upgrades
                    ? upgrades->find(boost.upgrade) : nullptr;
                if (!definition) {
                    batch.warn("MoneyCrateCollide references a missing Upgrade",
                               objectName, "MoneyCrateCollide", "Upgrade",
                               boost.upgrade);
                    continue;
                }
                if (definition->type != UpgradeDefinitionType::Player) {
                    batch.warn(
                        "MoneyCrateCollide UpgradedBoost must reference a PLAYER Upgrade",
                        objectName, "MoneyCrateCollide", "UpgradedBoost",
                        definition->name);
                }
            }
            if (rule.kind == game::ObjectCrateCollideKind::Unit &&
                (rule.unitName.empty() || archetypes.find(rule.unitName) == archetypes.end())) {
                batch.warn("UnitCrateCollide references a missing Object",
                           objectName, "UnitCrateCollide", "UnitName",
                           rule.unitName);
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateTransitionDamageFxReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const game::ObjectCreationListCatalog* ocls,
    container::String* error) {
    const bool strict = ocls != nullptr;
    if (!strict) return true;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->transitionDamageFxPlan) continue;
        for (const game::ObjectTransitionDamageFxRule& rule :
             archetype->transitionDamageFxPlan->rules) {
            for (const auto& state : rule.entries) {
                for (const game::ObjectTransitionDamageFxEntry& entry : state) {
                    if (entry.kind != game::ObjectTransitionDamageFxPayloadKind::ObjectCreationList ||
                        entry.resource.empty()) continue;
                    if (!ocls->findId(entry.resource)) {
                        batch.warn(
                            "TransitionDamageFX references a missing ObjectCreationList",
                            objectName, "TransitionDamageFX", "OCL",
                            entry.resource);
                    }
                }
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateBoneFxReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const game::ObjectCreationListCatalog* ocls,
    container::String* error) {
    const bool strict = ocls != nullptr;
    if (!strict) return true;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->boneFxPlan) continue;
        for (const game::ObjectBoneFxRule& rule : archetype->boneFxPlan->rules) {
            for (const auto& state : rule.entries) {
                for (const game::ObjectBoneFxEntry& entry : state) {
                    if (entry.kind !=
                            game::ObjectBoneFxPayloadKind::ObjectCreationList ||
                        entry.resource.empty()) {
                        continue;
                    }
                    if (!ocls->findId(entry.resource)) {
                        batch.warn(
                            "BoneFXUpdate references a missing ObjectCreationList",
                            objectName, "BoneFXUpdate", "OCL",
                            entry.resource);
                    }
                }
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectUpgradeReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const game::ObjectCreationListCatalog* ocls,
    container::String* error) {
    const bool strictOcls = ocls != nullptr;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->objectUpgradePlan) continue;
        for (const game::ObjectUpgradeRule& rule :
             archetype->objectUpgradePlan->rules) {
            if (rule.operation ==
                    game::ObjectUpgradeOperation::ObjectCreation) {
                if (strictOcls && !rule.objectCreationList.empty() &&
                    !ocls->findId(rule.objectCreationList)) {
                    batch.warn(
                        "ObjectCreationUpgrade references a missing ObjectCreationList",
                        objectName, "ObjectCreationUpgrade",
                        "ObjectCreationList", rule.objectCreationList);
                }
            } else if (rule.operation ==
                           game::ObjectUpgradeOperation::ReplaceObject) {
                if (rule.replacementObject.empty() ||
                    archetypes.find(rule.replacementObject) ==
                        archetypes.end()) {
                    batch.warn("ReplaceObjectUpgrade references a missing object",
                               objectName, "ReplaceObjectUpgrade",
                               "ReplacementObject", rule.replacementObject);
                }
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateObjectSpecialPowerReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const SpecialPowerCatalog* specialPowers,
    const ScienceCatalog* sciences,
    const game::ObjectCreationListCatalog* ocls,
    container::String* error) {
    const bool strictPowers = specialPowers != nullptr;
    const bool strictSciences = sciences && sciences->isLoaded();
    const bool strictOcls = ocls != nullptr;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype) continue;
        if (archetype->specialPowerPlan) {
          for (const game::ObjectSpecialPowerRule& rule :
               archetype->specialPowerPlan->rules) {
            const SpecialPowerDefinition* definition = specialPowers
                ? specialPowers->find(rule.specialPowerTemplate) : nullptr;
            if (!definition) {
                if (strictPowers) {
                    batch.warn("object module references a missing SpecialPower",
                               objectName, rule.moduleClass,
                               "SpecialPowerTemplate",
                               rule.specialPowerTemplate);
                }
                continue;
            }
            if (!definition->requiredScience.empty() &&
                !asciiEqualIgnoreCase(definition->requiredScience, "None") &&
                !asciiEqualIgnoreCase(definition->requiredScience,
                                      "SCIENCE_INVALID") &&
                strictSciences &&
                !sciences->find(definition->requiredScience)) {
                batch.warn("SpecialPower requires a missing Science",
                           definition->name, "SpecialPower",
                           "RequiredScience", definition->requiredScience);
            }
            if (rule.kind ==
                    game::ObjectSpecialPowerKind::ObjectCreationList) {
                if (strictOcls && !rule.objectCreationList.empty() &&
                    !ocls->findId(rule.objectCreationList)) {
                    batch.warn(
                        "OCLSpecialPower references a missing ObjectCreationList",
                        objectName, "OCLSpecialPower", "ObjectCreationList",
                        rule.objectCreationList);
                }
                for (const game::ObjectSpecialPowerUpgradeOcl& upgrade :
                     rule.upgradeObjectCreationLists) {
                    if (strictSciences &&
                        !sciences->find(upgrade.science)) {
                        batch.warn(
                            "OCLSpecialPower UpgradeOCL references a missing Science",
                            objectName, "OCLSpecialPower", "UpgradeScience",
                            upgrade.science);
                    }
                    if (strictOcls &&
                        !ocls->findId(upgrade.objectCreationList)) {
                        batch.warn(
                            "OCLSpecialPower UpgradeOCL references a missing ObjectCreationList",
                            objectName, "OCLSpecialPower",
                            "UpgradeObjectCreationList",
                            upgrade.objectCreationList);
                    }
                }
                if (!rule.referenceObject.empty() &&
                    archetypes.find(rule.referenceObject) ==
                        archetypes.end()) {
                    batch.warn("OCLSpecialPower references a missing object",
                               objectName, "OCLSpecialPower",
                               "ReferenceObject", rule.referenceObject);
                }
            }
            if (rule.kind == game::ObjectSpecialPowerKind::CashHack) {
                for (const game::ObjectSpecialPowerUpgradeMoney& upgrade :
                     rule.upgradeMoneyAmounts) {
                    if (strictSciences &&
                        !sciences->find(upgrade.science)) {
                        batch.warn(
                            "CashHackSpecialPower UpgradeMoneyAmount references a missing Science",
                            objectName, "CashHackSpecialPower",
                            "UpgradeScience", upgrade.science);
                    }
                }
            }
            if (rule.kind ==
                    game::ObjectSpecialPowerKind::BaikonurLaunch &&
                (rule.detonationObject.empty() ||
                 archetypes.find(rule.detonationObject) ==
                     archetypes.end())) {
                batch.warn("BaikonurLaunchPower references a missing object",
                           objectName, "BaikonurLaunchPower",
                           "DetonationObject", rule.detonationObject);
            }
          }
        }
        if (archetype->missileLauncherBuildingPlan && strictPowers) {
            for (const game::ObjectMissileLauncherBuildingRule& rule :
                 archetype->missileLauncherBuildingPlan->rules) {
                if (!specialPowers->find(rule.specialPowerTemplate)) {
                    batch.warn(
                        "MissileLauncherBuildingUpdate references a missing SpecialPower",
                        objectName, "MissileLauncherBuildingUpdate",
                        "SpecialPowerTemplate", rule.specialPowerTemplate);
                }
            }
        }
        if (archetype->objectUpgradePlan) {
            for (const game::ObjectUpgradeRule& rule :
                 archetype->objectUpgradePlan->rules) {
                if (rule.operation !=
                        game::ObjectUpgradeOperation::UnpauseSpecialPower ||
                    !strictPowers) {
                    continue;
                }
                if (!specialPowers->find(rule.specialPowerTemplate)) {
                    batch.warn(
                        "UnpauseSpecialPowerUpgrade references a missing SpecialPower",
                        objectName, "UnpauseSpecialPowerUpgrade",
                        "SpecialPowerTemplate", rule.specialPowerTemplate);
                }
            }
        }
    }
    return batch.finish(error);
}

[[nodiscard]] bool validateParticleUplinkReferences(
    const container::HashMap<container::String,
        container::SharedPtr<const game::ObjectArchetype>>& archetypes,
    const SpecialPowerCatalog* specialPowers,
    const fx::LegacyBeamTemplateCatalog& beams,
    container::String* error) {
    const bool strictPowers = specialPowers != nullptr;
    ReferenceValidationBatch batch;
    for (const auto& [objectName, archetype] : archetypes) {
        if (!archetype || !archetype->particleUplinkCannonPlan) continue;
        for (const game::ObjectParticleUplinkCannonRule& rule :
             archetype->particleUplinkCannonPlan->rules) {
            if (strictPowers &&
                !specialPowers->find(rule.specialPowerTemplate)) {
                batch.warn(
                    "ParticleUplinkCannonUpdate references a missing SpecialPower",
                    objectName, "ParticleUplinkCannonUpdate",
                    "SpecialPowerTemplate", rule.specialPowerTemplate);
            }
            const auto beam = beams.find(rule.particleBeamLaserName);
            if (beam == beams.end() ||
                beam->second.kind != fx::LegacyBeamTemplateKind::Laser) {
                batch.warn(
                    "ParticleUplinkCannonUpdate references a missing or non-laser beam",
                    objectName, "ParticleUplinkCannonUpdate",
                    "ParticleBeamLaserName", rule.particleBeamLaserName);
            }
            const auto validateConnector = [&](
                container::StringView name,
                container::StringView field) {
                if (name.empty()) return;
                const auto found = beams.find(container::String{name});
                if (found != beams.end() &&
                    found->second.kind == fx::LegacyBeamTemplateKind::Laser) {
                    return;
                }
                batch.warn(
                    "ParticleUplinkCannonUpdate references a missing or non-laser connector",
                    objectName, "ParticleUplinkCannonUpdate",
                    container::String{field}, container::String{name});
            };
            validateConnector(rule.connectorMediumLaserName,
                              "ConnectorMediumLaserName");
            validateConnector(rule.connectorIntenseLaserName,
                              "ConnectorIntenseLaserName");
            if (!rule.damagePulseRemnantObjectName.empty() &&
                archetypes.find(rule.damagePulseRemnantObjectName) ==
                    archetypes.end()) {
                batch.warn(
                    "ParticleUplinkCannonUpdate references a missing remnant object",
                    objectName, "ParticleUplinkCannonUpdate",
                    "DamagePulseRemnantObjectName",
                    rule.damagePulseRemnantObjectName);
            }
        }
    }
    return batch.finish(error);
}

} // namespace

bool compileProjectileStreamRenderDescriptor(
    const game::ThingTemplate& streamObject,
    game::ProjectileStreamRenderDescriptor& output,
    container::String* error) {
    if (error) error->clear();
    return compileProjectileStreamObjectDescriptor(streamObject, output, error);
}

bool GameContentSnapshot::capture(const GameDataRegistry& source, container::String* error) {
    return capture(source, {}, {}, error);
}

bool GameContentSnapshot::capture(
    const GameDataRegistry& source, container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
    container::String* error) {
    return capture(source, {}, std::move(upgradeCatalogOverride), error);
}

bool GameContentSnapshot::capture(
    const GameDataRegistry& source,
    container::SharedPtr<const ScienceCatalog> scienceCatalogOverride,
    container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
    container::String* error) {
    return captureFromStores(
        source, game::ThingFactory::instance(), game::LocomotorStore::instance(),
        game::WeaponStore::instance(), game::ArmorStore::instance(),
        game::CommandSetStore::instance(), game::CommandButtonStore::instance(),
        nullptr, false, {}, std::move(scienceCatalogOverride), {},
        std::move(upgradeCatalogOverride), {}, {}, {}, {}, {}, {}, error);
}

bool GameContentSnapshot::captureWithGameplayModifiers(
    const GameDataRegistry& source,
    container::Span<const container::String> modifierPaths,
    container::SharedPtr<const ScienceCatalog> scienceCatalogOverride,
    container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
    container::String* error) {
    if (modifierPaths.empty()) {
        return capture(
            source, std::move(scienceCatalogOverride),
            std::move(upgradeCatalogOverride), error);
    }
    game::ThingFactory things = game::ThingFactory::instance();
    game::LocomotorStore locomotors = game::LocomotorStore::instance();
    game::WeaponStore weapons = game::WeaponStore::instance();
    game::ArmorStore armors = game::ArmorStore::instance();
    game::CommandSetStore commandSets = game::CommandSetStore::instance();
    game::CommandButtonStore commandButtons = game::CommandButtonStore::instance();
    game::WeaponBonusSet globalWeaponBonuses = source.globalWeaponBonuses();
    bool objectRecipesChanged = false;
    for (const container::String& path : modifierPaths) {
        game::GeneralsIniParser modifierParser;
        const container::String modifierContent =
            io::VFS::instance().readAll(path);
        if (!modifierParser.parse(modifierContent)) {
            if (error) *error = "session gameplay modifier parse failed: " + path;
            return false;
        }
        container::String weaponBonusError;
        if (!globalWeaponBonuses.applyLegacyGameDataOverrides(
                modifierContent, path, &weaponBonusError)) {
            if (error) {
                *error = "session WeaponBonus modifier failed for '" + path +
                    "': " + weaponBonusError;
            }
            return false;
        }
        objectRecipesChanged = objectRecipesChanged || std::any_of(
            modifierParser.blocks().begin(), modifierParser.blocks().end(),
            [](const game::IniBlock& block) {
                return container::asciiEqualIgnoreCase(
                           block.type, "Object") ||
                    container::asciiEqualIgnoreCase(
                           block.type, "ObjectReskin");
            });
        if (path.empty() || !things.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides) ||
            !locomotors.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides) ||
            !weapons.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides) ||
            !armors.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides) ||
            !commandSets.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides) ||
            !commandButtons.loadFromIni(
                path, game::ini::LegacyIniLoadType::CreateOverrides)) {
            if (error) {
                *error = "session gameplay modifier parse failed: " + path;
            }
            return false;
        }
    }
    container::SharedPtr<const ScienceCatalog> baseScience =
        scienceCatalogOverride ? scienceCatalogOverride
                               : source.scienceCatalogSnapshot();
    container::SharedPtr<const RankInfoCatalog> baseRankInfo =
        source.rankInfoCatalogSnapshot();
    container::SharedPtr<const SpecialPowerCatalog> baseSpecialPower =
        source.specialPowerCatalogSnapshot();
    container::SharedPtr<const UpgradeCatalog> baseUpgrade =
        upgradeCatalogOverride ? upgradeCatalogOverride
                               : source.upgradeCatalogSnapshot();
    auto science = baseScience
        ? std::make_shared<ScienceCatalog>(*baseScience) : nullptr;
    auto rankInfo = baseRankInfo
        ? std::make_shared<RankInfoCatalog>(*baseRankInfo) : nullptr;
    auto specialPower = baseSpecialPower
        ? std::make_shared<SpecialPowerCatalog>(*baseSpecialPower) : nullptr;
    auto upgrade = baseUpgrade
        ? std::make_shared<UpgradeCatalog>(*baseUpgrade) : nullptr;
    const auto baseObjectCreationLists =
        source.objectCreationListCatalogSnapshot();
    const auto baseCrates = source.crateTemplateCatalogSnapshot();
    const auto baseDamageFx = source.damageFxCatalogSnapshot();
    const auto baseFxLists = source.fxListCatalogSnapshot();
    const auto baseParticles = source.particleSystemCatalogSnapshot();
    auto objectCreationLists = baseObjectCreationLists
        ? std::make_shared<game::ObjectCreationListCatalog>(
              *baseObjectCreationLists)
        : nullptr;
    auto crates = baseCrates
        ? std::make_shared<game::CrateTemplateCatalog>(*baseCrates)
        : nullptr;
    auto damageFx = baseDamageFx
        ? std::make_shared<game::DamageFxCatalog>(*baseDamageFx)
        : nullptr;
    auto fxLists = baseFxLists
        ? std::make_shared<fx::FxListCatalog>(*baseFxLists)
        : nullptr;
    auto particles = baseParticles
        ? std::make_shared<fx::ParticleSystemCatalog>(*baseParticles)
        : std::make_shared<fx::ParticleSystemCatalog>();
    container::SharedPtr<game::W3dPristineBoneCatalog> pristineBones;
    if (objectRecipesChanged) {
        pristineBones = std::make_shared<game::W3dPristineBoneCatalog>();
        container::String pristineError;
        if (!pristineBones->build(things, &pristineError)) {
            if (error) {
                *error = "session gameplay modifier pristine-bone rebuild failed: " +
                    pristineError;
            }
            return false;
        }
    }
    container::String catalogError;
    for (const container::String& path : modifierPaths) {
        if ((rankInfo &&
             !rankInfo->applyOverridesFromVfs(path, &catalogError)) ||
            (science &&
             !science->applyOverridesFromVfs(path, &catalogError)) ||
            (specialPower &&
             !specialPower->applyOverridesFromVfs(path, &catalogError)) ||
            (upgrade &&
             !upgrade->applyOverridesFromVfs(path, &catalogError)) ||
            (objectCreationLists &&
             !objectCreationLists->applyOverridesFromVfs(
                 path, &catalogError)) ||
            (crates && !crates->applyOverridesFromVfs(
                           path, &catalogError)) ||
            (damageFx && !damageFx->applyOverridesFromVfs(
                              path, &catalogError)) ||
            (particles && !particles->applyOverridesFromVfs(
                              path, &catalogError)) ||
            (fxLists && !fxLists->applyOverridesFromVfs(
                            path, &catalogError))) {
            if (error) {
                *error = "session gameplay catalog override failed for '" +
                    path + "': " + catalogError;
            }
            return false;
        }
    }
    if (fxLists) {
        fxLists->resolveReferences(*particles);
    }
    if (damageFx && fxLists) {
        damageFx->resolveFxReferences(*fxLists);
    }
    return captureFromStores(
        source, things, locomotors, weapons, armors, commandSets,
        commandButtons, &globalWeaponBonuses, true, std::move(rankInfo),
        std::move(science),
        std::move(specialPower),
        std::move(upgrade), std::move(objectCreationLists),
        std::move(crates), std::move(damageFx),
        std::move(fxLists),
        std::move(particles),
        std::move(pristineBones), error);
}

bool GameContentSnapshot::captureFromStores(
    const GameDataRegistry& source, game::ThingFactory& things,
    const game::LocomotorStore& locomotorStore,
    const game::WeaponStore& weaponStore, const game::ArmorStore& armorStore,
    const game::CommandSetStore& commandSetStore,
    const game::CommandButtonStore& commandButtonStore,
    const game::WeaponBonusSet* globalWeaponBonusesOverride,
    bool allowSessionModifierCatalogs,
    container::SharedPtr<const RankInfoCatalog> rankInfoCatalogOverride,
    container::SharedPtr<const ScienceCatalog> scienceCatalogOverride,
    container::SharedPtr<const SpecialPowerCatalog>
        specialPowerCatalogOverride,
    container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
    container::SharedPtr<const game::ObjectCreationListCatalog>
        objectCreationListCatalogOverride,
    container::SharedPtr<const game::CrateTemplateCatalog>
        crateTemplateCatalogOverride,
    container::SharedPtr<const game::DamageFxCatalog>
        damageFxCatalogOverride,
    container::SharedPtr<const fx::FxListCatalog> fxListCatalogOverride,
    container::SharedPtr<const fx::ParticleSystemCatalog>
        particleSystemCatalogOverride,
    container::SharedPtr<const game::W3dPristineBoneCatalog>
        pristineBoneCatalogOverride,
    container::String* error) {
    if (error) error->clear();
    clear();

    // Direct tools and future map-level Object overrides may use ThingFactory
    // without going through GameDataLoader::loadAll().  Snapshot capture is
    // the sealing boundary, so make the reverse prerequisite classification
    // current before copying immutable archetypes.
    things.finalizeDerivedMetadata();

    // Acquired before the weapon table is frozen: OCL nuggets are a first-class
    // source of weapon references, so the catalog has to be in hand while the
    // referenced-weapon set is still open.
    container::SharedPtr<const game::ObjectCreationListCatalog>
        objectCreationLists = objectCreationListCatalogOverride
            ? std::move(objectCreationListCatalogOverride)
            : source.objectCreationListCatalogSnapshot();

    // ThingFactory exposes its stable source-name set but not a mutable
    // archetype container.  Resolve each name through GameDataRegistry, then
    // deep-copy the compiled immutable output before publishing the snapshot.
    const auto& sourceThings = things.all();
    container::HashMap<container::String, container::SharedPtr<const game::ObjectArchetype>> archetypes;
    archetypes.reserve(sourceThings.size());
    container::HashSet<container::String> referencedLocomotors;
    container::HashSet<container::String> referencedWeapons;
    container::HashSet<container::String> referencedArmors;
    container::HashSet<container::String> referencedCommandSets;
    for (const auto& [name, ignoredTemplate] : sourceThings) {
        static_cast<void>(ignoredTemplate);
        const container::SharedPtr<const game::ObjectArchetype> sourceArchetype =
            things.findArchetype(name);
        if (!sourceArchetype) {
            warnDegradedContent(
                "ThingFactory entry has no compiled ObjectArchetype",
                name, "ThingFactory", "ObjectArchetype", name,
                "object omitted");
            continue;
        }

        auto frozenArchetype = std::make_shared<const game::ObjectArchetype>(*sourceArchetype);
        appendReferencedLocomotors(frozenArchetype->templateData, referencedLocomotors);
        appendReferencedCommandSets(*frozenArchetype, referencedCommandSets);
        if (frozenArchetype->combatProfile) {
            appendReferencedCombatTemplates(*frozenArchetype->combatProfile,
                                            referencedWeapons, referencedArmors);
        }
        appendReferencedBehaviorWeapons(*frozenArchetype, referencedWeapons);
        archetypes.emplace(name, std::move(frozenArchetype));
    }

    container::HashMap<container::String, game::FrozenLocomotorTemplate>
        locomotors;
    locomotors.reserve(referencedLocomotors.size());
    for (const container::String& name : referencedLocomotors) {
        if (const game::LocomotorTemplate* sourceLocomotor = locomotorStore.find(name)) {
            locomotors.emplace(
                name, game::freezeLocomotorTemplate(*sourceLocomotor));
        } else {
            warnDegradedContent(
                "Object recipe references a missing Locomotor",
                {}, "LocomotorStore", "Locomotor", name);
        }
    }

    appendReferencedObjectCreationListWeapons(
        objectCreationLists.get(), referencedWeapons);

    // HistoricBonusWeapon is a content dependency just like a projectile or
    // OCL weapon. Close the graph before assigning compact session IDs so a
    // bonus weapon can itself name another bonus weapon without retaining a
    // mutable WeaponStore lookup in simulation.
    bool addedHistoricDependency = true;
    while (addedHistoricDependency) {
        addedHistoricDependency = false;
        container::Vector<container::String> current{
            referencedWeapons.begin(), referencedWeapons.end()};
        for (const container::String& name : current) {
            const game::WeaponAuthoringTemplate* weapon =
                weaponStore.find(name);
            if (!weapon || weapon->historicBonusWeaponName.empty()) continue;
            addedHistoricDependency |= referencedWeapons.insert(
                weapon->historicBonusWeaponName).second;
        }
    }

    container::Vector<container::String> weaponNames{referencedWeapons.begin(), referencedWeapons.end()};
    std::sort(weaponNames.begin(), weaponNames.end());
    container::Vector<game::WeaponTemplate> weapons;
    weapons.reserve(weaponNames.size());
    container::HashMap<container::String, game::WeaponContentId> weaponIds;
    weaponIds.reserve(weaponNames.size());
    for (const container::String& name : weaponNames) {
        const game::WeaponAuthoringTemplate* sourceWeapon = weaponStore.find(name);
        if (!sourceWeapon) {
            warnDegradedContent(
                "Object recipe references a missing Weapon",
                {}, "WeaponStore", "Weapon", name);
            continue;
        }
        // The snapshot is the final immutable content boundary. Rebuild the
        // canonical fixed block here as well so fixture/custom stores that
        // directly authored compatibility floats cannot leak float values
        // into confirmed simulation.
        game::WeaponAuthoringTemplate finalized = *sourceWeapon;
        finalized.synchronizeAuthoritativeScalars();
        game::WeaponTemplate weapon =
            static_cast<const game::WeaponTemplate&>(finalized);
        container::String diagnostic;
        if (!compileProjectileStreamDescriptor(
                weapon, archetypes, &diagnostic)) {
            warnDegradedContent(
                diagnostic.empty()
                    ? "ProjectileStream descriptor could not be compiled"
                    : std::move(diagnostic),
                weapon.name, "ProjectileStream", "ProjectileStream",
                weapon.projectileStreamName);
        }
        const uint32_t nextId = static_cast<uint32_t>(weapons.size() + 1);
        weapons.push_back(std::move(weapon));
        weaponIds.emplace(name, game::WeaponContentId{.value = nextId});
    }
    for (game::WeaponTemplate& weapon : weapons) {
        if (weapon.historicBonusWeaponName.empty()) continue;
        const auto found = weaponIds.find(weapon.historicBonusWeaponName);
        if (found != weaponIds.end()) {
            weapon.historicBonusWeapon = found->second;
        } else {
            warnDegradedContent(
                "Weapon references a missing HistoricBonusWeapon",
                weapon.name, "Weapon", "HistoricBonusWeapon",
                weapon.historicBonusWeaponName);
        }
    }

    auto legacyBeamTemplates =
        std::make_shared<fx::LegacyBeamTemplateCatalog>();
    legacyBeamTemplates->reserve(archetypes.size() / 8u + 1u);
    for (const auto& [name, archetype] : archetypes) {
        if (!archetype) continue;
        if (std::optional<fx::LegacyBeamTemplate> descriptor =
                fx::compileLegacyBeamTemplate(archetype->templateData)) {
            legacyBeamTemplates->emplace(name, std::move(*descriptor));
        }
    }
    for (const game::WeaponTemplate& weapon : weapons) {
        if (weapon.laserName.empty()) continue;
        const auto descriptor = legacyBeamTemplates->find(weapon.laserName);
        if (descriptor == legacyBeamTemplates->end() ||
            descriptor->second.kind != fx::LegacyBeamTemplateKind::Laser) {
            warnDegradedContent(
                "Weapon references a missing or non-laser LaserName",
                weapon.name, "Weapon", "LaserName", weapon.laserName);
        }
    }

    container::HashMap<container::String, game::ArmorTemplate> armors;
    armors.reserve(referencedArmors.size());
    for (const container::String& name : referencedArmors) {
        const game::ArmorTemplate* sourceArmor = armorStore.find(name);
        if (!sourceArmor) {
            warnDegradedContent(
                "Object combat profile references a missing Armor",
                {}, "ArmorStore", "Armor", name);
            continue;
        }
        armors.emplace(name, *sourceArmor);
    }

    // A script can add an otherwise unused CommandButton to an existing
    // object CommandSet.  Therefore the session freezes every button, while
    // it freezes only CommandSets referenced by an Object template.  This
    // keeps normal matches compact but still makes script-driven additions
    // independent of a menu-side GameData reload.
    // CommandSets are small and may be selected indirectly by RiderChange,
    // script slot overrides, or future typed containment rules. Freeze the
    // complete temporary store so Map.ini cannot create a valid indirect set
    // which is then lost to an incomplete reference walk.
    container::HashMap<container::String, game::CommandSetTemplate> commandSets;
    for (const container::String& name : referencedCommandSets) {
        if (!commandSetStore.find(name)) {
            warnDegradedContent(
                "Object recipe references a missing CommandSet",
                {}, "CommandSetStore", "CommandSet", name);
        }
    }
    commandSets.reserve(commandSetStore.all().size());
    for (const auto& [name, commandSet] : commandSetStore.all()) {
        commandSets.emplace(name, commandSet);
    }
    const auto& sourceButtons = commandButtonStore.all();
    container::HashMap<container::String, game::CommandButtonTemplate> commandButtons;
    commandButtons.reserve(sourceButtons.size());
    for (const auto& [name, button] : sourceButtons) {
        commandButtons.emplace(name, button);
    }

    // GameDataLoader creates an immutable empty catalog for minimal fixtures,
    // so preserving a null handle is only a defensive fallback.  Either way,
    // an active session never reaches back into a mutable loader/store.
    container::SharedPtr<const RankInfoCatalog> sourceRankInfoCatalog =
        source.rankInfoCatalogSnapshot();
    container::SharedPtr<const ScienceCatalog> sourceScienceCatalog = source.scienceCatalogSnapshot();
    container::SharedPtr<const SpecialPowerCatalog> specialPowerCatalog =
        specialPowerCatalogOverride
            ? std::move(specialPowerCatalogOverride)
            : source.specialPowerCatalogSnapshot();
    container::SharedPtr<const UpgradeCatalog> sourceUpgradeCatalog = source.upgradeCatalogSnapshot();
    container::SharedPtr<const game::CrateTemplateCatalog> crateTemplates =
        crateTemplateCatalogOverride
            ? std::move(crateTemplateCatalogOverride)
            : source.crateTemplateCatalogSnapshot();
    container::SharedPtr<const game::DamageFxCatalog> damageFxCatalog =
        damageFxCatalogOverride
            ? std::move(damageFxCatalogOverride)
            : source.damageFxCatalogSnapshot();
    container::SharedPtr<const fx::FxListCatalog> fxListCatalog =
        fxListCatalogOverride ? std::move(fxListCatalogOverride)
                              : source.fxListCatalogSnapshot();
    container::SharedPtr<const fx::ParticleSystemCatalog>
        particleSystemCatalog = particleSystemCatalogOverride
            ? std::move(particleSystemCatalogOverride)
            : source.particleSystemCatalogSnapshot();
    container::SharedPtr<const game::W3dPristineBoneCatalog> pristineBones =
        pristineBoneCatalogOverride
            ? std::move(pristineBoneCatalogOverride)
            : source.pristineBoneCatalogSnapshot();
    if (!objectCreationLists || !objectCreationLists->isLoaded()) {
        auto emptyCatalog = std::make_shared<game::ObjectCreationListCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty ObjectCreationList catalog: " +
                catalogError,
                {}, "ObjectCreationListCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "ObjectCreationList catalog is unavailable; published a sealed empty catalog",
                {}, "ObjectCreationListCatalog", {}, {}, "empty catalog");
        }
        objectCreationLists = std::move(emptyCatalog);
    }
    if (!crateTemplates || !crateTemplates->isLoaded()) {
        auto emptyCatalog = std::make_shared<game::CrateTemplateCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty CrateData catalog: " + catalogError,
                {}, "CrateTemplateCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "CrateData catalog is unavailable; published a sealed empty catalog",
                {}, "CrateTemplateCatalog", {}, {}, "empty catalog");
        }
        crateTemplates = std::move(emptyCatalog);
    }
    if (!damageFxCatalog || !damageFxCatalog->isLoaded()) {
        // Isolated content fixtures intentionally have no loader-owned
        // DamageFX store. Seal a genuinely empty catalog here: consulting
        // the process-global VFS would make capture depend on unrelated
        // mounts and would bypass the loader's frozen content fingerprint.
        auto emptyCatalog = std::make_shared<game::DamageFxCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            if (error) *error = "could not create empty DamageFX catalog: " +
                                catalogError;
            clear();
            return false;
        }
        damageFxCatalog = std::move(emptyCatalog);
    }
    if (rankInfoCatalogOverride && !rankInfoCatalogOverride->isLoaded()) {
        if (error) *error = "explicit RankInfoCatalog override is not sealed";
        clear();
        return false;
    }
    if (!allowSessionModifierCatalogs && rankInfoCatalogOverride &&
        sourceRankInfoCatalog && sourceRankInfoCatalog->isLoaded() &&
        rankInfoCatalogOverride->simulationFingerprint() !=
            sourceRankInfoCatalog->simulationFingerprint()) {
        if (error) *error =
            "explicit RankInfoCatalog override disagrees with loaded game content";
        clear();
        return false;
    }
    if (scienceCatalogOverride && !scienceCatalogOverride->isLoaded()) {
        if (error) *error = "explicit ScienceCatalog override is not sealed";
        clear();
        return false;
    }
    if (!allowSessionModifierCatalogs && scienceCatalogOverride &&
        sourceScienceCatalog && sourceScienceCatalog->isLoaded() &&
        scienceCatalogOverride->simulationFingerprint() !=
            sourceScienceCatalog->simulationFingerprint()) {
        if (error) *error = "explicit ScienceCatalog override disagrees with loaded game content";
        clear();
        return false;
    }
    if (upgradeCatalogOverride && !upgradeCatalogOverride->isLoaded()) {
        if (error) *error = "explicit UpgradeCatalog override is not sealed";
        clear();
        return false;
    }
    // Normal game/replay startup has a loader-owned catalog.  An explicit
    // dependency is allowed so isolated fixtures can avoid mutable globals,
    // but it may never substitute different technology rules under the same
    // aggregate content fingerprint in a real loaded session.
    if (!allowSessionModifierCatalogs && upgradeCatalogOverride &&
        sourceUpgradeCatalog && sourceUpgradeCatalog->isLoaded() &&
        upgradeCatalogOverride->simulationFingerprint() !=
            sourceUpgradeCatalog->simulationFingerprint()) {
        if (error) *error = "explicit UpgradeCatalog override disagrees with loaded game content";
        clear();
        return false;
    }
    container::SharedPtr<const RankInfoCatalog> rankInfoCatalog =
        rankInfoCatalogOverride ? std::move(rankInfoCatalogOverride)
                                : std::move(sourceRankInfoCatalog);
    container::SharedPtr<const UpgradeCatalog> upgradeCatalog = upgradeCatalogOverride
        ? std::move(upgradeCatalogOverride) : std::move(sourceUpgradeCatalog);
    container::SharedPtr<const ScienceCatalog> scienceCatalog = scienceCatalogOverride
        ? std::move(scienceCatalogOverride) : std::move(sourceScienceCatalog);

    if (!rankInfoCatalog || !rankInfoCatalog->isLoaded()) {
        auto emptyCatalog = std::make_shared<RankInfoCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty RankInfo catalog: " + catalogError,
                {}, "RankInfoCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "RankInfo catalog is unavailable; rank progression is disabled",
                {}, "RankInfoCatalog", {}, {}, "empty catalog");
        }
        rankInfoCatalog = std::move(emptyCatalog);
    }
    if (!scienceCatalog || !scienceCatalog->isLoaded()) {
        auto emptyCatalog = std::make_shared<ScienceCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty Science catalog: " + catalogError,
                {}, "ScienceCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "Science catalog is unavailable; published a sealed empty catalog",
                {}, "ScienceCatalog", {}, {}, "empty catalog");
        }
        scienceCatalog = std::move(emptyCatalog);
    }
    if (!upgradeCatalog || !upgradeCatalog->isLoaded()) {
        auto emptyCatalog = std::make_shared<UpgradeCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty Upgrade catalog: " + catalogError,
                {}, "UpgradeCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "Upgrade catalog is unavailable; published a sealed empty catalog",
                {}, "UpgradeCatalog", {}, {}, "empty catalog");
        }
        upgradeCatalog = std::move(emptyCatalog);
    }

    if (!specialPowerCatalog || !specialPowerCatalog->isLoaded()) {
        auto emptyCatalog = std::make_shared<SpecialPowerCatalog>();
        container::String catalogError;
        if (!emptyCatalog->loadFromVfsFiles({}, &catalogError)) {
            warnDegradedContent(
                "could not seal empty SpecialPower catalog: " + catalogError,
                {}, "SpecialPowerCatalog", {}, {}, "empty catalog");
        } else {
            warnDegradedContent(
                "SpecialPower catalog is unavailable; published a sealed empty catalog",
                {}, "SpecialPowerCatalog", {}, {}, "empty catalog");
        }
        specialPowerCatalog = std::move(emptyCatalog);
    }

    // Thing recipes are parsed before Upgrade.ini is sealed. Recompile the
    // upgrade-dependent plan at the snapshot boundary so every live object
    // receives catalog-stable trigger/conflict/removal masks. The authoring
    // names remain only in the immutable plan's provenance fields.
    for (auto& [objectName, archetype] : archetypes) {
        static_cast<void>(objectName);
        if (!archetype) continue;
        auto resolved = std::make_shared<game::ObjectArchetype>(*archetype);
        resolved->objectUpgradePlan = game::compileObjectUpgradePlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->autoHealPlan = game::compileObjectAutoHealPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->spyVisionPlan = game::compileObjectSpyVisionPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->minefieldPlan = game::compileObjectMinefieldPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->containmentPlan = game::compileObjectContainmentPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->tacticalPlan = game::compileObjectTacticalPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->createPlan = game::compileObjectCreatePlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->economyPlan = game::compileObjectEconomyPlan(
            resolved->templateData, upgradeCatalog.get());
        resolved->fireWeaponWhenDamagedPlan =
            game::compileObjectFireWeaponWhenDamagedPlan(
                resolved->templateData, upgradeCatalog.get());
        resolved->fireOclAfterCooldownPlan =
            game::compileObjectFireOclAfterCooldownPlan(
                resolved->templateData, upgradeCatalog.get());
        resolved->countermeasuresPlan =
            game::compileObjectCountermeasuresPlan(
                resolved->templateData, upgradeCatalog.get());
        resolved->deathReactionPlan =
            game::compileObjectDeathReactionPlan(
                resolved->templateData, upgradeCatalog.get());
        archetype = std::move(resolved);
    }

    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectCreateReferences(
            archetypes, scienceCatalog.get(), upgradeCatalog.get(),
            diagnostic);
    }, "ObjectCreate references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectDeathReferences(
            archetypes, upgradeCatalog.get(), scienceCatalog.get(),
            crateTemplates.get(), objectCreationLists.get(),
            specialPowerCatalog.get(), diagnostic);
    }, "ObjectDeath references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectContainmentReferences(
            archetypes, upgradeCatalog.get(), diagnostic);
    }, "ObjectContainment references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectAutoDepositReferences(
            archetypes, upgradeCatalog.get(), diagnostic);
    }, "ObjectAutoDeposit references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectCountermeasureReferences(archetypes, diagnostic);
    }, "ObjectCountermeasure references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectMinefieldReferences(
            archetypes, objectCreationLists.get(), diagnostic);
    }, "ObjectMinefield references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectCrateCollideReferences(
            archetypes, scienceCatalog.get(), upgradeCatalog.get(),
            diagnostic);
    }, "ObjectCrateCollide references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateTransitionDamageFxReferences(
            archetypes, objectCreationLists.get(), diagnostic);
    }, "TransitionDamageFX references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateBoneFxReferences(
            archetypes, objectCreationLists.get(), diagnostic);
    }, "BoneFX references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectUpgradeReferences(
            archetypes, objectCreationLists.get(), diagnostic);
    }, "ObjectUpgrade references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateObjectSpecialPowerReferences(
            archetypes, specialPowerCatalog.get(), scienceCatalog.get(),
            objectCreationLists.get(), diagnostic);
    }, "ObjectSpecialPower references");
    validateOrWarn([&](container::String* diagnostic) {
        return validateParticleUplinkReferences(
            archetypes, specialPowerCatalog.get(),
            *legacyBeamTemplates, diagnostic);
    }, "ParticleUplink references");

    m_objectArchetypes = std::move(archetypes);
    m_locomotors = std::move(locomotors);
    m_weapons = std::move(weapons);
    m_legacyBeamTemplates = std::move(legacyBeamTemplates);
    for (game::WeaponTemplate& weapon : m_weapons) {
        for (size_t level = 0; level < game::WeaponTemplate::kVeterancyLevelCount;
             ++level) {
            // An authored weapon OCL that does not resolve is degraded
            // content, not a capture failure: every sibling reference class in
            // this function warns and publishes a no-op, and the sealed empty
            // catalog installed above for isolated recipe/behavior probes
            // resolves nothing at all by design. Warn per reference and leave
            // the id invalid so the fire/detonation hook becomes a no-op.
            const auto resolveOcl = [&](const container::String& name,
                                        game::ObjectCreationListContentId& output,
                                        container::StringView field) {
                output = objectCreationLists->findId(name);
                if (!name.empty() && !output) {
                    warnDegradedContent(
                        "Weapon references a missing ObjectCreationList",
                        weapon.name, "Weapon", container::String{field}, name);
                }
            };
            resolveOcl(weapon.fireOcls[level], weapon.fireOclIds[level],
                       "FireOCL");
            resolveOcl(weapon.projectileDetonationOcls[level],
                       weapon.projectileDetonationOclIds[level],
                       "ProjectileDetonationOCL");
        }
    }
    m_weaponIds = std::move(weaponIds);
    m_globalWeaponBonuses = globalWeaponBonusesOverride
        ? *globalWeaponBonusesOverride : source.globalWeaponBonuses();
    m_armors = std::move(armors);
    m_commandSets = std::move(commandSets);
    m_commandButtons = std::move(commandButtons);
    m_rankInfoCatalog = std::move(rankInfoCatalog);
    m_scienceCatalog = std::move(scienceCatalog);
    m_specialPowerCatalog = std::move(specialPowerCatalog);
    m_veterancyUpgradeIds = {};
    const auto freezeVeterancyUpgrade = [&](game::ObjectVeterancyLevel level,
                                            container::StringView name) {
        const UpgradeDefinition* definition = upgradeCatalog
            ? upgradeCatalog->find(name) : nullptr;
        if (definition) {
            m_veterancyUpgradeIds[static_cast<size_t>(level)] = definition->id;
        } else {
            warnDegradedContent(
                "missing synthetic veterancy Upgrade; grant becomes no-op",
                {}, "GameContentSnapshot", "VeterancyUpgrade",
                container::String{name});
        }
    };
    freezeVeterancyUpgrade(game::ObjectVeterancyLevel::Veteran,
                           well_known_upgrade::VeterancyVeteran);
    freezeVeterancyUpgrade(game::ObjectVeterancyLevel::Elite,
                           well_known_upgrade::VeterancyElite);
    freezeVeterancyUpgrade(game::ObjectVeterancyLevel::Heroic,
                           well_known_upgrade::VeterancyHeroic);
    m_upgradeCatalog = std::move(upgradeCatalog);
    m_objectCreationLists = std::move(objectCreationLists);
    m_crateTemplates = std::move(crateTemplates);
    m_damageFxCatalog = std::move(damageFxCatalog);
    m_fxListCatalog = std::move(fxListCatalog);
    m_particleSystemCatalog = std::move(particleSystemCatalog);
    m_pristineBoneCatalog = std::move(pristineBones);
    m_captured = true;
    return true;
}

} // namespace engine
