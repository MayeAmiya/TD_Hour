#pragma once

// ============================================================================
// fixed_config.h — 编译期标量类型切换
// ============================================================================
// GAME_USE_FLOAT = 1 → float 模式（默认，开发调试快，VCL Vec4f SIMD）
// GAME_USE_FLOAT = 0 → q32_32 定点模式（原版精度，VCL Vec2q SIMD）
// ============================================================================

#ifndef GAME_USE_FLOAT
#define GAME_USE_FLOAT 1
#endif

#if GAME_USE_FLOAT
#include "../float/game_type_hub.h"
#else
#include "game_type_hub.h"
#endif
