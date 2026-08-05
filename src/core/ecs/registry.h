#pragma once

#include <entt/entt.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ecs {

// ── 核心类型别名 ────────────────────────────────────────────────────────
using entity = entt::entity;
using registry = entt::registry;

inline constexpr entity null = entt::null;

// ── Entity 创建 ─────────────────────────────────────────────────────────
inline entity create(registry& reg) noexcept
{
    return reg.create();
}

inline void destroy(registry& reg, entity e) noexcept
{
    reg.destroy(e);
}

// ── Component 管理 ──────────────────────────────────────────────────────
template <typename T, typename... Args>
inline T& emplace(registry& reg, entity e, Args&&... args)
{
    return reg.emplace<T>(e, std::forward<Args>(args)...);
}

template <typename T>
inline T& get(registry& reg, entity e)
{
    return reg.get<T>(e);
}

template <typename T>
inline const T& get(const registry& reg, entity e)
{
    return reg.get<T>(e);
}

template <typename T>
inline T* try_get(registry& reg, entity e) noexcept
{
    return reg.try_get<T>(e);
}

template <typename T>
inline const T* try_get(const registry& reg, entity e) noexcept
{
    return reg.try_get<T>(e);
}

template <typename T>
inline bool has(const registry& reg, entity e) noexcept
{
    return reg.all_of<T>(e);
}

template <typename T>
inline void remove(registry& reg, entity e)
{
    reg.remove<T>(e);
}

template <typename T, typename Func>
inline decltype(auto) patch(registry& reg, entity e, Func&& func)
{
    return reg.patch<T>(e, std::forward<Func>(func));
}

// ── View ────────────────────────────────────────────────────────────────
template <typename... Components>
inline auto view(registry& reg)
{
    return reg.view<Components...>();
}

template <typename... Components>
inline auto view(const registry& reg)
{
    return reg.view<Components...>();
}

// ── Group ────────────────────────────────────────────────────────────────
template <typename... Owned>
inline auto group(registry& reg)
{
    return reg.group<Owned...>();
}

} // namespace ecs
