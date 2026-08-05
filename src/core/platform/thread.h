#pragma once

#include "container/container_types.h"

#include <thread>
#include <functional>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
#endif

namespace platform {

class thread
{
public:
    thread() noexcept = default;

    template <typename F, typename... Args>
    explicit thread(F&& f, Args&&... args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...)
    {
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& other) noexcept
        : t_(std::move(other.t_))
    {
    }

    thread& operator=(thread&& other) noexcept
    {
        if (this != &other)
        {
            join();
            t_ = std::move(other.t_);
        }
        return *this;
    }

    ~thread()
    {
        join();
    }

    void join()
    {
        if (t_.joinable()) t_.join();
    }

    void detach()
    {
        if (t_.joinable()) t_.detach();
    }

    [[nodiscard]] bool joinable() const noexcept { return t_.joinable(); }
    [[nodiscard]] std::thread::id get_id() const noexcept { return t_.get_id(); }

    [[nodiscard]] std::thread move() && { return std::move(t_); }

private:
    std::thread t_;
};

inline void set_thread_name(const char* name)
{
#if defined(_WIN32)
    auto h = ::GetCurrentThread();
    auto wlen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wlen > 0)
    {
        container::WString wname(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), static_cast<int>(wname.size()));
        ::SetThreadDescription(h, wname.c_str());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

} // namespace platform
