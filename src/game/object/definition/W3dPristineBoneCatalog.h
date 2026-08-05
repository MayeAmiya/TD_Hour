#pragma once

#include "core/container/hash_containers.h"
#include "data/w3d/W3dFixedPose.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace game {

class ThingFactory;

struct W3dPristinePoseId final {
    uint32_t value = 0;
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
    friend constexpr bool operator==(
        const W3dPristinePoseId&, const W3dPristinePoseId&) = default;
};

struct W3dPristineBoneEntry final {
    container::String foldedName;
    data::w3d::FixedRigidTransform local;
};

struct W3dPristinePose final {
    container::String canonicalModelPath;
    container::String prototype;
    container::String animation;
    container::Vector<W3dPristineBoneEntry> bones;
    bool finalFrame = false;
};

struct ObjectPristinePoseBinding final {
    // Flattened Draw channels in declaration order: all normal condition
    // rules first, followed by all transition rules. Normal rule indices
    // therefore remain stable for existing gameplay consumers.
    container::Vector<W3dPristinePoseId> poseByVisualRule;
    math::q32_32 assetScale{int32_t{1}};
};

struct W3dWeaponBarrelEntry final {
    container::String fireFxBone;
    container::String recoilBone;
    container::String muzzleFlash;
    container::String launchBone;
    data::w3d::FixedRigidTransform launchLocal;
    bool hasLaunchBone = false;
};

struct W3dWeaponBarrelTable final {
    container::Vector<W3dWeaponBarrelEntry> barrels;
    bool numbered = false;
};

// Session-source catalog built once from logic-referenced W3D resources.
// Runtime lookup never touches VFS, W3dLoader, renderer caches or float poses.
class W3dPristineBoneCatalog final {
public:
    [[nodiscard]] bool build(const ThingFactory& things,
                             container::String* error = nullptr);
    void clear();

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] uint64_t sourceFingerprint() const noexcept {
        return m_sourceFingerprint;
    }
    [[nodiscard]] std::optional<data::w3d::FixedRigidTransform> find(
        container::StringView archetypeName, size_t visualRuleIndex,
        container::StringView boneName) const;
    // RefCode validates one combined Name01..99 table across FireFX, recoil,
    // muzzle and launch names. The first ordinal where all four are absent
    // ends the table; bare names are considered only when no numbered entry
    // exists at all.
    [[nodiscard]] W3dWeaponBarrelTable resolveWeaponBarrels(
        container::StringView archetypeName, size_t visualRuleIndex,
        container::StringView fireFxPrefix,
        container::StringView recoilPrefix,
        container::StringView muzzleFlashPrefix,
        container::StringView launchPrefix) const;
    [[nodiscard]] const container::Vector<container::String>& diagnostics()
        const noexcept {
        return m_diagnostics;
    }

private:
    container::Vector<W3dPristinePose> m_poses;
    container::HashMap<container::String, ObjectPristinePoseBinding> m_bindings;
    container::Vector<container::String> m_diagnostics;
    uint64_t m_sourceFingerprint = 0;
    bool m_loaded = false;
};

} // namespace game
