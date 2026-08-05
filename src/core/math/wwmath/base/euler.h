#pragma once

#include "matrix/transform.h"
#include <limits>
#include <numbers>

namespace math {

// ── Euler Order Constants ──────────────────────────────────────────────
// Encoding: bits 0-1 = i axis, 2-3 = j axis, 4 = parity, 5 = repetition, 6 = frame (0=static,1=rotating)

inline constexpr int EULER_ORDER_XYZ_S = (0 << 0) | (1 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_XYX_S = (0 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_XZY_S = (0 << 0) | (2 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_XZX_S = (0 << 0) | (2 << 2) | (1 << 4) | (1 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_YZX_S = (1 << 0) | (2 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_YZY_S = (1 << 0) | (2 << 2) | (1 << 4) | (1 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_YXZ_S = (1 << 0) | (0 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_YXY_S = (1 << 0) | (0 << 2) | (1 << 4) | (1 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_ZXY_S = (2 << 0) | (0 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_ZXZ_S = (2 << 0) | (0 << 2) | (1 << 4) | (1 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_ZYX_S = (2 << 0) | (1 << 2) | (0 << 4) | (0 << 5) | (0 << 6);
inline constexpr int EULER_ORDER_ZYZ_S = (2 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (0 << 6);

inline constexpr int EULER_ORDER_XYZ_R = (0 << 0) | (1 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_XYX_R = (0 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_XZY_R = (0 << 0) | (2 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_XZX_R = (0 << 0) | (2 << 2) | (1 << 4) | (1 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_YZX_R = (1 << 0) | (2 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_YZY_R = (1 << 0) | (2 << 2) | (1 << 4) | (1 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_YXZ_R = (1 << 0) | (0 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_YXY_R = (1 << 0) | (0 << 2) | (1 << 4) | (1 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_ZXY_R = (2 << 0) | (0 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_ZXZ_R = (2 << 0) | (0 << 2) | (1 << 4) | (1 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_ZYX_R = (2 << 0) | (1 << 2) | (0 << 4) | (0 << 5) | (1 << 6);
inline constexpr int EULER_ORDER_ZYZ_R = (2 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 6);

// ── Euler Angles Class ─────────────────────────────────────────────────
// Based on Ken Shoemake's algorithm from Graphics Gems IV
class euler_angles
{
public:
    double angle[3]{};
    int order = EULER_ORDER_XYZ_S;

    // `order` is a public data member and the i/j axis fields it packs are 2 bits
    // wide, so they can hold 3 while only 0..2 name an axis.  order = 15 decodes
    // to i = j = 3, repetition = 0 and therefore k = 3 - 3 - 3 = -3, which turns
    // the (&f._11)[row * 4 + col] element accessors below into index -9 — an
    // out-of-bounds write nine floats before a stack XMFLOAT4X4.  Nothing
    // validates that `order` is one of the 24 EULER_ORDER_* constants, so the
    // decode is validated here instead.
    [[nodiscard]] static constexpr bool valid_order(int ord) noexcept
    {
        const int i = (ord >> 0) & 3;
        const int j = (ord >> 2) & 3;
        if (i > 2 || j > 2) { return false; }
        const int repetition = (ord >> 5) & 1;
        const int k = repetition ? j : (3 - i - j);
        return k >= 0 && k <= 2;
    }

    euler_angles() noexcept = default;
    euler_angles(const transform& from, int ord) noexcept
        : order(ord)
    {
        from_matrix(from, ord);
    }

    void from_matrix(const transform& from, int ord) noexcept
    {
        if (!valid_order(ord))
        {
            order = EULER_ORDER_XYZ_S;
            angle[0] = angle[1] = angle[2] = 0.0;
            return;
        }
        order = ord;

        int i = (ord >> 0) & 3;
        int j = (ord >> 2) & 3;
        int parity = (ord >> 4) & 1;
        int repetition = (ord >> 5) & 1;
        int frame = (ord >> 6) & 1;

        // Determine k axis
        int k = repetition ? j : (3 - i - j);

        // Get matrix elements
        DirectX::XMMATRIX m = from.load();
        DirectX::XMFLOAT4X4 f;
        DirectX::XMStoreFloat4x4(&f, m);

        // Access matrix elements using axis indices:
        // m[axis][axis] where axis = 0(X),1(Y),2(Z)
        auto elem = [&](int row, int col) -> float {
            return (&f._11)[row * 4 + col];
        };

        double cy = std::sqrt(
            static_cast<double>(elem(i, k)) * elem(i, k) +
            static_cast<double>(elem(j, k)) * elem(j, k));
        if (cy > 16.0 * std::numeric_limits<double>::epsilon())
        {
            angle[0] = std::atan2(
                static_cast<double>(elem(j, k)),
                static_cast<double>(elem(i, k)));
            angle[1] = std::atan2(
                static_cast<double>(elem(k, k)), cy);
            angle[2] = std::atan2(
                static_cast<double>(elem(k, j)),
                -static_cast<double>(elem(k, i)));
        }
        else
        {
            angle[0] = std::atan2(
                -static_cast<double>(elem(j, i)),
                static_cast<double>(elem(j, j)));
            angle[1] = std::atan2(
                static_cast<double>(elem(k, k)), cy);
            angle[2] = 0.0;
        }

        if (parity)
        {
            angle[0] = -angle[0];
            angle[1] = -angle[1];
            angle[2] = -angle[2];
        }
        if (frame)
        {
            // Swap for rotating frame
            double tmp = angle[0];
            angle[0] = angle[2];
            angle[2] = tmp;
        }
    }

    void to_matrix(transform& m) const noexcept
    {
        if (!valid_order(order))
        {
            m = transform::identity();
            return;
        }
        int i = (order >> 0) & 3;
        int j = (order >> 2) & 3;
        int parity = (order >> 4) & 1;
        int repetition = (order >> 5) & 1;
        int frame = (order >> 6) & 1;

        int k = repetition ? j : (3 - i - j);

        double a[3] = {angle[0], angle[1], angle[2]};
        if (frame)
        {
            double tmp = a[0];
            a[0] = a[2];
            a[2] = tmp;
        }
        if (parity)
        {
            a[0] = -a[0];
            a[1] = -a[1];
            a[2] = -a[2];
        }

        double si = std::sin(a[0]), ci = std::cos(a[0]);
        double sj = std::sin(a[1]), cj = std::cos(a[1]);
        double sk = std::sin(a[2]), ck = std::cos(a[2]);

        double cc = ci * ck;
        double cs = ci * sk;
        double sc = si * ck;
        double ss = si * sk;

        DirectX::XMFLOAT4X4 mat;
        auto set = [&](int row, int col, double val) {
            (&mat._11)[row * 4 + col] = static_cast<float>(val);
        };

        if (repetition)
        {
            set(i, i, cj);
            set(i, j, sj * si);
            set(i, k, sj * ci);
            set(j, i, sj * sk);
            set(j, j, -cj * ss + cc);
            set(j, k, -cj * cs - sc);
            set(k, i, -sj * ck);
            set(k, j, cj * sc + cs);
            set(k, k, cj * cc - ss);
        }
        else
        {
            set(i, i, cj * ck);
            set(i, j, sj * sc - cs);
            set(i, k, sj * cc + ss);
            set(j, i, cj * sk);
            set(j, j, sj * ss + cc);
            set(j, k, sj * cs - sc);
            set(k, i, -sj);
            set(k, j, cj * si);
            set(k, k, cj * ci);
        }
        set(0, 3, 0); set(1, 3, 0); set(2, 3, 0); set(3, 3, 1);
        set(3, 0, 0); set(3, 1, 0); set(3, 2, 0);

        m.store(DirectX::XMLoadFloat4x4(&mat));
    }

    double get_angle(int i) const noexcept { return angle[i]; }
};

} // namespace math
