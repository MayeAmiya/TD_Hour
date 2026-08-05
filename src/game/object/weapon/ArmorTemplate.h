#pragma once

#include "core/container/hash_containers.h"

#include "game/base/DamageTypes.h"
#include "game/data/base/LegacyIniLoadType.h"
#include "math/fixed/q32_32.h"
namespace game {

struct ArmorTemplate {
    container::String name;
    container::Array<math::q32_32, DAMAGE_TYPE_COUNT> armor{};
    bool loaded = false;

    ArmorTemplate() { armor.fill(math::q32_32{int32_t{1}}); }

    math::q32_32 getArmor(DamageType dt) const {
        return armor[static_cast<int>(dt)];
    }
};

class ArmorStore {
public:
    static ArmorStore& instance();

    void clear();
    bool loadFromIni(
        const container::String& filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    const ArmorTemplate* find(const container::String& name) const;

private:
    container::HashMap<container::String, ArmorTemplate> m_armors;
};

} // namespace game
