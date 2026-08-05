#pragma once

#include "core/debug/debug_config.h"

#include <d3dcompiler.h>

namespace engine::d3d12 {

[[nodiscard]] inline constexpr UINT runtimeShaderCompileFlags() noexcept {
#if TD_DEBUG_ENABLED
    return D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_DEBUG |
        D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    return D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
}

} // namespace engine::d3d12
