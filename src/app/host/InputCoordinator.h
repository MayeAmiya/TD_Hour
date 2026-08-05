#pragma once

#include <memory>

class InGameGuiSubsystem;
class RendererSubsystem;

namespace engine {
class TextureManager;
}

namespace app {

class PresentationCoordinator;
namespace runtime {
class GameLogicIntentMailbox;
struct GameUiProjection;
}

class InputCoordinator final {
public:
    InputCoordinator(RendererSubsystem& renderer,
                     InGameGuiSubsystem& inGameGui,
                     engine::TextureManager& textureManager,
                     PresentationCoordinator& presentation,
                     runtime::GameLogicIntentMailbox& logicIntents);
    ~InputCoordinator();

    InputCoordinator(const InputCoordinator&) = delete;
    InputCoordinator& operator=(const InputCoordinator&) = delete;

    // Returns false only when the global GUI quit request requires the rest of
    // this frame to be skipped. SDL quit still completes the current frame.
    bool processFrame(float presentationDeltaSeconds, bool& running);
    void setGameProjection(const runtime::GameUiProjection& projection);
    void updateAfterLogicTick(int frameCount);
    void synchronizePresentationState();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace app
