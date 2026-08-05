#include "core/container/container_types.h"
#include "game/content/loader/GameDataRegistry.h"
#include "game/command/CommandButtonStore.h"
#include "game/command/CommandSetStore.h"
#include "game/ini/GameDataLoader.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/weapon/ArmorTemplate.h"
#include "game/object/weapon/WeaponTemplate.h"
namespace engine {

bool GameDataRegistry::isLoaded() const {
    return game::GameDataLoader::instance().isLoaded();
}

const game::ArmorTemplate* GameDataRegistry::findArmor(container::StringView name) const {
    return game::ArmorStore::instance().find(container::String{name});
}

const game::WeaponAuthoringTemplate* GameDataRegistry::findWeapon(container::StringView name) const {
    return game::WeaponStore::instance().find(container::String{name});
}

const game::WeaponBonusSet& GameDataRegistry::globalWeaponBonuses() const noexcept {
    return game::GameDataLoader::instance().globalWeaponBonuses();
}

const game::LocomotorTemplate* GameDataRegistry::findLocomotor(container::StringView name) const {
    return game::LocomotorStore::instance().find(container::String{name});
}

const game::ThingAuthoringTemplate* GameDataRegistry::findThing(container::StringView name) const {
    return game::ThingFactory::instance().find(container::String{name});
}

container::SharedPtr<const game::ObjectArchetype>
GameDataRegistry::findObjectArchetype(container::StringView name) const {
    return game::ThingFactory::instance().findArchetype(container::String{name});
}

const game::CommandButtonTemplate* GameDataRegistry::findCommandButton(container::StringView name) const {
    return game::CommandButtonStore::instance().find(name);
}

const game::CommandSetTemplate* GameDataRegistry::findCommandSet(container::StringView name) const {
    return game::CommandSetStore::instance().find(name);
}

uint64_t GameDataRegistry::simulationContentFingerprint() const noexcept {
    return game::GameDataLoader::instance().simulationContentFingerprint();
}

const VeterancySimulationRules& GameDataRegistry::veterancySimulationRules() const noexcept {
    return game::GameDataLoader::instance().veterancySimulationRules();
}

container::SharedPtr<const MultiplayerRuleset> GameDataRegistry::rulesetSnapshot() const noexcept {
    return game::GameDataLoader::instance().rulesetSnapshot();
}

container::SharedPtr<const RankInfoCatalog>
GameDataRegistry::rankInfoCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().rankInfoCatalogSnapshot();
}

container::SharedPtr<const ScienceCatalog> GameDataRegistry::scienceCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().scienceCatalogSnapshot();
}

container::SharedPtr<const SpecialPowerCatalog>
GameDataRegistry::specialPowerCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().specialPowerCatalogSnapshot();
}

container::SharedPtr<const UpgradeCatalog> GameDataRegistry::upgradeCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().upgradeCatalogSnapshot();
}

container::SharedPtr<const game::ObjectCreationListCatalog>
GameDataRegistry::objectCreationListCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().objectCreationListCatalogSnapshot();
}

container::SharedPtr<const game::CrateTemplateCatalog>
GameDataRegistry::crateTemplateCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().crateTemplateCatalogSnapshot();
}

container::SharedPtr<const game::DamageFxCatalog>
GameDataRegistry::damageFxCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().damageFxCatalogSnapshot();
}

container::SharedPtr<const fx::FxListCatalog>
GameDataRegistry::fxListCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().fxListCatalogSnapshot();
}

container::SharedPtr<const fx::ParticleSystemCatalog>
GameDataRegistry::particleSystemCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().particleSystemCatalogSnapshot();
}

container::SharedPtr<const game::W3dPristineBoneCatalog>
GameDataRegistry::pristineBoneCatalogSnapshot() const noexcept {
    return game::GameDataLoader::instance().pristineBoneCatalogSnapshot();
}

const script::ScriptWeatherSnowSettings&
GameDataRegistry::weatherPresentationSettings() const noexcept {
    return game::GameDataLoader::instance().weatherPresentationSettings();
}

const script::ScriptSkyboxTextureSet&
GameDataRegistry::skyboxPresentationTextures() const noexcept {
    return game::GameDataLoader::instance().skyboxPresentationTextures();
}

const script::ScriptWaterPresentationSettings&
GameDataRegistry::waterPresentationSettings() const noexcept {
    return game::GameDataLoader::instance().waterPresentationSettings();
}

const script::ScriptTerrainRoadPresentationSettings&
GameDataRegistry::terrainRoadPresentationSettings() const noexcept {
    return game::GameDataLoader::instance().terrainRoadPresentationSettings();
}

const TrackMarksPresentationSettings&
GameDataRegistry::trackMarksPresentationSettings() const noexcept {
    return game::GameDataLoader::instance().trackMarksPresentationSettings();
}

const RenderGameDataSettings&
GameDataRegistry::renderGameDataSettings() const noexcept {
    return game::GameDataLoader::instance().renderGameDataSettings();
}

container::SharedPtr<const RenderQualitySettingsSnapshot>
GameDataRegistry::renderQualitySettingsSnapshot() const noexcept {
    return game::GameDataLoader::instance().renderQualitySettingsSnapshot();
}

} // namespace engine
