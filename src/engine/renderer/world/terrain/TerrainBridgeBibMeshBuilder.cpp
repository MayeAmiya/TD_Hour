#include "engine/renderer/world/terrain/TerrainBridgeBibMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace engine::render {
namespace {

void appendBridgeBox(TerrainBridgeMeshCpu& output,
                     math::vec3 center,
                     math::vec2 forward,
                     math::vec2 side,
                     float halfForward,
                     float halfSide,
                     float height) {
    const uint32_t first = static_cast<uint32_t>(output.vertices.size());
    const container::Array<math::vec2, 4> footprint{{
        {center.x() - forward.x() * halfForward - side.x() * halfSide,
         center.y() - forward.y() * halfForward - side.y() * halfSide},
        {center.x() + forward.x() * halfForward - side.x() * halfSide,
         center.y() + forward.y() * halfForward - side.y() * halfSide},
        {center.x() + forward.x() * halfForward + side.x() * halfSide,
         center.y() + forward.y() * halfForward + side.y() * halfSide},
        {center.x() - forward.x() * halfForward + side.x() * halfSide,
         center.y() - forward.y() * halfForward + side.y() * halfSide},
    }};
    for (uint32_t level = 0; level < 2u; ++level) {
        for (uint32_t corner = 0; corner < 4u; ++corner) {
            StaticMeshVertex vertex;
            vertex.position = {
                footprint[corner].x(), footprint[corner].y(),
                center.z() + (level == 0u ? 0.0f : height)};
            vertex.normal = level == 0u
                ? math::vec3{0.0f, 0.0f, -1.0f}
                : math::vec3{0.0f, 0.0f, 1.0f};
            vertex.texcoord = {
                static_cast<float>(corner & 1u),
                static_cast<float>((corner >> 1u) ^ (corner & 1u)),
            };
            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
            output.vertices.push_back(vertex);
        }
    }
    output.indices.insert(output.indices.end(), {
        first + 4u, first + 5u, first + 6u,
        first + 4u, first + 6u, first + 7u,
        first + 0u, first + 2u, first + 1u,
        first + 0u, first + 3u, first + 2u,
        first + 0u, first + 1u, first + 5u,
        first + 0u, first + 5u, first + 4u,
        first + 1u, first + 2u, first + 6u,
        first + 1u, first + 6u, first + 5u,
        first + 2u, first + 3u, first + 7u,
        first + 2u, first + 7u, first + 6u,
        first + 3u, first + 0u, first + 4u,
        first + 3u, first + 4u, first + 7u,
    });
}

} // namespace

bool buildTerrainBridgeMesh(const TerrainBridgeRenderData& source,
                            TerrainBridgeMeshCpu& output) {
    math::vec3 delta = source.end - source.start;
    const float length = delta.length();
    math::vec2 forward{delta.x(), delta.y()};
    const float planarLength = forward.length();
    if (!std::isfinite(length) || !std::isfinite(planarLength) ||
        length <= math::EPSILON || planarLength <= math::EPSILON ||
        !std::isfinite(source.bridgeWidth) || source.bridgeWidth <= 0.0f) {
        return false;
    }
    forward = forward / planarLength;
    const math::vec2 side{-forward.y(), forward.x()};
    const float halfWidth = source.bridgeWidth * 0.5f;
    const uint32_t spanCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(length / 50.0f)), 1u, 256u);
    const float damageSag = [&source]() noexcept {
        switch (source.damageState) {
        case TerrainBridgeDamageState::Pristine: return 0.0f;
        case TerrainBridgeDamageState::Damaged: return 1.5f;
        case TerrainBridgeDamageState::ReallyDamaged: return 4.0f;
        case TerrainBridgeDamageState::Rubble: return 8.0f;
        }
        return 0.0f;
    }();
    output.vertices.reserve(static_cast<size_t>(spanCount + 1u) * 4u + 32u);
    output.indices.reserve(static_cast<size_t>(spanCount) * 18u + 144u);
    for (uint32_t row = 0; row <= spanCount; ++row) {
        const float t = static_cast<float>(row) /
            static_cast<float>(spanCount);
        math::vec3 center = source.start + delta * t;
        center[2] -= std::sin(t * math::PI) * damageSag;
        const float deckBottom = center.z() - std::max(
            0.75f, source.bridgeWidth * 0.04f);
        for (uint32_t level = 0; level < 2u; ++level) {
            for (float sign : {-1.0f, 1.0f}) {
                StaticMeshVertex vertex;
                vertex.position = {
                    center.x() + side.x() * halfWidth * sign,
                    center.y() + side.y() * halfWidth * sign,
                    level == 0u ? center.z() : deckBottom,
                };
                vertex.normal = level == 0u
                    ? math::vec3{0.0f, 0.0f, 1.0f}
                    : math::vec3{side.x() * sign, side.y() * sign, 0.0f};
                vertex.texcoord = {
                    t * static_cast<float>(spanCount),
                    sign < 0.0f ? 0.0f : 1.0f,
                };
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                output.vertices.push_back(vertex);
            }
        }
        if (row == spanCount) continue;
        const uint32_t base = row * 4u;
        const bool rubbleGap =
            source.damageState == TerrainBridgeDamageState::Rubble &&
            row == spanCount / 2u;
        if (!rubbleGap) {
            output.indices.insert(output.indices.end(), {
                base + 0u, base + 4u, base + 5u,
                base + 0u, base + 5u, base + 1u,
            });
        }
        output.indices.insert(output.indices.end(), {
            base + 0u, base + 2u, base + 6u,
            base + 0u, base + 6u, base + 4u,
            base + 1u, base + 5u, base + 7u,
            base + 1u, base + 7u, base + 3u,
        });
    }
    const float postSize = std::max(0.75f, source.bridgeWidth * 0.06f);
    const float postHeight = std::max(5.0f, source.bridgeWidth * 0.5f);
    for (uint32_t endpoint = 0; endpoint < 2u; ++endpoint) {
        const math::vec3 center = endpoint == 0u ? source.start : source.end;
        for (uint32_t sideIndex = 0; sideIndex < 2u; ++sideIndex) {
            const uint32_t towerIndex = endpoint * 2u + sideIndex;
            if (source.towerObjectNames[towerIndex].empty()) continue;
            const float sign = sideIndex == 0u ? 1.0f : -1.0f;
            appendBridgeBox(
                output,
                {center.x() + side.x() * halfWidth * sign,
                 center.y() + side.y() * halfWidth * sign,
                 center.z()},
                forward, side, postSize, postSize, postHeight);
        }
    }
    return !output.indices.empty();
}

bool buildTerrainBibMesh(const TerrainBibRenderData& source,
                         TerrainBibMeshCpu& output) {
    for (const RenderVector& corner : source.corners) {
        if (!std::isfinite(corner.x()) || !std::isfinite(corner.y()) ||
            !std::isfinite(corner.z())) {
            return false;
        }
    }
    output.vertices.reserve(4u);
    const container::Array<math::vec2, 4> uv{{
        {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f},
    }};
    for (size_t index = 0; index < source.corners.size(); ++index) {
        StaticMeshVertex vertex;
        vertex.position = source.corners[index];
        vertex.normal = {0.0f, 0.0f, 1.0f};
        vertex.texcoord = uv[index];
        vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
        output.vertices.push_back(vertex);
    }
    output.indices = {0u, 1u, 2u, 0u, 2u, 3u};
    return true;
}

std::optional<TerrainBridgeMeshInspection>
inspectTerrainBridgeMesh(const TerrainBridgeRenderData& bridge) {
    TerrainBridgeMeshCpu cpu;
    if (!buildTerrainBridgeMesh(bridge, cpu)) return std::nullopt;
    TerrainBridgeMeshInspection output;
    output.vertices.reserve(cpu.vertices.size());
    for (const StaticMeshVertex& vertex : cpu.vertices) {
        output.vertices.push_back(vertex.position);
    }
    output.indices = std::move(cpu.indices);
    return output;
}

std::optional<TerrainBibMeshInspection>
inspectTerrainBibMesh(const TerrainBibRenderData& bib) {
    TerrainBibMeshCpu cpu;
    if (!buildTerrainBibMesh(bib, cpu)) return std::nullopt;
    TerrainBibMeshInspection output;
    output.vertices.reserve(cpu.vertices.size());
    for (const StaticMeshVertex& vertex : cpu.vertices) {
        output.vertices.push_back(vertex.position);
    }
    output.indices = std::move(cpu.indices);
    return output;
}

} // namespace engine::render
