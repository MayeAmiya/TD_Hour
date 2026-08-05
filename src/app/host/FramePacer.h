#pragma once

#include <chrono>

namespace app {
namespace runtime {
struct GameUiProjection;
}

class FramePacer final {
public:
    FramePacer();

    [[nodiscard]] float beginFrame();
    void pace(const runtime::GameUiProjection& projection) const;

private:
    std::chrono::steady_clock::time_point m_previousFrameStart;
    std::chrono::steady_clock::time_point m_frameStart;
};

} // namespace app
