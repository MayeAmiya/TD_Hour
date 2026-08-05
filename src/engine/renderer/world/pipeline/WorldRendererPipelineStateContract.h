#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <d3d12.h>

namespace engine::render::world_renderer_detail {

inline constexpr container::Array<
    D3D12_COMPARISON_FUNC,
    static_cast<size_t>(StaticMeshDepthCompare::Count)>
    kDepthCompareFunctions{{
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL,
        D3D12_COMPARISON_FUNC_ALWAYS,
    }};

} // namespace engine::render::world_renderer_detail
