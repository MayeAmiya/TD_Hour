-- GeneralsTD xmake 项目配置
-- Win x64 only · C++20 · SDL3

set_project("GeneralsTD")
set_version("0.1.0")
set_languages("c++20")
-- All third-party package sources are vendored below third_party.  Keep xmake
-- out of public package repositories during configure and build.
set_policy("network.mode", "private")
-- Never substitute vendored sources with downloadable xmake binary artifacts.
set_policy("package.precompiled", false)
set_policy("package.cmake_generator.ninja", false)

option("build_tools")
    set_default(false)
    set_showmenu(true)
    set_description("Include optional probe and developer-tool targets")
option_end()

-- Project code must use the aliases in hash_containers.h so the selected
-- hash-table implementation and its policy parameters stay centralized.
local function check_container_policy(xmake_io)
    local projectdir = os.projectdir()
    local hash_aliasfile = path.normalize(path.join(
        projectdir, "src/core/container/hash_containers.h"))
    local standard_aliasfile = path.normalize(path.join(
        projectdir, "src/core/container/container_types.h"))
    local source_patterns = {
        "src/**.h",
        "src/**.hpp",
        "src/**.cpp",
        "src/**.cc",
    }
    local forbidden_std_tokens = {
        "std::unordered_map",
        "std::unordered_set",
        "<unordered_map>",
        "<unordered_set>",
    }
    local forbidden_standard_type_tokens = {
        "std::vector",
        "std::basic_string",
        "std::string",
        "std::wstring",
        "std::u8string",
        "std::u16string",
        "std::u32string",
        "std::string_view",
        "std::wstring_view",
        "std::u8string_view",
        "std::u16string_view",
        "std::u32string_view",
        "std::map",
        "std::set",
        "std::array",
        "std::span",
        "std::unique_ptr",
        "std::shared_ptr",
        "std::weak_ptr",
        "std::deque",
        "std::list",
        "std::queue",
        "std::stack",
        "std::priority_queue",
        "<vector>",
        "<string>",
        "<string_view>",
        "<memory>",
        "<array>",
        "<span>",
        "<map>",
        "<set>",
        "<deque>",
        "<list>",
        "<queue>",
        "<stack>",
    }
    local forbidden_direct_tokens = {
        "ankerl::unordered_dense::map",
        "ankerl::unordered_dense::set",
    }
    local forbidden_custom_type_tokens = {
        "container::dict",
        "container::dict_value",
        "container::dict_entry",
        "container::string_id",
        "container::string_id_type",
        "container::string_id_generator",
        "container::static_string_id",
        "container::intrusive_list",
        "container::intrusive_list_node",
        "container::intrusive_hash_set",
        "container::intrusive_hash_set_node",
        "container::sparse_set",
        "container::ordered_id_set",
        "container::ring_buffer",
        "container::bit_flags",
    }

    for _, source_pattern in ipairs(source_patterns) do
        for _, sourcefile in ipairs(os.files(path.join(projectdir, source_pattern))) do
            local contents = xmake_io.readfile(sourcefile)
            for _, token in ipairs(forbidden_std_tokens) do
                if contents:find(token, 1, true) then
                    raise("hash container policy violation in %s: forbidden token '%s'",
                          path.relative(sourcefile, projectdir), token)
                end
            end
            if path.normalize(sourcefile) ~= standard_aliasfile then
                for _, token in ipairs(forbidden_standard_type_tokens) do
                    local search_from = 1
                    while true do
                        local first, last = contents:find(token, search_from, true)
                        if not first then
                            break
                        end
                        local following = contents:sub(last + 1, last + 1)
                        if following == "" or not following:match("[%w_]") then
                            raise("container type policy violation in %s: use a container alias instead of '%s'",
                                  path.relative(sourcefile, projectdir), token)
                        end
                        search_from = last + 1
                    end
                end
            end
            if path.normalize(sourcefile) ~= hash_aliasfile then
                for _, token in ipairs(forbidden_direct_tokens) do
                    if contents:find(token, 1, true) then
                        raise("hash container policy violation in %s: use container aliases instead of '%s'",
                              path.relative(sourcefile, projectdir), token)
                    end
                end
            end
            for _, token in ipairs(forbidden_custom_type_tokens) do
                if contents:find(token, 1, true) then
                    raise("custom container policy violation in %s: use the public container alias instead of '%s'",
                          path.relative(sourcefile, projectdir), token)
                end
            end
        end
    end
end

local container_policy_checked = false
rule("check.container_policy")
    on_load(function ()
        if container_policy_checked then
            return
        end
        check_container_policy(io)
        container_policy_checked = true
    end)
rule_end()

-- Keep the policy audit available for an explicit maintenance pass, but do
-- not make every normal/game build scan and reject the whole source tree.
-- The project is being migrated incrementally and gameplay work must remain
-- buildable while older modules are converted.
add_rules("mode.debug", "mode.release")

rule("mode.internal")
    on_load(function (target)
        if not is_mode("internal") then
            return
        end
        target:set("optimize", "fastest")
        target:set("symbols", "hidden")
        target:add("defines", "NDEBUG", "_INTERNAL")
    end)
rule_end()

if is_plat("windows") then
    add_defines("WIN32", "_WINDOWS", "_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "TD_LOG")
    add_cxflags("/W3", "/utf-8", "/Zc:__cplusplus", "/Zc:preprocessor")
    set_runtimes("MD")
    set_arch("x64")
end

if is_mode("debug") then
    add_defines("TD_DEBUG")
    if is_plat("windows") then
        -- Several large presentation/ECS translation units exceed the
        -- ordinary MSVC COFF section count once Debug COMDATs are enabled.
        -- Keep this strictly in Debug; it does not alter code semantics.
        add_cxflags("/bigobj")
    end
end
-- Release is the single production configuration and always uses the full
-- production optimization set; do not maintain a second shipping mode.
if is_mode("release") then
    set_optimize("fastest")
    set_symbols("hidden")
    add_defines("NDEBUG")
    if is_plat("windows") then
        add_cxflags(
            "/Ob3",           -- 更激进的内联
            "/GL",            -- 全程序优化
            "/arch:AVX2",     -- AVX2 指令集
            "/Gw"             -- 全局数据优化
        )
        add_ldflags("/LTCG")
        add_shflags("/LTCG")
    end
end
if is_mode("internal") then add_defines("TD_DEBUG", "_INTERNAL") end

set_targetdir("$(projectdir)/Bin/$(mode)")
set_objectdir("$(projectdir)/build/objs/$(mode)/$(target)")

-------------------------------------------------------------------------------
-- 第三方库：cmake 构建
-------------------------------------------------------------------------------

package("sdl3")
    set_kind("library")
    set_sourcedir("$(projectdir)/third_party/SDL3")
    on_install(function (package)
        import("package.tools.cmake").install(package, {
            "-DSDL_STATIC=ON", "-DSDL_SHARED=OFF",
            "-DSDL_TEST=OFF", "-DSDL_TESTS=OFF",
            "-DSDL_VIDEO=ON", "-DSDL_RENDER=ON",
            "-DSDL_RENDER_D3D12=ON", "-DSDL_RENDER_VULKAN=ON",
            "-DSDL_AUDIO=OFF", "-DSDL_CAMERA=OFF",
            "-DSDL_JOYSTICK=OFF", "-DSDL_HAPTIC=OFF",
            "-DSDL_SENSOR=OFF", "-DSDL_HIDAPI=OFF",
            "-DSDL_MAIN_HANDLED=ON",
        }, {cmake_generator = "Visual Studio 18 2026"})
    end)
    on_fetch(function (package)
        local install_dir = package:installdir()
        if not os.isfile(path.join(install_dir, "include", "SDL3", "SDL.h")) or
           not os.isfile(path.join(install_dir, "lib", "SDL3-static.lib")) then
            return
        end
        return {
            linkdirs = path.join(install_dir, "lib"),
            links = {"SDL3-static"},
            includedirs = {path.join(install_dir, "include", "SDL3"),
                           path.join(install_dir, "include")},
            syslinks = {"user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
                         "setupapi", "shlwapi", "cfgmgr32", "dxgi", "comctl32",
                         "dinput8", "dxguid", "version", "msimg32",
                         "advapi32", "shell32", "hid", "d3d12", "d3dcompiler"},
            defines = {"SDL_STATIC", "SDL_MAIN_HANDLED"},
        }
    end)
package_end()

package("freetype")
    set_kind("library")
    set_sourcedir("$(projectdir)/third_party/freetype")
    on_install(function (package)
        import("package.tools.cmake").install(package, {
            "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON",
            "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON",
            "-DFT_DISABLE_ZLIB=ON", "-DBUILD_SHARED_LIBS=OFF",
        }, {cmake_generator = "Visual Studio 18 2026"})
    end)
    on_fetch(function (package)
        local install_dir = package:installdir()
        if not os.isfile(path.join(install_dir, "include", "freetype2", "ft2build.h")) or
           not os.isfile(path.join(install_dir, "lib", "freetype.lib")) then
            return
        end
        return {
            linkdirs = path.join(install_dir, "lib"),
            links = {"freetype"},
            includedirs = {path.join(install_dir, "include"),
                           path.join(install_dir, "include", "freetype2"),
                           path.join(install_dir, "include", "freetype2", "freetype")},
        }
    end)
package_end()

package("zlib")
    set_kind("library")
    set_sourcedir("$(projectdir)/third_party/zlib")
    on_install(function (package)
        import("package.tools.cmake").install(package, {
            "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON",
            "-DZLIB_BUILD_TESTING=OFF",
        }, {cmake_generator = "Visual Studio 18 2026"})
    end)
    on_fetch(function (package)
        local install_dir = package:installdir()
        if not os.isfile(path.join(install_dir, "include", "zlib.h")) or
           not os.isfile(path.join(install_dir, "lib", "libzs.lib")) then
            return
        end
        return {
            linkdirs = path.join(install_dir, "lib"),
            links = {"libzs"},
            includedirs = {path.join(install_dir, "include")},
        }
    end)
package_end()

add_requires("sdl3", "freetype", "zlib")

-------------------------------------------------------------------------------
-- 第三方库：header-only / 简单静态库
-------------------------------------------------------------------------------

target("unordered_dense")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/unordered_dense/include", {public = true})

target("miniaudio")
    set_kind("static")
    set_group("third_party")
    add_files("third_party/miniaudio/miniaudio.c")
    add_includedirs("third_party/miniaudio", {public = true})

-- Process-wide C++ allocator used by the shipping executable in every build
-- mode.  mimalloc's MSVC build compiles the amalgamated C source as C++ so it
-- can use the platform's modern atomics.  MI_MALLOC_OVERRIDE is retained to
-- match the upstream static target; with our /MD runtime it deliberately does
-- not replace the CRT malloc/free entry points.  The unique new/delete
-- override translation unit lives in src/app/MimallocOverride.cpp.
target("mimalloc")
    set_kind("static")
    set_group("third_party")
    add_files("third_party/mimalloc/src/static.c", {sourcekind = "cxx"})
    add_includedirs("third_party/mimalloc/include", {public = true})
    add_defines("MI_STATIC_LIB", "MI_MALLOC_OVERRIDE")
    if is_plat("windows") then
        add_syslinks("psapi", "shell32", "user32", "advapi32", "bcrypt")
    end

target("DirectXMath")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/DirectXMath/Inc", {public = true})

target("taskflow")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/taskflow", {public = true})

target("entt")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/entt/single_include", {public = true})

target("fmt")
    set_kind("static")
    set_group("third_party")
    add_files("third_party/fmt/src/format.cc")
    add_includedirs("third_party/fmt/include", {public = true})

target("spdlog")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/spdlog/include", {public = true})
    add_deps("fmt")
    add_defines("SPDLOG_HEADER_ONLY", "SPDLOG_FMT_EXTERNAL")

target("vectorclass")
    set_kind("headeronly")
    set_group("third_party")
    add_includedirs("third_party/vectorclass", {public = true})

target("enet")
    set_kind("static")
    set_group("third_party")
    add_files("third_party/enet/*.c")
    add_includedirs("third_party/enet/include", {public = true})
    add_defines("ENET_DLL_EXPORTS=0")

target("tracy")
    set_kind("static")
    set_group("third_party")
    add_files("third_party/tracy/public/TracyClient.cpp")
    add_includedirs("third_party/tracy/public", {public = true})
    add_syslinks("dbghelp", "ws2_32", "iphlpapi", "shlwapi")

-------------------------------------------------------------------------------
-- 项目模块
-------------------------------------------------------------------------------
includes("src/core")
includes("src/presentation")
includes("src/game")
includes("src/engine")
includes("src/app")
if has_config("build_tools") then
    includes("src/tools")
end
