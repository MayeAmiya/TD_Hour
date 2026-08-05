#pragma once

#include "debug/td_assert.h"

#include <cstdint>

namespace math {

// Overflow-free arithmetic mean for signed fixed-point raw coordinates.
// The divisor is the final sample count; add() may then process values in any
// stable order without widening support such as non-MSVC __int128.
class FixedRawMeanAccumulator final {
public:
    explicit FixedRawMeanAccumulator(int64_t divisor) noexcept
        : m_divisor(divisor) {
        TD_ASSERT(divisor > 0);
    }

    void add(int64_t value) noexcept {
        TD_ASSERT(m_divisor > 0);
        const int64_t quotient = value / m_divisor;
        const int64_t remainder = value % m_divisor;
        if (quotient >= 0) {
            m_positiveQuotient += quotient;
        } else {
            m_negativeQuotient += quotient;
        }
        if (remainder > 0) {
            addPositiveRemainder(remainder);
        } else if (remainder < 0) {
            // A remainder can never be INT64_MIN because its magnitude is
            // strictly smaller than the positive divisor.
            addNegativeRemainder(-remainder);
        }
    }

    [[nodiscard]] int64_t value() const noexcept {
        int64_t quotient = m_positiveQuotient + m_negativeQuotient;
        if (m_positiveRemainder > m_negativeRemainder) {
            // quotient + positive proper fraction truncates toward zero.
            if (quotient < 0) ++quotient;
        } else if (m_negativeRemainder > m_positiveRemainder) {
            // quotient - positive proper fraction truncates toward zero.
            if (quotient > 0) --quotient;
        }
        return quotient;
    }

private:
    void addPositiveRemainder(int64_t remainder) noexcept {
        if (m_positiveRemainder >= m_divisor - remainder) {
            m_positiveRemainder -= m_divisor - remainder;
            ++m_positiveQuotient;
        } else {
            m_positiveRemainder += remainder;
        }
    }

    void addNegativeRemainder(int64_t magnitude) noexcept {
        if (m_negativeRemainder >= m_divisor - magnitude) {
            m_negativeRemainder -= m_divisor - magnitude;
            --m_negativeQuotient;
        } else {
            m_negativeRemainder += magnitude;
        }
    }

    int64_t m_divisor = 1;
    int64_t m_positiveQuotient = 0;
    int64_t m_negativeQuotient = 0;
    int64_t m_positiveRemainder = 0;
    int64_t m_negativeRemainder = 0;
};

} // namespace math
