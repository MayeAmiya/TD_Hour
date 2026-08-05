#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine::ai
{

// A stock AI recipe has exactly one production owner.  Generic ownership is
// deliberately narrower than ObjectModuleCatalogEntry::isAiModule: lifecycle
// may register every hasAiUpdate recipe as a capability-off actor, but only the
// base AIUpdateInterface recipe may opt into generic 44-state order ownership.
enum class AIRecipeOwnerClass : uint8_t
{
    Generic,
    Specialized,
    DisabledStockFeature,
    Unsupported,
};

// Stable recipe identity used by actor bindings and snapshots.  Values are
// append-only: save/replay compatibility must not depend on table position.
enum class AIRecipeId : uint8_t
{
    Invalid = 0,
    DeployStyleAIUpdate = 1,
    AssaultTransportAIUpdate = 2,
    MissileAIUpdate = 3,
    ChinookAIUpdate = 4,
    JetAIUpdate = 5,
    AIUpdateInterface = 6,
    SupplyTruckAIUpdate = 7,
    DeliverPayloadAIUpdate = 8,
    HackInternetAIUpdate = 9,
    DozerAIUpdate = 10,
    POWTruckAIUpdate = 11,
    RailedTransportAIUpdate = 12,
    TransportAIUpdate = 13,
    WanderAIUpdate = 14,
    WorkerAIUpdate = 15,
    FlightDeckBehavior = 16,
};

enum class AIRecipeOwner : uint8_t
{
    ObjectAIRuntime,
    ObjectAirfieldSystem,
    ObjectBridgeSystem,
    ObjectBuilderSystem,
    ObjectBuilderAndEconomySystems,
    ObjectContainmentSystem,
    ObjectEconomySystem,
    ObjectProjectileSystem,
    ObjectTacticalSystem,
    CompileTimeDisabledFeature,
    Unimplemented,
};

enum class AIRecipeGenericAdmission : uint8_t
{
    Generic44StateRecipe,
    RejectedSpecializedOwner,
    RejectedDisabledStockFeature,
    RejectedUnsupportedUnimplemented,
};

// Recipe classification never opts an actor into MoveStop, Attack, Special,
// or future order capabilities.  Those ownership transfers remain explicit
// runtime operations after actor creation.
enum class AIRecipeCapabilityAdmission : uint8_t
{
    DefaultOffExplicitOptIn,
};

// ZH separates the concrete AIUpdate recipe from the owner of an individual
// command.  Most specialized AIUpdate classes intercept their own behavior
// and then delegate ordinary commands to AIUpdateInterface.  Railed transport
// and FlightDeck are deliberate exceptions with specialized command policy.
enum class AIRecipeCommandOwner : uint8_t
{
    ObjectAIRuntime,
    InheritedAIUpdate,
    SpecializedOwner,
    Unsupported,
};

enum class AIRecipeResolutionStatus : uint8_t
{
    Resolved,
    Missing,
    Unknown,
    Ambiguous,
};

struct AIRecipeOwnerRoute final
{
    AIRecipeId id = AIRecipeId::Invalid;
    std::string_view moduleClass;
    AIRecipeOwnerClass ownerClass = AIRecipeOwnerClass::Unsupported;
    AIRecipeOwner owner = AIRecipeOwner::Unimplemented;
    AIRecipeGenericAdmission genericAdmission =
        AIRecipeGenericAdmission::RejectedUnsupportedUnimplemented;
    AIRecipeCapabilityAdmission capabilityAdmission =
        AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn;
    AIRecipeCommandOwner moveOwner = AIRecipeCommandOwner::Unsupported;
    AIRecipeCommandOwner stopOwner = AIRecipeCommandOwner::Unsupported;
    AIRecipeCommandOwner waypointOwner = AIRecipeCommandOwner::Unsupported;
    AIRecipeCommandOwner attackOwner = AIRecipeCommandOwner::Unsupported;
};

struct AIRecipeResolution final
{
    AIRecipeResolutionStatus status = AIRecipeResolutionStatus::Missing;
    AIRecipeId recipe = AIRecipeId::Invalid;

    [[nodiscard]] constexpr bool resolved() const noexcept
    {
        return status == AIRecipeResolutionStatus::Resolved &&
               recipe != AIRecipeId::Invalid;
    }
};

inline constexpr size_t EXPECTED_STOCK_AI_RECIPE_OWNER_COUNT = 16;

inline constexpr std::array<AIRecipeOwnerRoute,
                            EXPECTED_STOCK_AI_RECIPE_OWNER_COUNT>
    AI_RECIPE_OWNER_ROUTES{{
        {AIRecipeId::DeployStyleAIUpdate, "DeployStyleAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectTacticalSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::AssaultTransportAIUpdate, "AssaultTransportAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectContainmentSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::MissileAIUpdate, "MissileAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectProjectileSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::ChinookAIUpdate, "ChinookAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectAirfieldSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::JetAIUpdate, "JetAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectAirfieldSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::AIUpdateInterface, "AIUpdateInterface", AIRecipeOwnerClass::Generic,
         AIRecipeOwner::ObjectAIRuntime,
         AIRecipeGenericAdmission::Generic44StateRecipe,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::ObjectAIRuntime,
         AIRecipeCommandOwner::ObjectAIRuntime,
         AIRecipeCommandOwner::ObjectAIRuntime,
         AIRecipeCommandOwner::ObjectAIRuntime},
        {AIRecipeId::SupplyTruckAIUpdate, "SupplyTruckAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectEconomySystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::DeliverPayloadAIUpdate, "DeliverPayloadAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectContainmentSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::HackInternetAIUpdate, "HackInternetAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectEconomySystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::DozerAIUpdate, "DozerAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectBuilderSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::POWTruckAIUpdate, "POWTruckAIUpdate", AIRecipeOwnerClass::DisabledStockFeature,
         AIRecipeOwner::CompileTimeDisabledFeature,
         AIRecipeGenericAdmission::RejectedDisabledStockFeature,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::Unsupported,
         AIRecipeCommandOwner::Unsupported,
         AIRecipeCommandOwner::Unsupported,
         AIRecipeCommandOwner::Unsupported},
        {AIRecipeId::RailedTransportAIUpdate, "RailedTransportAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectBridgeSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::TransportAIUpdate, "TransportAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectContainmentSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::WanderAIUpdate, "WanderAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectTacticalSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::WorkerAIUpdate, "WorkerAIUpdate", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectBuilderAndEconomySystems,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate,
         AIRecipeCommandOwner::InheritedAIUpdate},
        {AIRecipeId::FlightDeckBehavior, "FlightDeckBehavior", AIRecipeOwnerClass::Specialized,
         AIRecipeOwner::ObjectAirfieldSystem,
         AIRecipeGenericAdmission::RejectedSpecializedOwner,
         AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn,
         AIRecipeCommandOwner::Unsupported,
         AIRecipeCommandOwner::SpecializedOwner,
         AIRecipeCommandOwner::Unsupported,
         AIRecipeCommandOwner::SpecializedOwner},
    }};

[[nodiscard]] consteval bool validateAIRecipeOwnerRoutes()
{
    size_t genericCount = 0;
    for (size_t index = 0; index < AI_RECIPE_OWNER_ROUTES.size(); ++index)
    {
        const AIRecipeOwnerRoute& route = AI_RECIPE_OWNER_ROUTES[index];
        if (route.id == AIRecipeId::Invalid || route.moduleClass.empty() ||
            route.capabilityAdmission !=
                AIRecipeCapabilityAdmission::DefaultOffExplicitOptIn)
            return false;
        for (size_t other = index + 1; other < AI_RECIPE_OWNER_ROUTES.size();
             ++other)
        {
            if (route.id == AI_RECIPE_OWNER_ROUTES[other].id ||
                route.moduleClass == AI_RECIPE_OWNER_ROUTES[other].moduleClass)
                return false;
        }

        switch (route.ownerClass)
        {
        case AIRecipeOwnerClass::Generic:
            ++genericCount;
            if (route.moduleClass != "AIUpdateInterface" ||
                route.owner != AIRecipeOwner::ObjectAIRuntime ||
                route.genericAdmission !=
                    AIRecipeGenericAdmission::Generic44StateRecipe ||
                route.moveOwner != AIRecipeCommandOwner::ObjectAIRuntime ||
                route.stopOwner != AIRecipeCommandOwner::ObjectAIRuntime ||
                route.waypointOwner != AIRecipeCommandOwner::ObjectAIRuntime ||
                route.attackOwner != AIRecipeCommandOwner::ObjectAIRuntime)
                return false;
            break;
        case AIRecipeOwnerClass::Specialized:
            if (route.owner == AIRecipeOwner::ObjectAIRuntime ||
                route.owner == AIRecipeOwner::Unimplemented ||
                route.genericAdmission !=
                    AIRecipeGenericAdmission::RejectedSpecializedOwner ||
                route.moveOwner == AIRecipeCommandOwner::ObjectAIRuntime ||
                route.stopOwner == AIRecipeCommandOwner::ObjectAIRuntime ||
                route.waypointOwner == AIRecipeCommandOwner::ObjectAIRuntime ||
                route.attackOwner == AIRecipeCommandOwner::ObjectAIRuntime)
                return false;
            break;
        case AIRecipeOwnerClass::DisabledStockFeature:
            if (route.moduleClass != "POWTruckAIUpdate" ||
                route.owner != AIRecipeOwner::CompileTimeDisabledFeature ||
                route.genericAdmission !=
                    AIRecipeGenericAdmission::RejectedDisabledStockFeature ||
                route.moveOwner != AIRecipeCommandOwner::Unsupported ||
                route.stopOwner != AIRecipeCommandOwner::Unsupported ||
                route.waypointOwner != AIRecipeCommandOwner::Unsupported ||
                route.attackOwner != AIRecipeCommandOwner::Unsupported)
                return false;
            break;
        case AIRecipeOwnerClass::Unsupported:
            return false;
            break;
        }
    }
    return genericCount == 1;
}

[[nodiscard]] constexpr const AIRecipeOwnerRoute* aiRecipeOwnerRouteFor(
    AIRecipeId recipe) noexcept
{
    for (const AIRecipeOwnerRoute& route : AI_RECIPE_OWNER_ROUTES)
        if (route.id == recipe)
            return &route;
    return nullptr;
}

static_assert(AI_RECIPE_OWNER_ROUTES.size() ==
              EXPECTED_STOCK_AI_RECIPE_OWNER_COUNT);
static_assert(validateAIRecipeOwnerRoutes());

[[nodiscard]] constexpr const AIRecipeOwnerRoute* aiRecipeOwnerRouteFor(
    std::string_view moduleClass) noexcept
{
    for (const AIRecipeOwnerRoute& route : AI_RECIPE_OWNER_ROUTES)
        if (route.moduleClass == moduleClass)
            return &route;
    return nullptr;
}

template <typename ModuleRange>
[[nodiscard]] AIRecipeResolution resolveAIRecipe(
    const ModuleRange& modules) noexcept
{
    const AIRecipeOwnerRoute* resolved = nullptr;
    for (const auto& module : modules)
    {
        if (!module.isAiModule)
            continue;
        const std::string_view moduleClass{
            module.moduleClass.data(), module.moduleClass.size()};
        const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(moduleClass);
        if (!route)
            return {AIRecipeResolutionStatus::Unknown, AIRecipeId::Invalid};
        if (resolved)
            return {AIRecipeResolutionStatus::Ambiguous, AIRecipeId::Invalid};
        resolved = route;
    }
    return resolved
        ? AIRecipeResolution{AIRecipeResolutionStatus::Resolved, resolved->id}
        : AIRecipeResolution{AIRecipeResolutionStatus::Missing,
                             AIRecipeId::Invalid};
}

[[nodiscard]] constexpr bool aiRecipeUsesGenericMoveStop(
    AIRecipeId recipe) noexcept
{
    const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(recipe);
    const auto inheritedOrDirect = [](AIRecipeCommandOwner owner) constexpr {
        return owner == AIRecipeCommandOwner::ObjectAIRuntime ||
               owner == AIRecipeCommandOwner::InheritedAIUpdate;
    };
    return route && route->ownerClass != AIRecipeOwnerClass::Unsupported &&
           route->ownerClass != AIRecipeOwnerClass::DisabledStockFeature &&
           inheritedOrDirect(route->moveOwner) &&
           inheritedOrDirect(route->stopOwner) &&
           inheritedOrDirect(route->waypointOwner);
}

[[nodiscard]] constexpr bool aiRecipeUsesGenericAttack(
    AIRecipeId recipe) noexcept
{
    const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(recipe);
    return route && route->ownerClass != AIRecipeOwnerClass::Unsupported &&
           route->ownerClass != AIRecipeOwnerClass::DisabledStockFeature &&
           (route->attackOwner == AIRecipeCommandOwner::ObjectAIRuntime ||
            route->attackOwner == AIRecipeCommandOwner::InheritedAIUpdate);
}

[[nodiscard]] constexpr bool admitsGeneric44StateRecipe(
    std::string_view moduleClass) noexcept
{
    const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(moduleClass);
    return route && route->genericAdmission ==
                        AIRecipeGenericAdmission::Generic44StateRecipe;
}

} // namespace engine::ai
