#pragma once

#include "ObjectArchetype.h"
#include "ThingObjectRecipe.h"
#include "game/data/base/LegacyIniLoadType.h"

namespace game {

// Preloaded logical sources let the content fingerprint and Thing compiler
// consume the exact same bytes. Keeping sources separate preserves the authored
// source-order semantics used by explicit INI load operations. Physical VFS
// duplicates of one logical path are resolved before they reach the factory.
struct ThingIniSource final {
    container::String path;
    container::Vector<container::String> layers;
};

class ThingFactory {
public:
    static ThingFactory& instance();

    void clear();
    bool loadFromIni(
        const container::String& filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    bool loadFromIniSources(
        container::Span<const ThingIniSource> sources,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    void finalizeDerivedMetadata();
    [[nodiscard]] bool derivedMetadataFinalized() const noexcept {
        return m_derivedMetadataFinalized;
    }
    const ThingAuthoringTemplate* find(const container::String& name) const;
    [[nodiscard]] container::SharedPtr<const ObjectArchetype> findArchetype(const container::String& name) const;
    const container::HashMap<container::String, ThingAuthoringTemplate>& all() const { return m_things; }

private:
    container::HashMap<container::String, ThingAuthoringTemplate> m_things;
    container::HashMap<container::String, container::SharedPtr<const ObjectArchetype>> m_archetypes;
    bool m_derivedMetadataFinalized = false;
};

} // namespace game
