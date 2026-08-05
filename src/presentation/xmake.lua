-- presentation_contracts：game 与 engine 共享的纯值契约。
target("presentation_contracts")
    set_kind("headeronly")
    set_group("presentation")
    add_includedirs("..", ".", {public = true})
    add_deps("core")

-- authored 表现内容的加载期目录与配置投影。
target("presentation_content")
    set_kind("static")
    set_group("presentation")
    set_pcxxheader("$(projectdir)/src/pch.h")
    add_includedirs("..", ".", {public = true})
    add_files("fx/content/*.cpp",
              "render/RenderGameDataSettings.cpp")
    add_deps("core", "presentation_contracts")
    if is_mode("release") then
        add_cxflags("/fp:precise", {force = true})
    end

-- 与 renderer 后端无关的相机、FX 和快照纯算法。
target("presentation_runtime")
    set_kind("static")
    set_group("presentation")
    set_pcxxheader("$(projectdir)/src/pch.h")
    add_includedirs("..", ".", {public = true})
    add_files("camera/*.cpp",
              "fx/runtime/*.cpp",
              "render/*.cpp")
    remove_files("render/RenderGameDataSettings.cpp")
    add_deps("core", "presentation_contracts", "presentation_content")
    if is_mode("release") then
        add_cxflags("/fp:fast", {force = true})
    end

-- 兼容组合根；新 shard 应声明自己真实使用的下层目标。
target("presentation")
    set_kind("phony")
    set_group("presentation")
    add_deps("presentation_contracts", "presentation_content",
             "presentation_runtime")
