#pragma once

#include "system/SubsystemInterface.h"
// GameTextSubsystem: loads CSF string table for localization.
class GameTextSubsystem : public SubsystemInterface {
public:
    GameTextSubsystem();
    ~GameTextSubsystem() override;

    void init() override;
    void reset() override;
    void shutdown() override;

private:
    bool m_loaded = false;
};
