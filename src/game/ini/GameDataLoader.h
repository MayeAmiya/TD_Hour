#pragma once

#include "core/container/container_types.h"

#include "game/data/base/BaseRegenerationRules.h"
#include "game/data/base/ContentDiagnostics.h"
#include "game/data/base/AISimulationRules.h"
#include "game/data/base/BuildPlacementSimulationRules.h"
#include "game/data/base/EnergySimulationRules.h"
#include "game/data/base/EconomySimulationRules.h"
#include "game/data/base/DifficultySimulationRules.h"
#include "game/data/base/DamageFxCatalog.h"
#include "game/data/base/PhysicsSimulationRules.h"
#include "game/data/base/VeterancySimulationRules.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/creation/CrateTemplateCatalog.h"
#include "game/data/presentation/TrackMarksRenderDescriptor.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "presentation/render/RenderQualitySettingsManager.h"
#include "game/data/presentation/ScriptWeatherPresentationSettings.h"
#include "game/data/presentation/ScriptSkyboxPresentationSettings.h"
#include "game/data/presentation/ScriptTerrainRoadPresentationSettings.h"
#include "game/data/presentation/ScriptWaterPresentationSettings.h"

#include <cstdint>
namespace engine {
class MultiplayerRuleset;
class RankInfoCatalog;
class ScienceCatalog;
class SpecialPowerCatalog;
class UpgradeCatalog;

namespace fx {
class FxListCatalog;
class ParticleSystemCatalog;
}
}

namespace game {

class W3dPristineBoneCatalog;

class GameDataLoader {
public:
    static GameDataLoader& instance();

    bool loadAll();
    void clear();

    bool isLoaded() const { return m_loaded; }
    [[nodiscard]] bool contentDegraded() const noexcept {
        return processContentDiagnostics().degraded();
    }
    [[nodiscard]] container::Vector<ContentDiagnostic>
    contentDiagnostics() const {
        return processContentDiagnostics().entries();
    }
    [[nodiscard]] uint64_t suppressedContentDiagnosticCount() const noexcept {
        return processContentDiagnostics().duplicateCount();
    }
    ContentDiagnosticCollector& mutableContentDiagnostics() noexcept {
        return processContentDiagnostics();
    }
    [[nodiscard]] uint64_t simulationContentFingerprint() const noexcept {
        return m_simulationContentFingerprint;
    }
    [[nodiscard]] const engine::PhysicsSimulationRules& physicsSimulationRules() const noexcept {
        return m_physicsSimulationRules;
    }
    [[nodiscard]] const engine::BaseRegenerationRules& baseRegenerationRules() const noexcept {
        return m_baseRegenerationRules;
    }
    [[nodiscard]] const engine::AISimulationRules& aiSimulationRules() const noexcept {
        return m_aiSimulationRules;
    }
    [[nodiscard]] uint32_t maxTunnelCapacity() const noexcept {
        return m_maxTunnelCapacity;
    }
    [[nodiscard]] math::q32_32 unitDamagedThreshold() const noexcept {
        return m_unitDamagedThreshold;
    }
    [[nodiscard]] math::q32_32 unitReallyDamagedThreshold() const noexcept {
        return m_unitReallyDamagedThreshold;
    }
    [[nodiscard]] math::q32_32 standardMinefieldDistance() const noexcept {
        return m_standardMinefieldDistance;
    }
    [[nodiscard]] math::q32_32 standardMinefieldDensity() const noexcept {
        return m_standardMinefieldDensity;
    }
    [[nodiscard]] math::q32_32 groupMoveClickToGatherFactor() const noexcept {
        return m_groupMoveClickToGatherFactor;
    }
    // GameData.SpecialPowerViewObject. Empty means no superweapon view object
    // is spawned, matching RefCode's AsciiString::isEmpty() early return.
    [[nodiscard]] const container::String&
    specialPowerViewObject() const noexcept {
        return m_specialPowerViewObject;
    }
    [[nodiscard]] const engine::BuildPlacementSimulationRules&
    buildPlacementSimulationRules() const noexcept {
        return m_buildPlacementSimulationRules;
    }
    [[nodiscard]] const engine::EnergySimulationRules& energySimulationRules() const noexcept {
        return m_energySimulationRules;
    }
    [[nodiscard]] const engine::EconomySimulationRules& economySimulationRules() const noexcept {
        return m_economySimulationRules;
    }
    [[nodiscard]] const engine::VeterancySimulationRules& veterancySimulationRules() const noexcept {
        return m_veterancySimulationRules;
    }
    [[nodiscard]] const engine::DifficultySimulationRules&
    difficultySimulationRules() const noexcept {
        return m_difficultySimulationRules;
    }
    [[nodiscard]] const WeaponBonusSet& globalWeaponBonuses() const noexcept {
        return m_globalWeaponBonuses;
    }
    container::SharedPtr<const engine::MultiplayerRuleset> rulesetSnapshot() const noexcept {
        return m_ruleset;
    }
    // Rank.ini is authoritative progression content. The catalog is sealed
    // once per content load so active gameplay never observes mutable rank
    // thresholds or generals-point grants.
    container::SharedPtr<const engine::RankInfoCatalog>
    rankInfoCatalogSnapshot() const noexcept {
        return m_rankInfoCatalog;
    }
    // Science.ini controls authoritative generals-point purchase rules.  The
    // shared immutable handle is copied into GameContentSnapshot when a
    // session starts, so a later menu-side data reload cannot rewrite a live
    // script transaction.
    container::SharedPtr<const engine::ScienceCatalog> scienceCatalogSnapshot() const noexcept {
        return m_scienceCatalog;
    }
    container::SharedPtr<const engine::SpecialPowerCatalog>
    specialPowerCatalogSnapshot() const noexcept {
        return m_specialPowerCatalog;
    }
    // Upgrade.ini is simulation-authoritative production content. Just like
    // Science, the sealed handle is copied into each GameContentSnapshot so a
    // later menu/data reload cannot rewrite a running research queue.
    container::SharedPtr<const engine::UpgradeCatalog> upgradeCatalogSnapshot() const noexcept {
        return m_upgradeCatalog;
    }
    container::SharedPtr<const ObjectCreationListCatalog>
    objectCreationListCatalogSnapshot() const noexcept {
        return m_objectCreationListCatalog;
    }
    container::SharedPtr<const CrateTemplateCatalog>
    crateTemplateCatalogSnapshot() const noexcept {
        return m_crateTemplateCatalog;
    }
    container::SharedPtr<const DamageFxCatalog>
    damageFxCatalogSnapshot() const noexcept {
        return m_damageFxCatalog;
    }
    container::SharedPtr<const W3dPristineBoneCatalog>
    pristineBoneCatalogSnapshot() const noexcept {
        return m_pristineBoneCatalog;
    }
    // ParticleSystem/FXList content is presentation-only. These immutable
    // handles deliberately remain outside the lockstep content fingerprint;
    // a running client can retain them across later VFS/menu reloads.
    container::SharedPtr<const engine::fx::ParticleSystemCatalog>
    particleSystemCatalogSnapshot() const noexcept {
        return m_particleSystemCatalog;
    }
    container::SharedPtr<const engine::fx::FxListCatalog> fxListCatalogSnapshot() const noexcept {
        return m_fxListCatalog;
    }
    // Weather.ini is client presentation content.  A GameSession copies this
    // immutable-at-load value into its own stamped weather presentation state;
    // it is intentionally excluded from the lockstep simulation fingerprint.
    [[nodiscard]] const engine::script::ScriptWeatherSnowSettings&
    weatherPresentationSettings() const noexcept {
        return m_weatherPresentationSettings;
    }
    // Water.ini's WaterTransparency skybox faces are client presentation
    // content. GameSession snapshots this five-face set before rendering, so
    // an active map never observes a later VFS/data reload.
    [[nodiscard]] const engine::script::ScriptSkyboxTextureSet&
    skyboxPresentationTextures() const noexcept {
        return m_skyboxPresentationTextures;
    }
    [[nodiscard]] const engine::script::ScriptWaterPresentationSettings&
    waterPresentationSettings() const noexcept {
        return m_waterPresentationSettings;
    }
    [[nodiscard]] const engine::script::ScriptTerrainRoadPresentationSettings&
    terrainRoadPresentationSettings() const noexcept {
        return m_terrainRoadPresentationSettings;
    }
    [[nodiscard]] const engine::TrackMarksPresentationSettings&
    trackMarksPresentationSettings() const noexcept {
        return m_trackMarksPresentationSettings;
    }
    [[nodiscard]] const container::Vector<container::String>&
    trackMarksPresentationDiagnostics() const noexcept {
        return m_trackMarksPresentationDiagnostics;
    }
    [[nodiscard]] const engine::RenderGameDataSettings&
    renderGameDataSettings() const noexcept {
        return m_renderGameDataSettings;
    }
    [[nodiscard]] container::SharedPtr<
        const engine::RenderQualitySettingsSnapshot>
    renderQualitySettingsSnapshot() const noexcept {
        return m_renderQualitySettingsManager.snapshot();
    }
    void setRenderQualityExternalOverrides(
        const engine::RenderFeatureQualityOverrides& feature,
        const engine::RenderDisplayOverrides& display);
    void setRenderQualityRuntimeOverrides(
        const engine::RenderFeatureQualityOverrides& feature,
        const engine::RenderDisplayOverrides& display);
    void clearRenderQualityRuntimeOverrides();
    // Window-system observation only. In-game UI is not a quality owner; a
    // user resize updates the Runtime Display extent without replacing other
    // launcher/host fields in that sparse layer.
    void updateRenderWindowExtent(uint32_t width, uint32_t height);
    void setRenderDisplayCapabilities(
        const engine::RenderDisplayCapabilities& capabilities);
    [[nodiscard]] const container::Vector<container::String>&
    renderGameDataDiagnostics() const noexcept {
        return m_renderGameDataDiagnostics;
    }

private:
    bool m_loaded = false;
    uint64_t m_simulationContentFingerprint = 0;
    engine::BaseRegenerationRules m_baseRegenerationRules;
    engine::AISimulationRules m_aiSimulationRules;
    math::q32_32 m_unitDamagedThreshold;
    math::q32_32 m_unitReallyDamagedThreshold;
    uint32_t m_maxTunnelCapacity = 10;
    math::q32_32 m_standardMinefieldDistance{40.0f};
    math::q32_32 m_standardMinefieldDensity{0.01f};
    math::q32_32 m_groupMoveClickToGatherFactor{1.0f};
    container::String m_specialPowerViewObject;
    engine::BuildPlacementSimulationRules m_buildPlacementSimulationRules;
    engine::EnergySimulationRules m_energySimulationRules;
    engine::EconomySimulationRules m_economySimulationRules;
    engine::PhysicsSimulationRules m_physicsSimulationRules;
    engine::VeterancySimulationRules m_veterancySimulationRules;
    engine::DifficultySimulationRules m_difficultySimulationRules;
    WeaponBonusSet m_globalWeaponBonuses;
    engine::script::ScriptWeatherSnowSettings m_weatherPresentationSettings;
    engine::script::ScriptSkyboxTextureSet m_skyboxPresentationTextures;
    engine::script::ScriptWaterPresentationSettings m_waterPresentationSettings;
    engine::script::ScriptTerrainRoadPresentationSettings m_terrainRoadPresentationSettings;
    engine::TrackMarksPresentationSettings m_trackMarksPresentationSettings;
    container::Vector<container::String> m_trackMarksPresentationDiagnostics;
    engine::RenderGameDataSettings m_renderGameDataSettings;
    engine::RenderQualitySettingsManager m_renderQualitySettingsManager;
    container::Vector<container::String> m_renderGameDataDiagnostics;
    container::SharedPtr<const engine::MultiplayerRuleset> m_ruleset;
    container::SharedPtr<const engine::RankInfoCatalog> m_rankInfoCatalog;
    container::SharedPtr<const engine::ScienceCatalog> m_scienceCatalog;
    container::SharedPtr<const engine::SpecialPowerCatalog> m_specialPowerCatalog;
    container::SharedPtr<const engine::UpgradeCatalog> m_upgradeCatalog;
    container::SharedPtr<const ObjectCreationListCatalog> m_objectCreationListCatalog;
    container::SharedPtr<const CrateTemplateCatalog> m_crateTemplateCatalog;
    container::SharedPtr<const DamageFxCatalog> m_damageFxCatalog;
    container::SharedPtr<const W3dPristineBoneCatalog> m_pristineBoneCatalog;
    container::SharedPtr<const engine::fx::ParticleSystemCatalog> m_particleSystemCatalog;
    container::SharedPtr<const engine::fx::FxListCatalog> m_fxListCatalog;
};

} // namespace game
