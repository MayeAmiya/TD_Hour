#pragma once

#include "core/container/container_types.h"
#include "core/platform/LaunchContentContract.h"

#include "game/base/GameSettings.h"
namespace engine {

struct LauncherSessionDescriptor {
    GameStartInfo startInfo;
    LaunchContentContract content;
};

class GameLaunchDescriptor {
public:
    static bool loadFromVfs(container::StringView ticket, GameStartInfo& info, container::String* error = nullptr);
    // Formal launcher bootstrap. descriptorPath must be absolute because this
    // method runs before VFS construction; the descriptor then supplies every
    // root used to build the authoritative mount stack.
    static bool loadFromBootstrapFile(container::StringView descriptorPath,
                                      LauncherSessionDescriptor& descriptor,
                                      container::String* error = nullptr);
    static container::String pathForTicket(container::StringView ticket);
    // Shared by the launcher outcome path so descriptor input and result
    // output use one traversal-safe ticket grammar.
    static bool isValidTicket(container::StringView ticket);

private:
    static constexpr size_t kMaximumTicketLength = 96;
};

} // namespace engine
