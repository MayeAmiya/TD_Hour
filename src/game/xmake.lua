-- game：权威游戏领域。
--
-- 每个静态 shard 只声明真实的单向下层依赖；`game` 仅是人工聚合目标。
-- generals_td 不再硬列全部内部库，任何新反向链接边都会在正常生产链接时
-- 直接暴露，而不会被 MSVC 的静态库排列顺序掩盖。

-- Every game shard needs core, but presentation is not a universal lower
-- layer. Shards that actually compile presentation-facing camera/content/
-- snapshot code declare the narrow presentation target below.
local game_common_deps = {"core"}

local function configure_game_shard(name, files)
    target(name)
        set_kind("static")
        set_group("game")
        set_pcxxheader("$(projectdir)/src/pch.h")
        add_includedirs("..", {public = true})
        add_includedirs(".",
                        "base",
                        "content/runtime",
                        "data/base",
                        "fx/runtime",
                        "navigation/runtime",
                        "object/definition",
                        "object/ai/definition",
                        "session/core")
        add_files(table.unpack(files))
        add_deps(table.unpack(game_common_deps))
        if is_mode("release") then
            -- Authoritative simulation and authored-to-fixed projection must
            -- not inherit renderer-style reassociation or reciprocal math.
            add_cxflags("/fp:precise", {force = true})
        end
    target_end()
end

-- 基础值协议、数据目录、玩家、命令与选择；不拥有网络或内容装载器。
configure_game_shard("game_base", {
    "base/*.cpp", -- Includes MapSourceBlob.cpp and its shared map-start factory.
    "data/base/*.cpp",
    "audio/*.cpp",
    "data/presentation/*.cpp",
    "player/*.cpp",
    "command/*.cpp",
    "selection/*.cpp",
    "text/*.cpp",
})
target("game_base")
    remove_files("base/GameTacticalCamera.cpp")
    add_deps("presentation_runtime")

-- 联机传输是唯一允许依赖 ENet 的游戏分片。命令编解码和 GameSettings
-- 仍归 base；network 单向依赖 base，其他离线/模拟目标不再继承 ENet。
configure_game_shard("game_network", {
    "network/*.cpp",
})
target("game_network")
    add_deps("game_base", "enet")

-- 地图、场景和导航权威世界服务。
configure_game_shard("game_world", {
    "base/GameTacticalCamera.cpp",
    "terrain/*.cpp",
    "scenario/runtime/*.cpp",
    "navigation/**/*.cpp",
})
target("game_world")
    add_deps("game_base", "game_ai_contracts", "game_scenario_source",
             "presentation_runtime")

-- Thing/Object 编译、创建、武器模板与空间索引；不含逐帧模拟和 AI。
configure_game_shard("game_object", {
    "content/runtime/*.cpp",
    "object/definition/*.cpp",
    "object/contracts/*.cpp",
    "object/runtime/*.cpp",
    "object/creation/*.cpp",
    "object/weapon/*.cpp",
    "object/spatial/*.cpp",
})
target("game_object")
    add_deps("game_base", "game_ai", "game_world", "presentation_content")

-- 对象玩法系统。该分片仍较大，但已经与模板编译、AI、脚本和 Session 分离。
configure_game_shard("game_simulation", {
    "object/simulation/**/*.cpp",
})
target("game_simulation")
    add_deps("game_base", "game_ai", "game_object", "game_object_plan",
             "game_world", "presentation_content")

-- 加载期 Object*Plan 编译器。仅依赖 authored 数据、共享契约与只读目录；
-- 不允许反向依赖逐帧模拟实现。
configure_game_shard("game_object_plan", {
    "object/plan/**/*.cpp",
})
target("game_object_plan")
    add_deps("game_base", "game_ai", "game_object", "game_world",
             "presentation_content")

-- Thing authoring/W3D 到对象配方的编译阶段。所有 Object*Plan 编译器
-- 都是它的下层服务；该目标不进入逐帧模拟热路径。
configure_game_shard("game_object_compiler", {
    "content/compiler/ThingFactoryLoader.cpp",
    "content/compiler/W3dPristineBoneCatalogCompiler.cpp",
})
target("game_object_compiler")
    add_deps("game_base", "game_object", "game_object_plan", "game_ai")

-- 进程级 INI/VFS 装载器及其只读注册表 facade。
configure_game_shard("game_content_loader", {
    "ini/GameDataLoader.cpp",
    "content/loader/*.cpp",
})
target("game_content_loader")
    add_deps("game_base", "game_object", "game_object_compiler",
             "game_presentation", "presentation_runtime")

-- 已装载目录到单局不可变 GameContentSnapshot 的冻结和交叉校验。
configure_game_shard("game_content_snapshot_compiler", {
    "content/compiler/GameContentSnapshotCompiler.cpp",
})
target("game_content_snapshot_compiler")
    add_deps("game_base", "game_object", "game_object_plan",
             "game_content_loader", "game_object_compiler",
             "game_presentation", "presentation_runtime")

target("game_content_compiler")
    set_kind("phony")
    set_group("game")
    add_deps("game_object_compiler", "game_content_loader",
             "game_content_snapshot_compiler")

-- 对象级扁平 SoA 状态机、服务协议与生产 admission。
target("game_ai_contracts")
    set_kind("headeronly")
    set_group("game")
    add_includedirs("..", {public = true})
    add_deps("game_base")

configure_game_shard("game_ai", {
    "ai/*.cpp",
    "object/ai/definition/*.cpp",
    "object/ai/**/*.cpp",
})
target("game_ai")
    add_deps("game_base", "game_ai_contracts")

-- CkMp 的无损、不可变源模型与解析器；Scenario compiler 和脚本语义
-- compiler 都只向下依赖它。
configure_game_shard("game_scenario_source", {
    "scenario/source/LegacyMapScriptSource.cpp",
    "scenario/source/LegacySkirmishScriptSource.cpp",
})

-- 脚本确定性程序与运行时；不编译旧地图语法，也不实现本地 UI 状态。
configure_game_shard("game_script_runtime", {
    "script/bridge/*.cpp",
    "script/runtime/*.cpp",
})
target("game_script_runtime")
    add_deps("game_base")

-- 旧 SCB/地图脚本语法到不可变 ScriptProgram 的加载期编译器。
configure_game_shard("game_script_compiler", {
    "script/legacy/*.cpp",
})
target("game_script_compiler")
    add_deps("game_scenario_source", "game_script_runtime", "game_object")

-- 已确认脚本效果的客户端表现状态与只读消费者。
configure_game_shard("game_script_presentation", {
    "script/presentation/*.cpp",
})
target("game_script_presentation")
    add_deps("game_script_runtime", "game_base", "game_object")

target("game_script")
    set_kind("phony")
    set_group("game")
    add_deps("game_script_runtime", "game_script_compiler",
             "game_script_presentation", "game_scenario_source")

-- GameSession 组合根、confirmed-tick 各领域编排与外部域集成 adapter。
configure_game_shard("game_session", {
    "session/**/*.cpp",
})
target("game_session")
    -- Session is the composition root, but these are still real direct
    -- dependencies: its translation units include and call each domain rather
    -- than merely receiving them through another archive's link closure.
    add_deps("game_base", "game_ai", "game_world", "game_object",
             "game_object_plan", "game_simulation", "game_content_loader",
             "game_content_snapshot_compiler", "game_scenario_source",
             "game_script_compiler", "game_script_presentation",
             "game_script_runtime", "game_presentation", "presentation_runtime")

-- 逻辑侧渲染/FX/音频不可变提取，不包含 engine/D3D12 消费端。
configure_game_shard("game_presentation", {
    "render/*.cpp",
    "fx/runtime/*.cpp",
})
target("game_presentation")
    add_deps("game_base", "game_object", "game_world",
             "presentation_runtime")

target("game")
    set_kind("phony")
    set_group("game")
    add_deps(
        "game_base",
        "game_network",
        "game_world",
        "game_object",
        "game_simulation",
        "game_object_plan",
        "game_object_compiler",
        "game_content_loader",
        "game_content_snapshot_compiler",
        "game_ai",
        "game_scenario_source",
        "game_script_runtime",
        "game_script_compiler",
        "game_script_presentation",
        "game_session",
        "game_presentation")
