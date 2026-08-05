#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace platform::runtime {

// Ordered non-blocking hand-off for immutable history that must never make
// its producer wait for a slower presentation consumer. The queue is
// intentionally unbounded: callers use it only for short-lived endpoint
// history whose downstream pipeline is expected to catch up. Retiring deep
// values happens outside the mutex.
template <typename T>
class UnboundedMailbox final {
public:
    UnboundedMailbox() = default;
    ~UnboundedMailbox() = default;

    UnboundedMailbox(const UnboundedMailbox&) = delete;
    UnboundedMailbox& operator=(const UnboundedMailbox&) = delete;

    [[nodiscard]] bool push(T value) {
        auto next = std::make_unique<T>(std::move(value));
        std::lock_guard lock(m_mutex);
        if (m_closed) return false;
        m_values.push_back(std::move(next));
        return true;
    }

    [[nodiscard]] std::optional<T> tryPop() {
        std::unique_ptr<T> value;
        {
            std::lock_guard lock(m_mutex);
            if (m_values.empty()) return std::nullopt;
            value = std::move(m_values.front());
            m_values.pop_front();
        }
        return std::move(*value);
    }

    void close() noexcept {
        std::deque<std::unique_ptr<T>> retired;
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            retired.swap(m_values);
        }
    }

    void reset() {
        std::deque<std::unique_ptr<T>> retired;
        {
            std::lock_guard lock(m_mutex);
            retired.swap(m_values);
            m_closed = false;
        }
    }

    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard lock(m_mutex);
        return m_values.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<std::unique_ptr<T>> m_values;
    bool m_closed = false;
};

// 表现帧只关心最新完整状态。生产者覆盖尚未消费的旧帧，既不阻塞主/逻辑
// 线程，也不会让渲染线程追赶已经过期的 UI 或相机状态。
template <typename T>
class LatestValueMailbox final {
public:
    LatestValueMailbox() = default;
    ~LatestValueMailbox() = default;

    LatestValueMailbox(const LatestValueMailbox&) = delete;
    LatestValueMailbox& operator=(const LatestValueMailbox&) = delete;

    [[nodiscard]] bool publish(T value) {
        // Moving the previous endpoint out is normally O(1) for snapshot
        // values, but destroying its vectors/shared ownership can be deep.
        // Never perform that retirement while holding the producer mutex:
        // a slow destructor must not extend the logic->presentation critical
        // section or block the consumer from taking the new endpoint.
        auto next = std::make_unique<T>(std::move(value));
        std::unique_ptr<T> retired;
        bool accepted = false;
        {
            std::lock_guard lock(m_mutex);
            if (!m_closed) {
                retired = std::move(m_value);
                m_value = std::move(next);
                ++m_revision;
                accepted = true;
            }
        }
        if (!accepted) return false;
        m_changed.notify_one();
        return true;
    }

    [[nodiscard]] bool waitTake(T& value,
                                std::stop_token stopToken = {}) {
        std::unique_ptr<T> endpoint;
        std::unique_lock lock(m_mutex);
        const bool readable = m_changed.wait(
            lock, stopToken,
            [this] { return m_closed || static_cast<bool>(m_value); });
        if (!readable || !m_value) return false;
        endpoint = std::move(m_value);
        lock.unlock();
        value = std::move(*endpoint);
        return true;
    }

    [[nodiscard]] bool tryTake(T& value) {
        std::unique_ptr<T> endpoint;
        {
            std::lock_guard lock(m_mutex);
            if (!m_value) return false;
            endpoint = std::move(m_value);
        }
        value = std::move(*endpoint);
        return true;
    }

    void close() noexcept {
        std::unique_ptr<T> retired;
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            retired = std::move(m_value);
        }
        m_changed.notify_all();
    }

    void reset() {
        std::unique_ptr<T> retired;
        {
            std::lock_guard lock(m_mutex);
            retired = std::move(m_value);
            m_closed = false;
            m_revision = 0;
        }
    }

    [[nodiscard]] uint64_t revision() const noexcept {
        std::lock_guard lock(m_mutex);
        return m_revision;
    }

    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard lock(m_mutex);
        return m_value ? 1u : 0u;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_changed;
    std::unique_ptr<T> m_value;
    uint64_t m_revision = 0;
    bool m_closed = false;
};

// 有界、无损的跨线程邮箱。生产者在队列满时形成显式背压；close()
// 会唤醒全部等待者，用于运行时按确定顺序停止线程。
template <typename T, size_t Capacity>
class BoundedMailbox final {
    static_assert(Capacity > 0u);

public:
    BoundedMailbox() = default;
    ~BoundedMailbox() = default;

    BoundedMailbox(const BoundedMailbox&) = delete;
    BoundedMailbox& operator=(const BoundedMailbox&) = delete;

    [[nodiscard]] bool push(T value, std::stop_token stopToken = {}) {
        std::unique_lock lock(m_mutex);
        const bool writable = m_notFull.wait(
            lock, stopToken,
            [this] { return m_closed || m_size < Capacity; });
        if (!writable || m_closed) return false;
        const size_t tail = (m_head + m_size) % Capacity;
        m_values[tail].emplace(std::move(value));
        ++m_size;
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    [[nodiscard]] bool tryPush(T value) {
        std::unique_lock lock(m_mutex);
        if (m_closed || m_size >= Capacity) return false;
        const size_t tail = (m_head + m_size) % Capacity;
        m_values[tail].emplace(std::move(value));
        ++m_size;
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    [[nodiscard]] bool waitPop(T& value,
                               std::stop_token stopToken = {}) {
        std::unique_lock lock(m_mutex);
        const bool readable = m_notEmpty.wait(
            lock, stopToken,
            [this] { return m_closed || m_size != 0u; });
        if (!readable || m_size == 0u) return false;
        value = std::move(*m_values[m_head]);
        m_values[m_head].reset();
        m_head = (m_head + 1u) % Capacity;
        --m_size;
        lock.unlock();
        m_notFull.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> tryPop() {
        std::unique_lock lock(m_mutex);
        if (m_size == 0u) return std::nullopt;
        T value = std::move(*m_values[m_head]);
        m_values[m_head].reset();
        m_head = (m_head + 1u) % Capacity;
        --m_size;
        lock.unlock();
        m_notFull.notify_one();
        return value;
    }

    template <typename Consumer>
    size_t drain(Consumer&& consumer) {
        std::vector<T> values;
        {
            std::lock_guard lock(m_mutex);
            values.reserve(m_size);
            while (m_size != 0u) {
                values.push_back(std::move(*m_values[m_head]));
                m_values[m_head].reset();
                m_head = (m_head + 1u) % Capacity;
                --m_size;
            }
        }
        if (!values.empty()) m_notFull.notify_all();
        for (T& value : values) consumer(std::move(value));
        return values.size();
    }

    void close() noexcept {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void reset() {
        {
            std::lock_guard lock(m_mutex);
            while (m_size != 0u) {
                m_values[m_head].reset();
                m_head = (m_head + 1u) % Capacity;
                --m_size;
            }
            m_head = 0u;
            m_closed = false;
        }
        m_notFull.notify_all();
    }

    [[nodiscard]] bool closed() const noexcept {
        std::lock_guard lock(m_mutex);
        return m_closed;
    }

    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard lock(m_mutex);
        return m_size;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_notEmpty;
    std::condition_variable_any m_notFull;
    std::array<std::optional<T>, Capacity> m_values;
    size_t m_head = 0;
    size_t m_size = 0;
    bool m_closed = false;
};

} // namespace platform::runtime
