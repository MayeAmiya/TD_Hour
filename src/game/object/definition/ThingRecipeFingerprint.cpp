#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "CombatProfile.h"
#include "ObjectModuleCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include "ThingRecipeDetail.h"

namespace game::detail {

void hashRecipeBytes(uint64_t& hash, container::StringView value) noexcept {
    // FNV-1a is sufficient here: this is a content identity/fingerprint, not
    // a security primitive. Hash the length so adjacent field boundaries do
    // not collapse into the same byte stream.
    constexpr uint64_t kPrime = 1099511628211ull;
    const auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= kPrime;
    };
    uint64_t length = value.size();
    for (size_t index = 0; index < sizeof(length); ++index) {
        mix(static_cast<uint8_t>(length >> (index * 8u)));
    }
    for (const char character : value) mix(static_cast<uint8_t>(character));
}

void hashRecipeValue(uint64_t& hash, uint64_t value) noexcept {
    constexpr uint64_t kPrime = 1099511628211ull;
    for (size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<uint8_t>(value >> (index * 8u));
        hash *= kPrime;
    }
}

void hashRecipeModule(uint64_t& hash, const ModuleData& module) noexcept {
    hashRecipeBytes(hash, module.type);
    hashRecipeBytes(hash, module.tag);
    hashRecipeBytes(hash, module.moduleClass);
    hashRecipeBytes(hash, module.moduleTag);
    hashRecipeValue(hash, static_cast<uint64_t>(module.category));
    hashRecipeValue(hash, module.interfaceMask);
    hashRecipeValue(hash, module.interfaceMaskKnown ? 1u : 0u);
    hashRecipeValue(hash, module.isAiModule ? 1u : 0u);
    hashRecipeValue(hash, static_cast<uint64_t>(module.recipeOperation));
    hashRecipeValue(hash, module.authoredOrder);
    hashRecipeValue(hash, module.copiedFromParent ? 1u : 0u);
    hashRecipeValue(hash, module.values.size());
    for (const auto& [key, value] : module.values) {
        hashRecipeBytes(hash, key);
        hashRecipeBytes(hash, value);
    }
    hashRecipeValue(hash, module.children.size());
    for (const ModuleData& child : module.children) hashRecipeModule(hash, child);
}

[[nodiscard]] uint64_t objectRecipeFingerprint(const ThingTemplate& templateData,
                                                const CombatProfile* combatProfile) noexcept {
    uint64_t hash = 14695981039346656037ull;
    hashRecipeBytes(hash, templateData.name);
    hashRecipeValue(hash, static_cast<uint64_t>(templateData.editorSorting));
    hashRecipeValue(hash, templateData.editorDisplayColor);
    hashRecipeValue(hash, templateData.forbidden ? 1u : 0u);
    hashRecipeBytes(hash, templateData.defaultW3dModel);
    hashRecipeValue(hash, static_cast<uint64_t>(templateData.assetScale.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.instanceScaleFuzziness.raw()));
    hashRecipeBytes(hash, templateData.kindOf);
    hashRecipeBytes(hash, templateData.defaultOwningSide);
    hashRecipeBytes(hash, templateData.armorName);
    hashRecipeBytes(hash, templateData.selectedPortraitImage);
    for (const container::String& cameo : templateData.upgradeCameos) {
        hashRecipeBytes(hash, cameo);
    }
    hashRecipeValue(hash, templateData.enterGuard ? 1u : 0u);
    hashRecipeValue(hash, templateData.hijackGuard ? 1u : 0u);
    hashRecipeBytes(hash, templateData.soundAmbient);
    hashRecipeBytes(hash, templateData.soundAmbientDamaged);
    hashRecipeBytes(hash, templateData.soundAmbientReallyDamaged);
    hashRecipeBytes(hash, templateData.soundAmbientRubble);
    hashRecipeBytes(hash, templateData.soundOnDamaged);
    hashRecipeBytes(hash, templateData.soundOnReallyDamaged);
    hashRecipeBytes(hash, templateData.voiceFear);
    hashRecipeBytes(hash, templateData.voiceEject);
    hashRecipeBytes(hash, templateData.soundEject);
    // Per-unit voice/sound response family. These participate in the recipe
    // fingerprint for the same reason the ambient family does: two objects
    // that differ only in their authored speech are different content, and an
    // override stream that changes just a voice must invalidate the archetype.
    hashRecipeBytes(hash, templateData.voiceSelect);
    hashRecipeBytes(hash, templateData.voiceSelectElite);
    hashRecipeBytes(hash, templateData.voiceGroupSelect);
    hashRecipeBytes(hash, templateData.voiceMove);
    hashRecipeBytes(hash, templateData.voiceAttack);
    hashRecipeBytes(hash, templateData.voiceAttackAir);
    hashRecipeBytes(hash, templateData.voiceGuard);
    hashRecipeBytes(hash, templateData.voiceTaskComplete);
    hashRecipeBytes(hash, templateData.voiceTaskUnable);
    hashRecipeBytes(hash, templateData.voiceMeetEnemy);
    hashRecipeBytes(hash, templateData.voiceCreated);
    hashRecipeBytes(hash, templateData.voiceDefect);
    hashRecipeBytes(hash, templateData.soundCreated);
    hashRecipeBytes(hash, templateData.soundDie);
    hashRecipeBytes(hash, templateData.soundDieFire);
    hashRecipeBytes(hash, templateData.soundDieToxin);
    hashRecipeBytes(hash, templateData.soundEnter);
    hashRecipeBytes(hash, templateData.soundExit);
    hashRecipeBytes(hash, templateData.soundStealthOn);
    hashRecipeBytes(hash, templateData.soundStealthOff);
    hashRecipeBytes(hash, templateData.soundFallingFromPlane);
    hashRecipeBytes(hash, templateData.soundMoveLoop);
    hashRecipeBytes(hash, templateData.soundMoveLoopDamaged);
    hashRecipeBytes(hash, templateData.soundPromotedVeteran);
    hashRecipeBytes(hash, templateData.soundPromotedElite);
    hashRecipeBytes(hash, templateData.soundPromotedHero);
    hashRecipeBytes(hash, templateData.soundMoveStart);
    hashRecipeBytes(hash, templateData.soundMoveStartDamaged);
    hashRecipeBytes(hash, templateData.voiceEnter);
    hashRecipeBytes(hash, templateData.voiceGarrison);
    hashRecipeValue(hash, templateData.unitSpecificSounds.size());
    for (const auto& [semanticName, eventName] :
         templateData.unitSpecificSounds) {
        hashRecipeBytes(hash, semanticName);
        hashRecipeBytes(hash, eventName);
    }
    hashRecipeValue(hash, templateData.unitSpecificFx.size());
    for (const auto& [semanticName, fxListName] :
         templateData.unitSpecificFx) {
        hashRecipeBytes(hash, semanticName);
        hashRecipeBytes(hash, fxListName);
    }
    hashRecipeValue(
        hash, static_cast<uint64_t>(templateData.buildCostFixed.raw()));
    hashRecipeValue(hash, templateData.refundValue);
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.structureRubbleHeightFixed.raw()));
    hashRecipeValue(hash, templateData.isBridge ? 1u : 0u);
    hashRecipeValue(hash, templateData.isPrerequisite ? 1u : 0u);
    hashRecipeValue(hash, templateData.isBuildFacility ? 1u : 0u);
    hashRecipeValue(hash, templateData.prerequisiteObjectAlternatives.size());
    for (const auto& alternatives : templateData.prerequisiteObjectAlternatives) {
        hashRecipeValue(hash, alternatives.size());
        for (const container::String& name : alternatives)
            hashRecipeBytes(hash, name);
    }
    hashRecipeValue(hash, templateData.prerequisiteSciences.size());
    for (const container::String& science : templateData.prerequisiteSciences)
        hashRecipeBytes(hash, science);
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.buildTimeSeconds.raw()));
    hashRecipeValue(hash, static_cast<uint8_t>(templateData.buildCompletion));
    hashRecipeValue(hash, static_cast<uint32_t>(templateData.energyProduction));
    hashRecipeValue(hash, static_cast<uint32_t>(templateData.energyBonus));
    hashRecipeValue(
        hash, static_cast<uint64_t>(templateData.sightRangeFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.shroudClearingRangeFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.shroudRevealToAllRangeFixed.raw()));
    hashRecipeValue(hash, static_cast<uint32_t>(templateData.crusherLevel));
    hashRecipeValue(hash, static_cast<uint32_t>(templateData.crushableLevel));
    hashRecipeValue(hash, templateData.isTrainable ? 1u : 0u);
    hashRecipeValue(hash, static_cast<uint8_t>(templateData.buildability));
    hashRecipeBytes(hash, templateData.legacyReskinRootName);
    hashRecipeValue(hash, templateData.buildVariations.size());
    for (const container::String& variation : templateData.buildVariations)
        hashRecipeBytes(hash, variation);
    hashRecipeValue(hash, static_cast<uint8_t>(templateData.radarPriority));
    for (const int32_t value : templateData.experienceValue)
        hashRecipeValue(hash, static_cast<uint32_t>(value));
    for (const int32_t value : templateData.experienceRequired)
        hashRecipeValue(hash, static_cast<uint32_t>(value));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.geometry.majorRadiusFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.geometry.minorRadiusFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.geometry.heightFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.placementViewAngleRadiansFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.factoryExitWidthFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.factoryExtraBibWidthFixed.raw()));
    hashRecipeValue(hash, templateData.shadow.typeMask);
    hashRecipeBytes(hash, templateData.shadow.texture);
    hashRecipeValue(hash, std::bit_cast<uint32_t>(templateData.shadow.sizeX));
    hashRecipeValue(hash, std::bit_cast<uint32_t>(templateData.shadow.sizeY));
    hashRecipeValue(hash, std::bit_cast<uint32_t>(templateData.shadow.offsetX));
    hashRecipeValue(hash, std::bit_cast<uint32_t>(templateData.shadow.offsetY));
    hashRecipeValue(
        hash, static_cast<uint64_t>(templateData.fenceWidthFixed.raw()));
    hashRecipeValue(
        hash, static_cast<uint64_t>(templateData.fenceXOffsetFixed.raw()));
    hashRecipeValue(hash, templateData.maxSimultaneousOfType);
    hashRecipeBytes(hash, templateData.maxSimultaneousLinkKey);
    hashRecipeValue(hash, static_cast<uint64_t>(templateData.body.kind));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.body.maximumHealthFixed.raw()));
    hashRecipeValue(hash, static_cast<uint64_t>(
        templateData.body.initialHealthFixed.raw()));
    hashRecipeValue(hash, templateData.modules.size());
    for (const ModuleData& module : templateData.modules) hashRecipeModule(hash, module);
    // ArmorSet/WeaponSet is no longer flattened into a top-level string.
    // Hash its immutable data projection so lockstep content validation sees
    // a condition/slot change even when all ordinary object fields match.
    hashRecipeValue(hash, combatProfile ? 1u : 0u);
    if (combatProfile) {
        const container::Span<const WeaponSetProfile> weaponSets = combatProfile->weaponSets();
        hashRecipeValue(hash, weaponSets.size());
        for (const WeaponSetProfile& set : weaponSets) {
            hashRecipeValue(hash, set.conditions);
            hashRecipeValue(hash, set.shareWeaponReloadTime ? 1u : 0u);
            hashRecipeValue(hash, set.weaponLockSharedAcrossSets ? 1u : 0u);
            for (const WeaponSlotProfile& slot : set.slots) {
                hashRecipeBytes(hash, slot.weaponTemplateName);
                hashRecipeValue(hash, slot.autoChooseSources);
                hashRecipeValue(hash, slot.preferredAgainstKinds.count());
                for (size_t kind = 0; kind < game::kObjectKindOfCount; ++kind) {
                    if (slot.preferredAgainstKinds.test(kind)) {
                        hashRecipeValue(hash, kind);
                    }
                }
            }
        }
        const container::Span<const ArmorSetProfile> armorSets = combatProfile->armorSets();
        hashRecipeValue(hash, armorSets.size());
        for (const ArmorSetProfile& set : armorSets) {
            hashRecipeValue(hash, set.conditions);
            hashRecipeBytes(hash, set.armorTemplateName);
            hashRecipeBytes(hash, set.damageFxName);
        }
    }
    return hash;
}


} // namespace game::detail
