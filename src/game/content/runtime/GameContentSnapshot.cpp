#include "game/content/runtime/GameContentSnapshot.h"

#include "game/data/base/SpecialPowerCatalog.h"

#include <memory>

namespace engine {

void GameContentSnapshot::clear() {
    m_objectArchetypes.clear();
    m_locomotors.clear();
    m_weapons.clear();
    m_weaponIds.clear();
    m_legacyBeamTemplates.reset();
    m_globalWeaponBonuses = {};
    m_armors.clear();
    m_commandSets.clear();
    m_commandButtons.clear();
    m_rankInfoCatalog.reset();
    m_scienceCatalog.reset();
    m_specialPowerCatalog.reset();
    m_upgradeCatalog.reset();
    m_veterancyUpgradeIds = {};
    m_objectCreationLists.reset();
    m_crateTemplates.reset();
    m_damageFxCatalog.reset();
    m_fxListCatalog.reset();
    m_particleSystemCatalog.reset();
    m_audioContentLayers.reset();
    m_evaEventCatalog.reset();
    m_mappedImageContentLayers.reset();
    m_mapStringContentLayer.reset();
    m_pristineBoneCatalog.reset();
    m_captured = false;
}

bool GameContentSnapshot::freezeAudioContentLayers(
    container::Span<const audio::AudioContentLayer> layers,
    container::String* error) {
    if (error) error->clear();
    if (!m_captured) {
        if (error) *error = "game content must be captured before audio";
        return false;
    }
    container::Vector<audio::AudioContentLayer> candidate;
    candidate.reserve(layers.size());
    for (const audio::AudioContentLayer& layer : layers) {
        if (layer.sourcePath.empty()) {
            if (error) *error = "audio content layer has no source path";
            return false;
        }
        candidate.push_back(layer);
    }
    m_audioContentLayers =
        std::make_shared<const container::Vector<audio::AudioContentLayer>>(
            std::move(candidate));
    return true;
}

bool GameContentSnapshot::freezeEvaEventCatalog(
    container::StringView content, container::StringView sourcePath,
    container::String* error) {
    if (error) error->clear();
    if (!m_captured) {
        if (error) *error = "game content must be captured before EVA";
        return false;
    }
    auto candidate = std::make_shared<game::EvaEventCatalog>();
    if (!candidate->compile(content, sourcePath, error)) return false;
    m_evaEventCatalog = std::move(candidate);
    return true;
}

bool GameContentSnapshot::freezeMappedImageContentLayers(
    container::Span<const ui::MappedImageContentLayer> layers,
    container::String* error) {
    if (error) error->clear();
    if (!m_captured) {
        if (error) *error = "game content must be captured before mapped images";
        return false;
    }
    container::Vector<ui::MappedImageContentLayer> candidate;
    candidate.reserve(layers.size());
    for (const ui::MappedImageContentLayer& layer : layers) {
        if (layer.sourcePath.empty()) {
            if (error) *error = "mapped-image content layer has no source path";
            return false;
        }
        candidate.push_back(layer);
    }
    m_mappedImageContentLayers = std::make_shared<
        const container::Vector<ui::MappedImageContentLayer>>(
            std::move(candidate));
    return true;
}

bool GameContentSnapshot::freezeMapStringContentLayer(
    const ui::MapStringContentLayer& layer, container::String* error) {
    if (error) error->clear();
    if (!m_captured) {
        if (error) *error = "game content must be captured before map strings";
        return false;
    }
    if (layer.sourcePath.empty()) {
        if (error) *error = "map string content layer has no source path";
        return false;
    }
    m_mapStringContentLayer =
        std::make_shared<const ui::MapStringContentLayer>(layer);
    return true;
}

container::SharedPtr<const game::ObjectArchetype>
GameContentSnapshot::findObjectArchetype(container::StringView name) const {
    if (!m_captured || name.empty()) return {};
    const auto found = m_objectArchetypes.find(container::String{name});
    return found == m_objectArchetypes.end()
        ? container::SharedPtr<const game::ObjectArchetype>{}
        : found->second;
}

const game::FrozenLocomotorTemplate*
GameContentSnapshot::findLocomotor(container::StringView name) const {
    if (!m_captured || name.empty()) return nullptr;
    const auto found = m_locomotors.find(container::String{name});
    return found == m_locomotors.end() ? nullptr : &found->second;
}

const SpecialPowerDefinition* GameContentSnapshot::findSpecialPower(
    container::StringView name) const noexcept {
    return m_captured && m_specialPowerCatalog
        ? m_specialPowerCatalog->find(name) : nullptr;
}

const SpecialPowerDefinition* GameContentSnapshot::findSpecialPower(
    SpecialPowerContentId id) const noexcept {
    return m_captured && m_specialPowerCatalog
        ? m_specialPowerCatalog->find(id) : nullptr;
}

const game::WeaponTemplate*
GameContentSnapshot::findWeapon(container::StringView name) const {
    return findWeapon(findWeaponId(name));
}

game::WeaponContentId
GameContentSnapshot::findWeaponId(container::StringView name) const noexcept {
    if (!m_captured || name.empty()) return game::INVALID_WEAPON_CONTENT_ID;
    const auto found = m_weaponIds.find(container::String{name});
    return found == m_weaponIds.end() ? game::INVALID_WEAPON_CONTENT_ID : found->second;
}

const game::WeaponTemplate*
GameContentSnapshot::findWeapon(game::WeaponContentId id) const noexcept {
    if (!m_captured || !id || id.value > m_weapons.size()) return nullptr;
    return &m_weapons[id.value - 1];
}

const fx::LegacyBeamTemplate* GameContentSnapshot::findLegacyBeamTemplate(
    container::StringView name) const noexcept {
    if (!m_captured || !m_legacyBeamTemplates || name.empty()) return nullptr;
    const auto found = m_legacyBeamTemplates->find(container::String{name});
    return found == m_legacyBeamTemplates->end() ? nullptr : &found->second;
}

game::ObjectCreationListContentId GameContentSnapshot::findObjectCreationListId(
    container::StringView name) const noexcept {
    return m_captured && m_objectCreationLists
        ? m_objectCreationLists->findId(name)
        : game::ObjectCreationListContentId{};
}

const game::ObjectCreationListDefinition*
GameContentSnapshot::findObjectCreationList(
    game::ObjectCreationListContentId id) const noexcept {
    return m_captured && m_objectCreationLists
        ? m_objectCreationLists->find(id) : nullptr;
}

game::WeaponBonus GameContentSnapshot::resolveWeaponBonus(
    const game::WeaponTemplate& weapon,
    game::WeaponBonusConditionMask conditions) const noexcept {
    game::WeaponBonus result;
    m_globalWeaponBonuses.append(conditions, result);
    weapon.weaponBonuses.append(conditions, result);
    return result;
}

const game::ArmorTemplate*
GameContentSnapshot::findArmor(container::StringView name) const {
    if (!m_captured || name.empty()) return nullptr;
    const auto found = m_armors.find(container::String{name});
    return found == m_armors.end() ? nullptr : &found->second;
}

const game::CommandSetTemplate*
GameContentSnapshot::findCommandSet(container::StringView name) const {
    if (!m_captured || name.empty()) return nullptr;
    const auto found = m_commandSets.find(container::String{name});
    return found == m_commandSets.end() ? nullptr : &found->second;
}

const game::CommandButtonTemplate*
GameContentSnapshot::findCommandButton(container::StringView name) const {
    if (!m_captured || name.empty()) return nullptr;
    const auto found = m_commandButtons.find(container::String{name});
    return found == m_commandButtons.end() ? nullptr : &found->second;
}

const game::CommandButtonTemplate*
GameContentSnapshot::findCommandButtonByStableId(uint64_t stableId) const
    noexcept {
    if (!m_captured || stableId == 0) return nullptr;
    for (const auto& [name, button] : m_commandButtons) {
        static_cast<void>(name);
        if (button.descriptor.stableId == stableId) return &button;
    }
    return nullptr;
}

} // namespace engine
