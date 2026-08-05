#pragma once

#include "core/container/container_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include "debug/td_assert.h"

namespace engine::fx {

struct AoSoABlockIndex final {
    uint32_t value = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != std::numeric_limits<uint32_t>::max();
    }
    friend bool operator==(AoSoABlockIndex, AoSoABlockIndex) = default;
};

// Stable, aligned backing storage for module-owned AoSoA blocks. It manages
// block capacity only: lane allocation, compaction, priorities and lifetime
// remain policies of BillboardParticlePool, TrailPointPool, and later users.
template <typename Block, uint32_t BlocksPerPage = 64>
class AoSoABlockStorage final {
    static_assert(BlocksPerPage > 0);
    static_assert(std::is_default_constructible_v<Block>);
    static_assert(std::is_move_assignable_v<Block>);

public:
    explicit AoSoABlockStorage(uint32_t maximumBlocks = 0)
        : m_maximumBlocks(maximumBlocks) {}

    AoSoABlockStorage(const AoSoABlockStorage&) = delete;
    AoSoABlockStorage& operator=(const AoSoABlockStorage&) = delete;
    AoSoABlockStorage(AoSoABlockStorage&&) noexcept = default;
    AoSoABlockStorage& operator=(AoSoABlockStorage&&) noexcept = default;

    void reserveBlocks(uint32_t blockCount) {
        if (m_maximumBlocks != 0) blockCount = std::min(blockCount, m_maximumBlocks);
        const uint32_t pageCount = pagesFor(blockCount);
        m_pages.reserve(pageCount);
        while (m_pages.size() < pageCount) {
            m_pages.push_back(std::make_unique<Page>());
        }
        m_live.reserve(blockCount);
    }

    [[nodiscard]] std::optional<AoSoABlockIndex> allocateBlock() {
        uint32_t index = 0;
        if (!m_freeBlocks.empty()) {
            index = m_freeBlocks.back();
            m_freeBlocks.pop_back();
        } else {
            if (m_maximumBlocks != 0 && m_nextBlock >= m_maximumBlocks) return std::nullopt;
            index = m_nextBlock++;
            ensurePage(index / BlocksPerPage);
            if (m_live.size() <= index) m_live.resize(static_cast<size_t>(index) + 1, false);
        }
        TD_ASSERT(index < m_live.size() && !m_live[index]);
        m_live[index] = true;
        ++m_allocatedBlocks;
        m_highWaterBlocks = std::max(m_highWaterBlocks, m_allocatedBlocks);
        return AoSoABlockIndex{index};
    }

    [[nodiscard]] bool releaseBlock(AoSoABlockIndex index) {
        if (!contains(index)) return false;
        get(index) = Block{};
        m_live[index.value] = false;
        m_freeBlocks.push_back(index.value);
        --m_allocatedBlocks;
        return true;
    }

    [[nodiscard]] bool contains(AoSoABlockIndex index) const noexcept {
        return index && index.value < m_live.size() && m_live[index.value];
    }

    [[nodiscard]] Block& get(AoSoABlockIndex index) noexcept {
        TD_ASSERT(contains(index));
        return m_pages[index.value / BlocksPerPage]->blocks[index.value % BlocksPerPage];
    }

    [[nodiscard]] const Block& get(AoSoABlockIndex index) const noexcept {
        TD_ASSERT(contains(index));
        return m_pages[index.value / BlocksPerPage]->blocks[index.value % BlocksPerPage];
    }

    void clear(bool releaseMemory = false) {
        for (uint32_t index = 0; index < m_nextBlock; ++index) {
            if (index < m_live.size() && m_live[index]) {
                m_pages[index / BlocksPerPage]->blocks[index % BlocksPerPage] = Block{};
            }
        }
        m_live.clear();
        m_freeBlocks.clear();
        m_nextBlock = 0;
        m_allocatedBlocks = 0;
        if (releaseMemory) m_pages.clear();
    }

    [[nodiscard]] uint32_t allocatedBlockCount() const noexcept { return m_allocatedBlocks; }
    [[nodiscard]] uint32_t highWaterBlockCount() const noexcept { return m_highWaterBlocks; }
    [[nodiscard]] uint32_t maximumBlockCount() const noexcept { return m_maximumBlocks; }
    [[nodiscard]] size_t pageCount() const noexcept { return m_pages.size(); }

private:
    struct alignas(Block) Page final {
        container::Array<Block, BlocksPerPage> blocks{};
    };

    [[nodiscard]] static constexpr uint32_t pagesFor(uint32_t blocks) noexcept {
        return blocks == 0 ? 0 : (blocks + BlocksPerPage - 1) / BlocksPerPage;
    }

    void ensurePage(uint32_t pageIndex) {
        while (m_pages.size() <= pageIndex) {
            m_pages.push_back(std::make_unique<Page>());
        }
    }

    container::Vector<container::UniquePtr<Page>> m_pages;
    container::Vector<uint32_t> m_freeBlocks;
    container::Vector<bool> m_live;
    uint32_t m_maximumBlocks = 0;
    uint32_t m_nextBlock = 0;
    uint32_t m_allocatedBlocks = 0;
    uint32_t m_highWaterBlocks = 0;
};

} // namespace engine::fx
