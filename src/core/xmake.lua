-- core：基础能力层

target("core")
    set_kind("static")
    set_group("core")
    set_pcxxheader("$(projectdir)/src/pch.h")
    add_includedirs("..",
                    ".",
                    "constants",
                    "math",
                    "math/base",
                    "math/wwmath",
                    "math/wwmath/base",
                    "math/fixed",
                    "container",
                    "debug",
                    "ecs",
                    "platform",
                    "compression",
                    "compression/runtime",
                    "io",
                    "config",
                    "localization",
                    "data/ini",
                    "data/datachunk",
                    "data/w3d",
                    "data/dds",
                    "data/tga",
                    "system",
                    {public = true})
    add_files("math/**/*.cpp",
              "math/wwmath/base/*.cpp",
              "math/fixed/*.cpp",
              "debug/*.cpp",
              "compression/runtime/*.cpp",
              "compression/**/*.cpp",
              "io/*.cpp",
              "config/*.cpp",
              "platform/*.cpp",
              "localization/*.cpp",
              "data/**/*.cpp",
              "system/*.cpp")
    add_deps("unordered_dense", "DirectXMath", "vectorclass",
             "tracy", "spdlog", "taskflow", "entt")
    add_packages("zlib")

    if is_mode("debug") or is_mode("releasedbg") then
        add_defines("TD_DEBUG=1")
    end

    add_defines("_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
