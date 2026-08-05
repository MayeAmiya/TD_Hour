#pragma once

#include "core/container/container_types.h"

#include <cstdint>
namespace engine::script {
struct ScriptSkyboxTextureSet;
struct ScriptTerrainRoadPresentationSettings;
struct ScriptWaterPresentationSettings;
struct ScriptWeatherSnowSettings;
}
namespace engine::fx {
class FxListCatalog;
class ParticleSystemCatalog;
}

namespace engine {
class MultiplayerRuleset;
class RankInfoCatalog;
class ScienceCatalog;
class SpecialPowerCatalog;
class UpgradeCatalog;
struct VeterancySimulationRules;
struct TrackMarksPresentationSettings;
struct RenderGameDataSettings;
struct RenderQualitySettingsSnapshot;
}

namespace game {
class ObjectCreationListCatalog;
class CrateTemplateCatalog;
class DamageFxCatalog;
struct ArmorTemplate;
struct CommandButtonTemplate;
struct CommandSetTemplate;
struct LocomotorTemplate;
struct ObjectArchetype;
struct ThingTemplate;
struct ThingAuthoringTemplate;
struct WeaponAuthoringTemplate;
struct WeaponBonusSet;
class W3dPristineBoneCatalog;
}

namespace engine {

class GameDataRegistry {
public:
    bool isLoaded() const;

    const game::ArmorTemplate* findArmor(container::StringView name) const;
    const game::WeaponAuthoringTemplate* findWeapon(container::StringView name) const;
    [[nodiscard]] const game::WeaponBonusSet& globalWeaponBonuses() const noexcept;
    const game::LocomotorTemplate* findLocomotor(container::StringView name) const;
    const game::ThingAuthoringTemplate* findThing(container::StringView name) const;
    [[nodiscard]] container::SharedPtr<const game::ObjectArchetype>
    findObjectArchetype(container::StringView name) const;
    const game::CommandButtonTemplate* findCommandButton(container::StringView name) const;
    const game::CommandSetTemplate* findCommandSet(container::StringView name) const;
    [[nodiscard]] uint64_t simulationContentFingerprint() const noexcept;
    [[nodiscard]] const VeterancySimulationRules& veterancySimulationRules() const noexcept;
    container::SharedPtr<const MultiplayerRuleset> rulesetSnapshot() const noexcept;
    container::SharedPtr<const RankInfoCatalog>
    rankInfoCatalogSnapshot() const noexcept;
    container::SharedPtr<const ScienceCatalog> scienceCatalogSnapshot() const noexcept;
    container::SharedPtr<const SpecialPowerCatalog>
    specialPowerCatalogSnapshot() const noexcept;
    container::SharedPtr<const UpgradeCatalog> upgradeCatalogSnapshot() const noexcept;
    container::SharedPtr<const game::ObjectCreationListCatalog>
    objectCreationListCatalogSnapshot() const noexcept;
    container::SharedPtr<const game::CrateTemplateCatalog>
    crateTemplateCatalogSnapshot() const noexcept;
    container::SharedPtr<const game::DamageFxCatalog>
    damageFxCatalogSnapshot() const noexcept;
    container::SharedPtr<const fx::FxListCatalog>
    fxListCatalogSnapshot() const noexcept;
    container::SharedPtr<const fx::ParticleSystemCatalog>
    particleSystemCatalogSnapshot() const noexcept;
    container::SharedPtr<const game::W3dPristineBoneCatalog>
    pristineBoneCatalogSnapshot() const noexcept;
    [[nodiscard]] const script::ScriptWeatherSnowSettings&
    weatherPresentationSettings() const noexcept;
    [[nodiscard]] const script::ScriptSkyboxTextureSet&
    skyboxPresentationTextures() const noexcept;
    [[nodiscard]] const script::ScriptWaterPresentationSettings&
    waterPresentationSettings() const noexcept;
    [[nodiscard]] const script::ScriptTerrainRoadPresentationSettings&
    terrainRoadPresentationSettings() const noexcept;
    [[nodiscard]] const TrackMarksPresentationSettings&
    trackMarksPresentationSettings() const noexcept;
    [[nodiscard]] const RenderGameDataSettings&
    renderGameDataSettings() const noexcept;
    [[nodiscard]] container::SharedPtr<const RenderQualitySettingsSnapshot>
    renderQualitySettingsSnapshot() const noexcept;
};

} // namespace engine
