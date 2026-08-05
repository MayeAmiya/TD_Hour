#pragma once

#include "core/container/hash_containers.h"
#include <cstdint>

namespace game {

enum class ModuleRecipeOperation : uint8_t {
    Direct,
    InheritableDefault,
    OverrideableDefault,
    Added,
    Replaced,
};

// Mirrors the original ModuleFactory ownership buckets. This is recipe
// metadata only: runtime ECS systems consume typed projections, never a
// generic virtual Module instance.
enum class ModuleRecipeCategory : uint8_t {
    Unknown,
    Behavior,
    Draw,
    ClientUpdate,
};

// Keep the original interface-bit layout so compatibility diagnostics and
// later module-family compilers have one stable vocabulary. A module whose
// factory metadata is not known yet has `interfaceMaskKnown == false`; it is
// retained rather than guessed away during default-recipe conflict handling.
enum ModuleRecipeInterface : uint32_t {
    ModuleRecipeInterfaceNone = 0,
    ModuleRecipeInterfaceUpdate = 0x00000001u,
    ModuleRecipeInterfaceDie = 0x00000002u,
    ModuleRecipeInterfaceDamage = 0x00000004u,
    ModuleRecipeInterfaceCreate = 0x00000008u,
    ModuleRecipeInterfaceCollide = 0x00000010u,
    ModuleRecipeInterfaceBody = 0x00000020u,
    ModuleRecipeInterfaceContain = 0x00000040u,
    ModuleRecipeInterfaceUpgrade = 0x00000080u,
    ModuleRecipeInterfaceSpecialPower = 0x00000100u,
    ModuleRecipeInterfaceDestroy = 0x00000200u,
    ModuleRecipeInterfaceDraw = 0x00000400u,
    ModuleRecipeInterfaceClientUpdate = 0x00000800u,
};

struct ModuleData {
    // `type` and `tag` preserve the raw parsed declaration for diagnostics
    // and loss-aware tooling. The normalized fields below are what recipe
    // compilation uses for collision checks and tag lookup.
    container::String type;
    container::String tag;
    container::String moduleClass;
    container::String moduleTag;
    container::SharedPtr<const container::String> sourcePath;
    uint32_t sourceLine = 0;
    container::Vector<uint32_t> valueSourceLines;
    ModuleRecipeCategory category = ModuleRecipeCategory::Unknown;
    uint32_t interfaceMask = ModuleRecipeInterfaceNone;
    bool interfaceMaskKnown = false;
    bool isAiModule = false;
    container::HashMap<container::String, container::String> properties;
    container::Vector<std::pair<container::String, container::String>> values;
    container::Vector<ModuleData> children;
    ModuleRecipeOperation recipeOperation = ModuleRecipeOperation::Direct;
    uint32_t authoredOrder = 0;
    // The original name `copiedFromDefault` is broader than it sounds:
    // ObjectReskin and map overlays also copy a parent template. Keep that
    // provenance explicit without reintroducing the old override pointer
    // chain.
    bool copiedFromParent = false;
};

} // namespace game
