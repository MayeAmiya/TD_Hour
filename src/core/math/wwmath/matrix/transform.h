#pragma once

#include "../quaternion/quat.h"
#include "../vector/float3.h"
#include <DirectXMath.h>

namespace math {

class transform
{
public:
    DirectX::XMFLOAT4X4 m{};

    transform() noexcept { set_identity(); }

    explicit transform(DirectX::FXMMATRIX mat) noexcept
    {
        store(mat);
    }

    void set_identity() noexcept
    {
        DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
    }

    DirectX::XMMATRIX load() const noexcept { return DirectX::XMLoadFloat4x4(&m); }
    void store(DirectX::FXMMATRIX mat) noexcept { DirectX::XMStoreFloat4x4(&m, mat); }
    operator DirectX::XMMATRIX() const noexcept { return load(); }
    operator const float*() const noexcept { return &m._11; }

    // Axis access
    vec3 right()       const noexcept { return vec3{load().r[0]}; }
    vec3 up()          const noexcept { return vec3{load().r[1]}; }
    vec3 forward()     const noexcept { return vec3{load().r[2]}; }
    vec3 translation() const noexcept
    {
        DirectX::XMVECTOR t = load().r[3];
        return vec3{t};
    }

    void set_translation(vec3 t) noexcept
    {
        DirectX::XMMATRIX mat = load();
        mat.r[3] = DirectX::XMVectorSelect(t, mat.r[3], DirectX::XMVectorSelectControl(0, 0, 0, 1));
        store(mat);
    }

    // Transform operations
    vec3 transform_point(vec3 pt) const noexcept
    {
        return vec3{DirectX::XMVector3Transform(pt, load())};
    }
    vec3 transform_dir(vec3 dir) const noexcept
    {
        return vec3{DirectX::XMVector3TransformNormal(dir, load())};
    }
    void transform_points(vec3* dst, const vec3* src, int count) const noexcept
    {
        DirectX::XMMATRIX mat = load();
        for (int i = 0; i < count; ++i)
        {
            dst[i] = vec3{DirectX::XMVector3Transform(src[i], mat)};
        }
    }

    // Composition (post-multiply: M * T)
    transform& rotate_x(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), DirectX::XMMatrixRotationX(angle)));
        return *this;
    }
    transform& rotate_y(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), DirectX::XMMatrixRotationY(angle)));
        return *this;
    }
    transform& rotate_z(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), DirectX::XMMatrixRotationZ(angle)));
        return *this;
    }
    transform& translate(vec3 t) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), DirectX::XMMatrixTranslation(t.x(), t.y(), t.z())));
        return *this;
    }
    transform& scale(vec3 s) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), DirectX::XMMatrixScaling(s.x(), s.y(), s.z())));
        return *this;
    }

    transform& pre_multiply(const transform& lhs) noexcept
    {
        store(DirectX::XMMatrixMultiply(lhs.load(), load()));
        return *this;
    }

    [[nodiscard]] transform inverse() const noexcept
    {
        return transform{DirectX::XMMatrixInverse(nullptr, load())};
    }

    [[nodiscard]] transform orthogonal_inverse() const noexcept
    {
        return transform{DirectX::XMMatrixInverse(nullptr, load())};
    }

    // Static builders
    static transform identity() noexcept { return transform{}; }
    static transform rotation_x(float angle) noexcept { return transform{DirectX::XMMatrixRotationX(angle)}; }
    static transform rotation_y(float angle) noexcept { return transform{DirectX::XMMatrixRotationY(angle)}; }
    static transform rotation_z(float angle) noexcept { return transform{DirectX::XMMatrixRotationZ(angle)}; }
    static transform translation(vec3 t) noexcept
    {
        return transform{DirectX::XMMatrixTranslation(t.x(), t.y(), t.z())};
    }
    static transform scale_uniform(float s) noexcept
    {
        return transform{DirectX::XMMatrixScaling(s, s, s)};
    }
    static transform from_trs(vec3 scale, quat rotation, vec3 translation) noexcept
    {
        return transform{DirectX::XMMatrixAffineTransformation(
            scale, DirectX::XMVectorZero(), rotation, translation)};
    }
    static transform look_at(vec3 eye, vec3 target, vec3 up = vec3::up()) noexcept
    {
        return transform{DirectX::XMMatrixLookAtLH(eye, target, up)};
    }
    static transform from_axes(vec3 right, vec3 up, vec3 forward, vec3 pos) noexcept
    {
        DirectX::XMVECTOR r = right;
        DirectX::XMVECTOR u = up;
        DirectX::XMVECTOR f = forward;
        DirectX::XMVECTOR p = pos;
        DirectX::XMMATRIX mat;
        // Row-vector affine layout: axis rows carry xyz,w=0 and the final
        // row carries position xyz,w=1.  XMVectorSelect's control bit chooses
        // its second operand; the previous reversed operands therefore
        // zeroed every axis and replaced position xyz with one.
        mat.r[0] = DirectX::XMVectorSetW(r, 0.0f);
        mat.r[1] = DirectX::XMVectorSetW(u, 0.0f);
        mat.r[2] = DirectX::XMVectorSetW(f, 0.0f);
        mat.r[3] = DirectX::XMVectorSetW(p, 1.0f);
        return transform{mat};
    }

    // Arithmetic
    transform operator*(const transform& r) const noexcept
    {
        return transform{DirectX::XMMatrixMultiply(load(), r)};
    }
    transform& operator*=(const transform& r) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), r));
        return *this;
    }

    // Comparison
    bool operator==(const transform& r) const noexcept
    {
        return DirectX::XMVector4Equal(load().r[0], r.load().r[0])
            && DirectX::XMVector4Equal(load().r[1], r.load().r[1])
            && DirectX::XMVector4Equal(load().r[2], r.load().r[2])
            && DirectX::XMVector4Equal(load().r[3], r.load().r[3]);
    }
};

// ── quat::from_matrix definition (needs full transform) ────────────────
inline quat quat::from_matrix(const transform& tm) noexcept
{
    return quat{DirectX::XMQuaternionRotationMatrix(tm.load())};
}

} // namespace math
