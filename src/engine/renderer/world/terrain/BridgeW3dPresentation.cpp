#include "engine/renderer/world/terrain/BridgeW3dPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

[[nodiscard]] char lowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool startsWithInsensitive(
    container::StringView value, container::StringView prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (size_t index = 0; index < prefix.size(); ++index) {
        if (lowerAscii(value[index]) != lowerAscii(prefix[index])) return false;
    }
    return true;
}

[[nodiscard]] bool equalInsensitive(
    container::StringView left, container::StringView right) noexcept {
    return left.size() == right.size() && startsWithInsensitive(left, right);
}

struct MeshSelection final {
    container::String name;
    container::Vector<size_t> indices;
    float minimumX = std::numeric_limits<float>::max();
    float maximumX = -std::numeric_limits<float>::max();
    float minimumY = std::numeric_limits<float>::max();
    float maximumY = -std::numeric_limits<float>::max();

    [[nodiscard]] bool valid() const noexcept {
        return !name.empty() && !indices.empty() &&
            std::isfinite(minimumX) && std::isfinite(maximumX) &&
            std::isfinite(minimumY) && std::isfinite(maximumY) &&
            maximumX >= minimumX && maximumY >= minimumY;
    }
};

[[nodiscard]] MeshSelection selectLastPrefixedMesh(
    const CpuStaticModel& model, container::StringView prefix) {
    MeshSelection output;
    for (const CpuStaticMesh& mesh : model.meshes) {
        if (startsWithInsensitive(mesh.name, prefix)) output.name = mesh.name;
    }
    if (output.name.empty()) return output;

    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const CpuStaticMesh& mesh = model.meshes[meshIndex];
        // Multiple material passes retain the same logical mesh name.  Keep
        // them together exactly as one legacy MeshClass render object.
        if (!equalInsensitive(mesh.name, output.name)) continue;
        output.indices.push_back(meshIndex);
        for (const StaticMeshVertex& vertex : mesh.vertices) {
            const math::vec3 point =
                mesh.localTransform.transform_point(vertex.position);
            output.minimumX = std::min(output.minimumX, point.x());
            output.maximumX = std::max(output.maximumX, point.x());
            output.minimumY = std::min(output.minimumY, point.y());
            output.maximumY = std::max(output.maximumY, point.y());
        }
    }
    return output;
}

[[nodiscard]] container::Vector<RenderSubObjectVisibility>
sectionVisibility(const CpuStaticModel& model,
                  container::StringView selectedName) {
    container::Vector<RenderSubObjectVisibility> output;
    output.reserve(model.meshes.size() + 1u);
    container::Vector<container::String> seen;
    seen.reserve(model.meshes.size());
    for (const CpuStaticMesh& mesh : model.meshes) {
        const bool duplicate = std::any_of(
            seen.begin(), seen.end(), [&mesh](container::StringView name) {
                return equalInsensitive(name, mesh.name);
            });
        if (duplicate) continue;
        seen.push_back(mesh.name);
        output.push_back({.name = mesh.name, .visible = false});
    }
    // Overrides are last-writer-wins.  Re-enable the selected render object
    // after hiding every root sibling; hierarchy children follow it.
    output.push_back({.name = container::String(selectedName), .visible = true});
    return output;
}

[[nodiscard]] size_t damageIndex(TerrainBridgeDamageState state) noexcept {
    return std::min<size_t>(static_cast<size_t>(state), 3u);
}

} // namespace

std::optional<BridgeW3dPresentationPlan> buildBridgeW3dPresentationPlan(
    const CpuStaticModel& model,
    const TerrainBridgeRenderData& bridge) {
    const size_t stateIndex = damageIndex(bridge.damageState);
    const container::String& modelName = bridge.modelNames[stateIndex];
    if (modelName.empty() || model.meshes.empty() ||
        !std::isfinite(bridge.scale) || bridge.scale <= 0.0f) {
        return std::nullopt;
    }
    const math::vec3 delta = bridge.end - bridge.start;
    const float desiredLength = delta.length();
    const float planarLength = std::hypot(delta.x(), delta.y());
    if (!std::isfinite(desiredLength) || !std::isfinite(planarLength) ||
        desiredLength <= math::EPSILON || planarLength <= math::EPSILON) {
        return std::nullopt;
    }

    const container::String leftPrefix = modelName + ".BRIDGE_LEFT";
    const container::String spanPrefix = modelName + ".BRIDGE_SPAN";
    const container::String rightPrefix = modelName + ".BRIDGE_RIGHT";
    const MeshSelection left = selectLastPrefixedMesh(model, leftPrefix);
    if (!left.valid()) return std::nullopt;
    const MeshSelection span = selectLastPrefixedMesh(model, spanPrefix);
    const MeshSelection right = selectLastPrefixedMesh(model, rightPrefix);

    BridgeW3dPresentationPlan output;
    output.sourceMinimumX = left.minimumX;
    output.sourceMaximumX = left.maximumX;
    output.sourceMinimumY = left.minimumY;
    output.sourceMaximumY = left.maximumY;
    output.bridgeWidth = (left.maximumY - left.minimumY) * bridge.scale;

    const bool hasSections = span.valid() && right.valid();
    const float modelRightMaximum = hasSections
        ? right.maximumX : left.maximumX;
    output.sourceLength = std::max(1.0f,
        modelRightMaximum - left.minimumX);
    output.spanLength = hasSections
        ? right.minimumX - left.maximumX : 0.0f;
    output.sectional = hasSections;

    uint32_t spanCount = 0;
    if (hasSections) {
        spanCount = 1u;
        const float allowableError = output.sourceLength * 0.05f;
        const bool aligned = left.maximumX <= span.minimumX + allowableError &&
            right.minimumX >= span.maximumX - allowableError &&
            std::isfinite(output.spanLength) &&
            output.spanLength > math::EPSILON;
        if (aligned) {
            const float spannable = desiredLength -
                (output.sourceLength - output.spanLength);
            const float authored = std::floor(
                (spannable + output.spanLength * 0.5f) /
                output.spanLength);
            spanCount = authored > 0.0f
                ? static_cast<uint32_t>(std::min(authored, 1024.0f)) : 0u;
        }
    }
    output.spanCount = spanCount;

    const float bridgeLength = hasSections
        ? output.sourceLength +
            (static_cast<float>(spanCount) - 1.0f) * output.spanLength
        : output.sourceLength;
    if (!std::isfinite(bridgeLength) ||
        std::abs(bridgeLength) <= math::EPSILON) return std::nullopt;

    const math::vec3 xAxis = delta / bridgeLength;
    const math::vec3 yAxis{
        -delta.y() / planarLength * bridge.scale,
         delta.x() / planarLength * bridge.scale,
         0.0f};
    const float slope = delta.z() / desiredLength;
    const float horizontal = std::sqrt(std::max(0.0f, 1.0f - slope * slope));
    // Preserve W3DBridge's authored local-Z mapping, including its historical
    // world-X slope term, rather than silently replacing asset parity with a
    // newly orthogonalized basis.
    const math::vec3 zAxis{
        -slope * bridge.scale, 0.0f, horizontal * bridge.scale};
    const float xOffset = -left.minimumX;

    const auto append = [&](BridgeW3dSectionKind kind,
                            const MeshSelection& selection,
                            float localOffset) {
        const math::vec3 position = bridge.start + xAxis * localOffset;
        output.sections.push_back({
            .kind = kind,
            .worldTransform = math::transform::from_axes(
                xAxis, yAxis, zAxis, position),
            .subObjectVisibility = sectionVisibility(model, selection.name),
        });
    };

    append(BridgeW3dSectionKind::Left, left, xOffset);
    if (hasSections) {
        for (uint32_t index = 0; index < spanCount; ++index) {
            append(BridgeW3dSectionKind::Span, span,
                   xOffset + static_cast<float>(index) * output.spanLength);
        }
        append(BridgeW3dSectionKind::Right, right,
               xOffset + (static_cast<float>(spanCount) - 1.0f) *
                   output.spanLength);
    }
    return output.sections.empty() ? std::nullopt
                                   : std::optional{std::move(output)};
}

} // namespace engine::render
