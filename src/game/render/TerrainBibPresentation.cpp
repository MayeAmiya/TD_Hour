#include "game/render/TerrainBibPresentation.h"

#include <cmath>

namespace engine::render {

std::optional<TerrainBibRenderData> buildTerrainBibFootprint(
    const TerrainBibFootprintInput& input) noexcept {
    const float values[] = {
        input.position.x(), input.position.y(), input.position.z(),
        input.yawRadians, input.geometryMajorRadius,
        input.geometryMinorRadius, input.factoryExitWidth,
        input.factoryExtraBibWidth, input.additionalExtraWidth,
    };
    for (const float value : values) {
        if (!std::isfinite(value)) return std::nullopt;
    }
    if (input.geometryMajorRadius < 0.0f ||
        input.geometryMinorRadius < 0.0f) {
        return std::nullopt;
    }

    const float sizeX = input.geometryMajorRadius;
    const float sizeY = input.geometryIsBox
        ? input.geometryMinorRadius : input.geometryMajorRadius;
    const float extraWidth =
        input.factoryExtraBibWidth + input.additionalExtraWidth;
    const float minimumX = -sizeX - extraWidth;
    const float maximumX = sizeX + input.factoryExitWidth + extraWidth;
    const float minimumY = -sizeY - extraWidth;
    const float maximumY = sizeY + extraWidth;
    if (!std::isfinite(minimumX) || !std::isfinite(maximumX) ||
        !std::isfinite(minimumY) || !std::isfinite(maximumY) ||
        maximumX < minimumX || maximumY < minimumY) {
        return std::nullopt;
    }

    const float cosine = std::cos(input.yawRadians);
    const float sine = std::sin(input.yawRadians);
    const auto worldCorner = [&](float localX, float localY) {
        return RenderVector{
            input.position.x() + cosine * localX - sine * localY,
            input.position.y() + sine * localX + cosine * localY,
            input.position.z(),
        };
    };

    TerrainBibRenderData output;
    output.ownerObjectId = input.ownerIdentity;
    output.kind = input.kind;
    output.corners = {{
        worldCorner(minimumX, minimumY),
        worldCorner(maximumX, minimumY),
        worldCorner(maximumX, maximumY),
        worldCorner(minimumX, maximumY),
    }};
    output.tint = input.tint != TerrainBibTint::Default
        ? input.tint
        : input.highlighted ? TerrainBibTint::Red
                            : TerrainBibTint::Default;
    output.textureName = output.tint == TerrainBibTint::Default &&
            input.highlighted
        ? "TBRedBib.tga" : "TBBib.tga";
    output.red = output.tint == TerrainBibTint::Red;
    output.receivesVisibility = input.receivesVisibility;
    return output;
}

} // namespace engine::render
