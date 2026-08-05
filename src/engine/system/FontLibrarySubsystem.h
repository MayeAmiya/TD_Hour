#pragma once

#include "system/SubsystemInterface.h"

// FontLibrarySubsystem: initializes FontRegistry and preloads common fonts.
class FontLibrarySubsystem : public SubsystemInterface {
public:
    FontLibrarySubsystem();
    ~FontLibrarySubsystem() override;

    void init() override;
    void reset() override;
    void shutdown() override;
};
