#pragma once

// Global math utilities (constants, fast_trig, clamp/lerp/sign, etc.)
#include "wwmath_core.h"

// Fixed-point types + game types (float or fixed, selected by GAME_USE_FLOAT)
#include "core/math/fixed/q16_16.h"
#include "core/math/fixed/q32_32.h"
#include "core/math/fixed/fixed_point_traits.h"
#include "core/math/fixed/fast_math.h"
#include "core/math/fixed/fixed_config.h"    // ← dispatches to float/ or fixed/ game_type_hub

// MathTraits<T> — generic template for float/double/int/fixed-point.
// The fixed-point specializations need q16_16/q32_32 to be complete first.
#include "core/math/base/math_traits.h"

// Vector types
#include "vector/float2.h"
#include "vector/float3.h"
#include "vector/float4.h"
#include "vector/int2.h"
#include "vector/int3.h"
#include "vector/rect.h"
#include "vector/vp.h"

// Euler angles
#include "euler.h"

// Matrix types
#include "matrix/float3x3.h"
#include "matrix/transform.h"
#include "matrix/float4x4.h"

// Quaternion
#include "quaternion/quat.h"

// Geometry types
#include "geometry/segment.h"
#include "geometry/triangle.h"
#include "geometry/aabb.h"
#include "geometry/obb.h"
#include "geometry/sphere.h"
#include "geometry/plane.h"
#include "geometry/frustum.h"
#include "geometry/axis_plane.h"
#include "geometry/normal_cone.h"
#include "geometry/geometry_detail.h"

// Collision
#include "collision/collision.h"

// Culling
#include "culling/aabb_tree.h"
#include "culling/grid.h"

// Spline
#include "spline/curve.h"
#include "spline/hermite.h"
#include "spline/vehicle_curve.h"

// ODE
#include "ode/ode.h"

// Random
#include "random/random_vec.h"
