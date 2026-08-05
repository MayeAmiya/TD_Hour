#include "engine/renderer/runtime/WorldAssetResidency.h"

#include "core/io/VFS.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <memory>
#include <utility>

namespace engine::render {

WorldAssetResidency::WorldAssetResidency(d3d12::D3D12Device& device)
    : textures(std::make_shared<WorldTextureCache>(device)),
      assets(io::VFS::instance()),
      animations(io::VFS::instance()) {
    assets.setRetireFunction([](
        container::SharedPtr<const W3dGpuModel> model) {
        const auto constD3dModel =
            std::dynamic_pointer_cast<const D3D12W3dModel>(
                std::move(model));
        if (constD3dModel) {
            std::const_pointer_cast<D3D12W3dModel>(constD3dModel)->retire();
        }
    });
}

} // namespace engine::render
