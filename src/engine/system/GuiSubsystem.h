#pragma once

#include "system/SubsystemInterface.h"

// GuiSubsystem: initializes draw function registry and loads MappedImageCollection.
class GuiSubsystem : public SubsystemInterface {
public:
    GuiSubsystem();
    ~GuiSubsystem() override;

    void init() override;
    void reset() override;
    void shutdown() override;
};
