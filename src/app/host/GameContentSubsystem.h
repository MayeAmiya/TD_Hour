#pragma once

#include "system/SubsystemInterface.h"

namespace app {

// Initializes game-owned catalogs after the engine VFS has been mounted.
class GameContentSubsystem final : public SubsystemInterface {
public:
    GameContentSubsystem();
    ~GameContentSubsystem() override;

    void init() override;
    void shutdown() override;

private:
    bool m_initialized = false;
};

} // namespace app
