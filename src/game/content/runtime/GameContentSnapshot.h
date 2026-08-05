#pragma once

#include "core/container/hash_containers.h"

#include "game/command/CommandButtonStore.h"
#include "game/command/CommandSetStore.h"
#include "game/audio/EvaEventCatalog.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/weapon/ArmorTemplate.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/creation/CrateTemplateCatalog.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/DamageFxCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/base/ObjectVeterancy.h"
#include "presentation/fx/runtime/LegacyBeamTemplate.h"
#include "presentation/audio/AudioContentLayer.h"
#include "presentation/ui/MappedImageContentLayer.h"
#include "presentation/ui/MapStringContentLayer.h"

#include <cstddef>
#include <cstdint>
namespace game {
struct ObjectArchetype;
struct ThingTemplate;
class ThingFactory;
class W3dPristineBoneCatalog;
}

namespace engine {

namespace fx {
class FxListCatalog;
class ParticleSystemCatalog;
}

class GameDataRegistry;
class RankInfoCatalog;
class ScienceCatalog;
class SpecialPowerCatalog;
class UpgradeCatalog;

// Compile the modern immutable render value from one already-resolved stream
// Object recipe. Exposed for content audit tools; runtime sessions normally
// receive the same result through GameContentSnapshot::capture.
[[nodiscard]] bool compileProjectileStreamRenderDescriptor(
    const game::ThingTemplate& streamObject,
    game::ProjectileStreamRenderDescriptor& output,
    container::String* error = nullptr);

// Immutable, session-owned projection of the object content needed by the
// current ECS slice.  It is captured exactly once before a GameSession starts
// creating map or gameplay objects.  The global parsers/stores remain useful
// for loading a later match, but an active session never looks through them.
//
// Locomotors are intentionally copied by value: unlike the legacy
// LocomotorTemplateOverride pointer chain, runtime ECS components receive
// scalar values and must not retain a mutable data-store address.  Object
// archetypes are also deep-copied to make their recipe/model-condition data
// independent of ThingFactory reload and clear operations.
class GameContentSnapshot final {
public:
    // Captures every compiled object recipe currently visible through
    // ThingFactory plus each locomotor referenced by those recipes.  A
    // missing locomotor is preserved as missing because Stage-0 historically
    // leaves that object without a supported locomotion component; a missing
    // compiled recipe for an enumerated ThingFactory entry is an invalid
    // content boundary and fails capture.
    [[nodiscard]] bool capture(const GameDataRegistry& source, container::String* error = nullptr);
    // Tests/replay bootstrap may already hold the exact immutable Upgrade
    // catalog selected for a match. Accepting it here keeps the content handoff
    // explicit and avoids a session reaching back into a process-global loader.
    [[nodiscard]] bool capture(const GameDataRegistry& source,
                               container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
                               container::String* error = nullptr);
    // Isolated fixtures/replay bootstrap may similarly provide the exact
    // sealed Science catalog. Production startup normally obtains it from
    // GameDataRegistry; a conflicting override is rejected below the same
    // content-fingerprint boundary as UpgradeCatalog.
    [[nodiscard]] bool capture(const GameDataRegistry& source,
                               container::SharedPtr<const ScienceCatalog> scienceCatalogOverride,
                               container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
                               container::String* error = nullptr);
    // Applies the ordered ZH Map.ini/solo.ini modifier sequence to private
    // store copies and freezes the result into this session. Process-global
    // stores are never mutated, so a later match starts from ordinary VFS
    // content again.
    [[nodiscard]] bool captureWithGameplayModifiers(
        const GameDataRegistry& source,
        container::Span<const container::String> modifierPaths,
        container::SharedPtr<const ScienceCatalog> scienceCatalogOverride,
        container::SharedPtr<const UpgradeCatalog> upgradeCatalogOverride,
        container::String* error = nullptr);
    void clear();

    [[nodiscard]] bool isCaptured() const noexcept { return m_captured; }
    [[nodiscard]] std::size_t objectArchetypeCount() const noexcept {
        return m_objectArchetypes.size();
    }
    [[nodiscard]] std::size_t locomotorCount() const noexcept {
        return m_locomotors.size();
    }
    [[nodiscard]] std::size_t weaponCount() const noexcept {
        return m_weapons.size();
    }
    [[nodiscard]] std::size_t armorCount() const noexcept {
        return m_armors.size();
    }
    [[nodiscard]] std::size_t commandSetCount() const noexcept {
        return m_commandSets.size();
    }
    [[nodiscard]] std::size_t commandButtonCount() const noexcept {
        return m_commandButtons.size();
    }

    [[nodiscard]] container::SharedPtr<const game::ObjectArchetype>
    findObjectArchetype(container::StringView name) const;
    [[nodiscard]] const game::FrozenLocomotorTemplate*
    findLocomotor(container::StringView name) const;
    [[nodiscard]] const game::WeaponTemplate*
    findWeapon(container::StringView name) const;
    [[nodiscard]] game::WeaponContentId
    findWeaponId(container::StringView name) const noexcept;
    [[nodiscard]] const game::WeaponTemplate*
    findWeapon(game::WeaponContentId id) const noexcept;
    [[nodiscard]] container::SharedPtr<const fx::LegacyBeamTemplateCatalog>
    legacyBeamTemplates() const noexcept { return m_legacyBeamTemplates; }
    [[nodiscard]] const fx::LegacyBeamTemplate* findLegacyBeamTemplate(
        container::StringView name) const noexcept;
    [[nodiscard]] game::ObjectCreationListContentId
    findObjectCreationListId(container::StringView name) const noexcept;
    [[nodiscard]] const game::ObjectCreationListDefinition*
    findObjectCreationList(game::ObjectCreationListContentId id) const noexcept;
    [[nodiscard]] const game::W3dPristineBoneCatalog*
    pristineBoneCatalog() const noexcept {
        return m_pristineBoneCatalog.get();
    }
    [[nodiscard]] size_t objectCreationListCount() const noexcept {
        return m_objectCreationLists ? m_objectCreationLists->size() : 0;
    }
    [[nodiscard]] const game::CrateTemplateDefinition* findCrateTemplate(
        container::StringView name) const noexcept {
        return m_crateTemplates ? m_crateTemplates->find(name) : nullptr;
    }
    [[nodiscard]] size_t crateTemplateCount() const noexcept {
        return m_crateTemplates ? m_crateTemplates->size() : 0;
    }
    [[nodiscard]] const game::DamageFxCatalog* damageFxCatalog() const noexcept {
        return m_damageFxCatalog.get();
    }
    [[nodiscard]] const game::DamageFxDefinition* findDamageFx(
        container::StringView name) const noexcept {
        return m_damageFxCatalog ? m_damageFxCatalog->find(name) : nullptr;
    }
    [[nodiscard]] const game::DamageFxDefinition* findDamageFx(
        game::DamageFxContentId id) const noexcept {
        return m_damageFxCatalog ? m_damageFxCatalog->find(id) : nullptr;
    }
    [[nodiscard]] game::DamageFxContentId findDamageFxId(
        container::StringView name) const noexcept {
        return m_damageFxCatalog ? m_damageFxCatalog->findId(name)
                                 : game::DamageFxContentId{};
    }
    [[nodiscard]] size_t damageFxCount() const noexcept {
        return m_damageFxCatalog ? m_damageFxCatalog->size() : 0;
    }
    [[nodiscard]] container::SharedPtr<const fx::FxListCatalog>
    fxListCatalogSnapshot() const noexcept { return m_fxListCatalog; }
    [[nodiscard]] container::SharedPtr<const fx::ParticleSystemCatalog>
    particleSystemCatalogSnapshot() const noexcept {
        return m_particleSystemCatalog;
    }
    // Audio is presentation-only but must share the exact map.ini/solo.ini
    // generation with FXList sound nuggets. No audio device or sequencer
    // state enters this immutable content snapshot.
    [[nodiscard]] bool freezeAudioContentLayers(
        container::Span<const audio::AudioContentLayer> layers,
        container::String* error = nullptr);
    [[nodiscard]] container::Span<const audio::AudioContentLayer>
    audioContentLayers() const noexcept {
        return m_audioContentLayers
            ? container::Span<const audio::AudioContentLayer>{
                  *m_audioContentLayers}
            : container::Span<const audio::AudioContentLayer>{};
    }
    [[nodiscard]] container::SharedPtr<
        const container::Vector<audio::AudioContentLayer>>
    audioContentLayerSnapshot() const noexcept {
        return m_audioContentLayers;
    }
    // EVA policy is parsed from the same winning VFS generation as the
    // session's audio catalog. Runtime death feedback uses enum IDs and this
    // immutable value; it never reopens Eva.ini or hashes an event name.
    [[nodiscard]] bool freezeEvaEventCatalog(
        container::StringView content, container::StringView sourcePath,
        container::String* error = nullptr);
    [[nodiscard]] const game::EvaEventCatalog* evaEventCatalog() const noexcept {
        return m_evaEventCatalog.get();
    }
    [[nodiscard]] bool freezeMappedImageContentLayers(
        container::Span<const ui::MappedImageContentLayer> layers,
        container::String* error = nullptr);
    [[nodiscard]] container::Span<const ui::MappedImageContentLayer>
    mappedImageContentLayers() const noexcept {
        return m_mappedImageContentLayers
            ? container::Span<const ui::MappedImageContentLayer>{
                  *m_mappedImageContentLayers}
            : container::Span<const ui::MappedImageContentLayer>{};
    }
    [[nodiscard]] const container::SharedPtr<
        const container::Vector<ui::MappedImageContentLayer>>&
    mappedImageContentLayerSnapshot() const noexcept {
        return m_mappedImageContentLayers;
    }
    [[nodiscard]] bool freezeMapStringContentLayer(
        const ui::MapStringContentLayer& layer,
        container::String* error = nullptr);
    [[nodiscard]] const container::SharedPtr<
        const ui::MapStringContentLayer>&
    mapStringContentLayerSnapshot() const noexcept {
        return m_mapStringContentLayer;
    }
    [[nodiscard]] game::WeaponBonus resolveWeaponBonus(
        const game::WeaponTemplate& weapon,
        game::WeaponBonusConditionMask conditions) const noexcept;
    [[nodiscard]] const game::ArmorTemplate*
    findArmor(container::StringView name) const;
    // Command bars are presentation/input content, but old map scripts can
    // mutate their slots.  Retain value copies so a map script never reads
    // CommandSetStore/CommandButtonStore after the session has started.
    [[nodiscard]] const game::CommandSetTemplate*
    findCommandSet(container::StringView name) const;
    [[nodiscard]] const game::CommandButtonTemplate*
    findCommandButton(container::StringView name) const;
    [[nodiscard]] const game::CommandButtonTemplate*
    findCommandButtonByStableId(uint64_t stableId) const noexcept;
    // Science rules are small but simulation-authoritative: map scripts may
    // spend generals points long after menu/global data was reloaded.  Keep
    // the exact immutable catalog handle selected at session startup.
    [[nodiscard]] const ScienceCatalog* scienceCatalog() const noexcept {
        return m_scienceCatalog.get();
    }
    [[nodiscard]] const RankInfoCatalog* rankInfoCatalog() const noexcept {
        return m_rankInfoCatalog.get();
    }
    [[nodiscard]] const SpecialPowerCatalog* specialPowerCatalog() const noexcept {
        return m_specialPowerCatalog.get();
    }
    [[nodiscard]] const SpecialPowerDefinition* findSpecialPower(
        container::StringView name) const noexcept;
    [[nodiscard]] const SpecialPowerDefinition* findSpecialPower(
        SpecialPowerContentId id) const noexcept;
    // Upgrade definitions are production simulation content. A queue stores
    // stable UpgradeContentId values and resolves them only through this
    // frozen handle, never through a mutable Upgrade.ini loader.
    [[nodiscard]] const UpgradeCatalog* upgradeCatalog() const noexcept {
        return m_upgradeCatalog.get();
    }
    [[nodiscard]] UpgradeContentId veterancyUpgradeId(
        game::ObjectVeterancyLevel level) const noexcept {
        return m_veterancyUpgradeIds[static_cast<size_t>(level)];
    }

private:
    [[nodiscard]] bool captureFromStores(
        const GameDataRegistry& source, game::ThingFactory& things,
        const game::LocomotorStore& locomotors,
        const game::WeaponStore& weapons, const game::ArmorStore& armors,
        const game::CommandSetStore& commandSets,
        const game::CommandButtonStore& commandButtons,
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
        container::SharedPtr<const fx::FxListCatalog>
            fxListCatalogOverride,
        container::SharedPtr<const fx::ParticleSystemCatalog>
            particleSystemCatalogOverride,
        container::SharedPtr<const game::W3dPristineBoneCatalog>
            pristineBoneCatalogOverride,
        container::String* error);
    container::HashMap<container::String, container::SharedPtr<const game::ObjectArchetype>>
        m_objectArchetypes;
    container::HashMap<container::String, game::FrozenLocomotorTemplate>
        m_locomotors;
    // Weapon runtime components retain only WeaponContentId.  The vector is
    // index-addressable and the name map exists only at spawn/content-query
    // time, so an active match never hashes a WeaponStore name per shot.
    container::Vector<game::WeaponTemplate> m_weapons;
    container::HashMap<container::String, game::WeaponContentId> m_weaponIds;
    container::SharedPtr<const fx::LegacyBeamTemplateCatalog>
        m_legacyBeamTemplates;
    game::WeaponBonusSet m_globalWeaponBonuses;
    container::HashMap<container::String, game::ArmorTemplate> m_armors;
    container::HashMap<container::String, game::CommandSetTemplate> m_commandSets;
    container::HashMap<container::String, game::CommandButtonTemplate> m_commandButtons;
    container::SharedPtr<const RankInfoCatalog> m_rankInfoCatalog;
    container::SharedPtr<const ScienceCatalog> m_scienceCatalog;
    container::SharedPtr<const SpecialPowerCatalog> m_specialPowerCatalog;
    container::SharedPtr<const UpgradeCatalog> m_upgradeCatalog;
    container::Array<UpgradeContentId, 4> m_veterancyUpgradeIds{};
    container::SharedPtr<const game::ObjectCreationListCatalog> m_objectCreationLists;
    container::SharedPtr<const game::CrateTemplateCatalog> m_crateTemplates;
    container::SharedPtr<const game::DamageFxCatalog> m_damageFxCatalog;
    container::SharedPtr<const fx::FxListCatalog> m_fxListCatalog;
    container::SharedPtr<const fx::ParticleSystemCatalog>
        m_particleSystemCatalog;
    container::SharedPtr<const container::Vector<audio::AudioContentLayer>>
        m_audioContentLayers;
    container::SharedPtr<const game::EvaEventCatalog> m_evaEventCatalog;
    container::SharedPtr<
        const container::Vector<ui::MappedImageContentLayer>>
        m_mappedImageContentLayers;
    container::SharedPtr<const ui::MapStringContentLayer>
        m_mapStringContentLayer;
    container::SharedPtr<const game::W3dPristineBoneCatalog>
        m_pristineBoneCatalog;
    bool m_captured = false;
};

} // namespace engine
