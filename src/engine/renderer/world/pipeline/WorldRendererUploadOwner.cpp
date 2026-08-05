#include "engine/renderer/world/pipeline/WorldRendererUploadOwner.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace engine::render::world_renderer_detail {

WorldRendererUploadOwner::PaletteSession::PaletteSession(
    d3d12::D3D12Device& device,
    container::Vector<SkinPaletteUploadEntry>& entries,
    container::Vector<SkinPaletteGpuJoint>& pairScratch,
    StaticMeshRenderStats& stats) noexcept
    : m_device(device),
      m_entries(entries),
      m_pairScratch(pairScratch),
      m_stats(stats) {}

d3d12::ConstantBufferAllocation
WorldRendererUploadOwner::PaletteSession::resolve(
    const StaticMeshDrawPacket& draw) {
    if (draw.skinBoneCount == 0) {
        if (!m_identityAllocation) {
            const math::transform identity = math::transform::identity();
            SkinPaletteGpuJoint identityPair{};
            std::memcpy(
                identityPair.previous, &identity.m,
                sizeof(identityPair.previous));
            std::memcpy(
                identityPair.current, &identity.m,
                sizeof(identityPair.current));
            m_identityAllocation = m_device.allocateConstantBuffer(
                &identityPair, sizeof(identityPair));
        }
        return m_identityAllocation;
    }

    for (const SkinPaletteUploadEntry& entry : m_entries) {
        if (draw.sharesSkinPaletteWith(*entry.packet)) {
            if (m_stats.skinPaletteUploadHits !=
                std::numeric_limits<uint32_t>::max()) {
                ++m_stats.skinPaletteUploadHits;
            }
            return entry.allocation;
        }
    }

    m_pairScratch.resize(draw.skinBoneCount);
    for (uint32_t index = 0; index < draw.skinBoneCount; ++index) {
        const math::transform& current = draw.skinPalette[index];
        const math::transform& previous = draw.previousSkinPalette
            ? draw.previousSkinPalette[index] : current;
        std::memcpy(
            m_pairScratch[index].previous, &previous.m,
            sizeof(m_pairScratch[index].previous));
        std::memcpy(
            m_pairScratch[index].current, &current.m,
            sizeof(m_pairScratch[index].current));
    }
    const d3d12::ConstantBufferAllocation allocation =
        m_device.allocateConstantBuffer(
            m_pairScratch.data(), static_cast<uint32_t>(
                m_pairScratch.size() * sizeof(SkinPaletteGpuJoint)));
    if (allocation) {
        m_entries.push_back({&draw, allocation});
        if (m_stats.skinPaletteUploadMisses !=
            std::numeric_limits<uint32_t>::max()) {
            ++m_stats.skinPaletteUploadMisses;
        }
        m_stats.skinPaletteUploadBytes +=
            m_pairScratch.size() * sizeof(SkinPaletteGpuJoint);
    }
    return allocation;
}

WorldRendererUploadOwner::PaletteSession
WorldRendererUploadOwner::beginPaletteUploads(
    d3d12::D3D12Device& device, size_t packetCapacity,
    StaticMeshRenderStats& stats) {
    const size_t capacityBefore = m_skinPaletteEntries.capacity();
    m_skinPaletteEntries.reserve(packetCapacity);
    if (m_skinPaletteEntries.capacity() > capacityBefore &&
        stats.skinPaletteScratchCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++stats.skinPaletteScratchCapacityGrowths;
    }
    const uint32_t capacity = static_cast<uint32_t>(
        std::min<size_t>(m_skinPaletteEntries.capacity(),
                         std::numeric_limits<uint32_t>::max()));
    m_skinPaletteCapacityHighWater = std::max(
        m_skinPaletteCapacityHighWater, capacity);
    stats.skinPaletteScratchCapacity = capacity;
    stats.skinPaletteScratchCapacityHighWater =
        m_skinPaletteCapacityHighWater;
    return PaletteSession{
        device, m_skinPaletteEntries, m_skinPalettePairs, stats};
}

void WorldRendererUploadOwner::resetPaletteEntries() noexcept {
    m_skinPaletteEntries.clear();
}

void WorldRendererUploadOwner::projectCapacities(
    StaticMeshRenderStats& stats) const noexcept {
    stats.instanceScratchCapacity = static_cast<uint32_t>(
        std::min<size_t>(m_instances.capacity(),
                         std::numeric_limits<uint32_t>::max()));
    stats.instanceScratchCapacityHighWater = m_instanceCapacityHighWater;
    stats.skinPaletteScratchCapacity = static_cast<uint32_t>(
        std::min<size_t>(m_skinPaletteEntries.capacity(),
                         std::numeric_limits<uint32_t>::max()));
    stats.skinPaletteScratchCapacityHighWater =
        m_skinPaletteCapacityHighWater;
}

void WorldRendererUploadOwner::noteInstanceReserve(
    size_t capacityBefore, StaticMeshRenderStats& stats) noexcept {
    if (m_instances.capacity() > capacityBefore &&
        stats.instanceScratchCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++stats.instanceScratchCapacityGrowths;
    }
    const uint32_t capacity = static_cast<uint32_t>(
        std::min<size_t>(m_instances.capacity(),
                         std::numeric_limits<uint32_t>::max()));
    m_instanceCapacityHighWater = std::max(
        m_instanceCapacityHighWater, capacity);
    stats.instanceScratchCapacity = capacity;
    stats.instanceScratchCapacityHighWater = m_instanceCapacityHighWater;
}

void WorldRendererUploadOwner::clearFramePointers() noexcept {
    m_skinPaletteEntries.clear();
    m_recordedDraws.clear();
}

} // namespace engine::render::world_renderer_detail
