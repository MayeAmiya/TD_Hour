#pragma once

#include <chrono>
#include <thread>

namespace platform {

class timer
{
public:
    using clock = std::chrono::steady_clock;

    timer() noexcept
        : start_(clock::now())
    {
    }

    void reset() noexcept
    {
        start_ = clock::now();
    }

    [[nodiscard]] float elapsed_sec() const noexcept
    {
        return std::chrono::duration<float>(clock::now() - start_).count();
    }

    [[nodiscard]] double elapsed_sec_d() const noexcept
    {
        return std::chrono::duration<double>(clock::now() - start_).count();
    }

    [[nodiscard]] int64_t elapsed_ms() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - start_).count();
    }

    [[nodiscard]] int64_t elapsed_us() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - start_).count();
    }

    [[nodiscard]] auto now() const noexcept { return clock::now(); }
    [[nodiscard]] auto start() const noexcept { return start_; }

private:
    clock::time_point start_;
};

inline void sleep_ms(int64_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace platform
