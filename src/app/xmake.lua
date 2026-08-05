-- app：程序入口（main）

target("generals_td")
    set_kind("binary")
    set_group("app")
    set_pcxxheader("$(projectdir)/src/pch.h")
    add_files("**.cpp")
    -- 二进制只依赖两个顶层游戏组合根：单局运行时与可选网络传输。
    -- 其余内部静态库必须由 shard 的真实 add_deps 单向传递，禁止再靠
    -- executable 把所有 .lib 硬列一遍来掩盖链接环。
    add_deps("game_session", "game_network", "engine", "spdlog", "mimalloc")
    add_packages("sdl3")
    add_includedirs("$(projectdir)/src",
                    "$(projectdir)/src/app",
                    "$(projectdir)/src/app/host",
                    "$(projectdir)/src/engine",
                    "$(projectdir)/src/engine/gui",
                    "$(projectdir)/src/engine/gui/base",
                    "$(projectdir)/src/engine/gui/core",
                    "$(projectdir)/src/engine/gui/draw",
                    "$(projectdir)/src/engine/gui/widget",
                    "$(projectdir)/src/engine/renderer",
                    "$(projectdir)/src/engine/renderer/runtime",
                    "$(projectdir)/src/engine/renderer/d3d12/runtime",
                    "$(projectdir)/src/engine/font",
                    "$(projectdir)/src/engine/input",
                    "$(projectdir)/src/engine/system",
                    "$(projectdir)/src/engine/texture",
                    {public = true})
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "dwmapi")
        -- Keep the ordinary main entry point and a console in every shipped
        -- configuration. Release still suppresses Debug/Trace diagnostics,
        -- but its operational Info/Warning/Error stream must remain visible
        -- and is also mirrored to generals.log.
        add_ldflags("/SUBSYSTEM:CONSOLE", {force = true})
    end
