#pragma once

#include "core/container/container_types.h"
#include "core/platform/LaunchContentContract.h"

#include "system/SubsystemInterface.h"
#include "GlobalData.h"
namespace fs = std::filesystem;

// FileSystemSubsystem: mounts locale loose files, product BIG archives, and
// the optional launcher/direct-start Mod archive group.
// Replaces the inline VFS setup that was in main.cpp.
class FileSystemSubsystem : public SubsystemInterface {
public:
    FileSystemSubsystem();
    explicit FileSystemSubsystem(const engine::LaunchContentContract& contentContract);
    ~FileSystemSubsystem() override;

    void init() override;
    void reset() override;
    void shutdown() override;

    config::GlobalData& getGlobalData() {
        return *config::TheWritableGlobalData;
    }
    const config::GlobalData& getGlobalData() const {
        return config::TheGlobalData;
    }
    [[nodiscard]] const engine::LaunchContentContract* contentContract() const noexcept {
        return m_hasContentContract ? &m_contentContract : nullptr;
    }

private:
    container::Vector<container::String> m_mountedBigFiles;
    engine::LaunchContentContract m_contentContract;
    bool m_hasContentContract = false;
};
