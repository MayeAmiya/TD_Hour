#include "ApplicationHost.h"

#include "CommandLine.h"
#include "debug/debug.h"
#include "core/constants/Paths.h"
#include "core/constants/Strings.h"
#include "core/platform/runtime_threads.h"

#include <Windows.h>

namespace {

LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* const address = ep->ExceptionRecord->ExceptionAddress;
    TD_LOG_ERROR("[CRASH] SEH exception 0x{:08X} at {:p}", code, address);
    debug::log::flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int main(int argc, char* argv[]) {
    const platform::runtime::ThreadRoleScope mainThreadRole(
        platform::runtime::ThreadRole::Main);
    const debug::log_scope logLifetime;
    SetUnhandledExceptionFilter(crashHandler);
    TD_LOG_INFO(LOG_BANNER.data());
    engine::CommandLine::instance().parse(argc, argv);

    app::ApplicationHost application;
    return application.run();
}
