#pragma once

#include "container/container_types.h"

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
namespace core {

// Dense value storage with stable, generation-checked handles. Erase uses a
// swap-remove in the dense arrays and updates the moved value's slot, so hot
// iteration remains contiguous while stale presentation handles fail closed.
template <typename T>
class generational_slot_map final {
public:
    static constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();

    struct handle final {
        uint32_t index = invalid_index;
        uint32_t generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept {
            return index != invalid_index && generation != 0;
        }
        friend bool operator==(handle, handle) = default;
    };

    generational_slot_map() = default;
    generational_slot_map(const generational_slot_map&) = delete;
    generational_slot_map& operator=(const generational_slot_map&) = delete;
    generational_slot_map(generational_slot_map&&) noexcept = default;
    generational_slot_map& operator=(generational_slot_map&&) noexcept = default;

    void reserve(size_t capacity) {
        m_values.reserve(capacity);
        m_denseToSlot.reserve(capacity);
        m_slots.reserve(capacity);
    }

    template <typename... Args>
    [[nodiscard]] handle emplace(Args&&... args) {
        uint32_t slotIndex = invalid_index;
        if (m_freeHead != invalid_index) {
            slotIndex = m_freeHead;
            slot& reused = m_slots[slotIndex];
            m_freeHead = reused.nextFree;
            reused.nextFree = invalid_index;
            reused.denseIndex = static_cast<uint32_t>(m_values.size());
            reused.occupied = true;
        } else {
            slotIndex = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back({
                .generation = 1,
                .denseIndex = static_cast<uint32_t>(m_values.size()),
                .nextFree = invalid_index,
                .occupied = true,
            });
        }

        const size_t oldSize = m_values.size();
        try {
            m_values.emplace_back(std::forward<Args>(args)...);
            m_denseToSlot.push_back(slotIndex);
        } catch (...) {
            if (m_values.size() != oldSize) m_values.pop_back();
            slot& failed = m_slots[slotIndex];
            failed.occupied = false;
            failed.denseIndex = invalid_index;
            failed.nextFree = m_freeHead;
            m_freeHead = slotIndex;
            throw;
        }
        return {.index = slotIndex, .generation = m_slots[slotIndex].generation};
    }

    [[nodiscard]] bool contains(handle value) const noexcept {
        return value.index < m_slots.size() && value.generation != 0 &&
            m_slots[value.index].occupied &&
            m_slots[value.index].generation == value.generation;
    }

    [[nodiscard]] T* get(handle value) noexcept {
        return contains(value) ? &m_values[m_slots[value.index].denseIndex] : nullptr;
    }

    [[nodiscard]] const T* get(handle value) const noexcept {
        return contains(value) ? &m_values[m_slots[value.index].denseIndex] : nullptr;
    }

    [[nodiscard]] bool erase(handle value) {
        if (!contains(value)) return false;
        slot& removedSlot = m_slots[value.index];
        const uint32_t removedDense = removedSlot.denseIndex;
        const uint32_t lastDense = static_cast<uint32_t>(m_values.size() - 1);
        if (removedDense != lastDense) {
            m_values[removedDense] = std::move(m_values[lastDense]);
            const uint32_t movedSlotIndex = m_denseToSlot[lastDense];
            m_denseToSlot[removedDense] = movedSlotIndex;
            m_slots[movedSlotIndex].denseIndex = removedDense;
        }
        m_values.pop_back();
        m_denseToSlot.pop_back();

        removedSlot.occupied = false;
        removedSlot.denseIndex = invalid_index;
        removedSlot.generation = nextGeneration(removedSlot.generation);
        removedSlot.nextFree = m_freeHead;
        m_freeHead = value.index;
        return true;
    }

    void clear() noexcept {
        m_values.clear();
        m_denseToSlot.clear();
        m_freeHead = invalid_index;
        for (size_t index = m_slots.size(); index > 0; --index) {
            slot& value = m_slots[index - 1];
            value.occupied = false;
            value.denseIndex = invalid_index;
            value.generation = nextGeneration(value.generation);
            value.nextFree = m_freeHead;
            m_freeHead = static_cast<uint32_t>(index - 1);
        }
    }

    [[nodiscard]] size_t size() const noexcept { return m_values.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_values.empty(); }
    [[nodiscard]] size_t slot_count() const noexcept { return m_slots.size(); }

    [[nodiscard]] container::Vector<T>& values() noexcept { return m_values; }
    [[nodiscard]] const container::Vector<T>& values() const noexcept { return m_values; }

    [[nodiscard]] handle handle_at_dense_index(size_t index) const noexcept {
        if (index >= m_denseToSlot.size()) return {};
        const uint32_t slotIndex = m_denseToSlot[index];
        return {.index = slotIndex, .generation = m_slots[slotIndex].generation};
    }

private:
    struct slot final {
        uint32_t generation = 1;
        uint32_t denseIndex = invalid_index;
        uint32_t nextFree = invalid_index;
        bool occupied = false;
    };

    [[nodiscard]] static uint32_t nextGeneration(uint32_t value) noexcept {
        ++value;
        return value == 0 ? 1 : value;
    }

    container::Vector<T> m_values;
    container::Vector<uint32_t> m_denseToSlot;
    container::Vector<slot> m_slots;
    uint32_t m_freeHead = invalid_index;
};

} // namespace core
