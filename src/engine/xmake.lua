-- engine：引擎服务层

local fxaa_shader_package_version = 1
local fxaa_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/fxaa.hlsl")
local fxaa_shader_source_hash = hash.sha256(fxaa_shader_source)
local projectile_trail_shader_package_version = 1
local projectile_trail_shader_source = path.join(
    os.projectdir(),
    "src/engine/renderer/d3d12/shaders/projectile_trail.hlsl")
local projectile_trail_shader_source_hash =
    hash.sha256(projectile_trail_shader_source)
local particle_shader_package_version = 1
local particle_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/particle.hlsl")
local particle_shader_source_hash = hash.sha256(particle_shader_source)
local ground_projector_shader_package_version = 2
local ground_projector_shader_source = path.join(
    os.projectdir(),
    "src/engine/renderer/d3d12/shaders/ground_projector.hlsl")
local ground_projector_shader_source_hash =
    hash.sha256(ground_projector_shader_source)
local typed_fx_world_shader_package_version = 1
local typed_fx_world_shader_source = path.join(
    os.projectdir(),
    "src/engine/renderer/d3d12/shaders/typed_fx_world.hlsl")
local typed_fx_world_shader_source_hash =
    hash.sha256(typed_fx_world_shader_source)
local ui_2d_shader_package_version = 1
local ui_2d_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/ui_2d.hlsl")
local ui_2d_shader_source_hash = hash.sha256(ui_2d_shader_source)
local world_main_shader_package_version = 1
local world_main_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/world_main.hlsl")
local world_main_shader_source_hash = hash.sha256(world_main_shader_source)
local world_directional_shadow_shader_package_version = 1
local world_directional_shadow_shader_source = path.join(
    os.projectdir(),
    "src/engine/renderer/d3d12/shaders/world_directional_shadow.hlsl")
local world_directional_shadow_shader_source_hash =
    hash.sha256(world_directional_shadow_shader_source)
local screen_fade_shader_package_version = 1
local screen_fade_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/screen_fade.hlsl")
local screen_fade_shader_source_hash = hash.sha256(screen_fade_shader_source)
local black_and_white_shader_package_version = 1
local black_and_white_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/black_and_white.hlsl")
local black_and_white_shader_source_hash =
    hash.sha256(black_and_white_shader_source)
local motion_blur_shader_package_version = 1
local motion_blur_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/motion_blur.hlsl")
local motion_blur_shader_source_hash = hash.sha256(motion_blur_shader_source)
-- Thousands digit is the shared GpuParticleContract version. Bump both when
-- the C++/HLSL layout changes so a stale compute package is rejected.
local particle_compute_shader_package_version = 3005
local particle_compute_shader_source = path.join(
    os.projectdir(), "src/engine/renderer/d3d12/shaders/particle_compute.hlsl")
local particle_compute_contract_source = path.join(
    os.projectdir(), "src/engine/fx/runtime/GpuParticleContract.hlsli")
local particle_compute_shader_source_hash =
    hash.sha256(particle_compute_shader_source)
local particle_gpu_billboard_shader_package_version = 3001
local particle_gpu_billboard_shader_source = path.join(
    os.projectdir(),
    "src/engine/renderer/d3d12/shaders/particle_gpu_billboard.hlsl")
local particle_gpu_billboard_shader_source_hash =
    hash.sha256(particle_gpu_billboard_shader_source)

local function build_stage_shader_package(
    target, depend, os, path, io, get_config, spec)
    local build_mode = get_config("mode") or "debug"
    local package_dir = path.join(target:targetdir(), "shaders")
    local manifest = path.join(package_dir, spec.name .. ".manifest")
    local depend_file = path.join(
        target:autogendir(), spec.name .. "_shader_package.d")
    local complete = os.isfile(manifest)
    local outputs = {}
    for index, stage in ipairs(spec.stages) do
        outputs[index] = path.join(package_dir, stage.file)
        complete = complete and os.isfile(outputs[index])
    end
    if not complete then os.tryrm(depend_file) end

    local dependency_files = {
        spec.source,
        path.join(os.projectdir(), "src/engine/xmake.lua"),
    }
    if spec.dependencies then
        for _, dependency in ipairs(spec.dependencies) do
            table.insert(dependency_files, dependency)
        end
    end

    depend.on_changed(function ()
        os.mkdir(package_dir)
        local temporary = {}
        for index, output in ipairs(outputs) do
            temporary[index] = output .. ".tmp"
            os.tryrm(temporary[index])
        end
        local manifest_temp = manifest .. ".tmp"
        os.tryrm(manifest_temp)

        local common_flags = {"/nologo", "/Ges", "/Zpr"}
        if build_mode == "debug" or build_mode == "internal" then
            table.insert(common_flags, "/Zi")
            table.insert(common_flags, "/Od")
        else
            table.insert(common_flags, "/O3")
        end
        for index, stage in ipairs(spec.stages) do
            os.vrunv("fxc", table.join(common_flags, {
                "/E", stage.entry,
                "/T", stage.profile,
                "/Fo", temporary[index],
                spec.source,
            }))
        end

        local debug_symbols =
            (build_mode == "debug" or build_mode == "internal") and
            "embedded" or "none"
        local contents = string.format(
            "format=GeneralsTDShaderPackage\n" ..
            "version=%d\n" ..
            "shader=%s\n" ..
            "source_sha256=%s\n",
            spec.version, spec.name, spec.source_hash)
        for _, stage in ipairs(spec.stages) do
            contents = contents .. string.format(
                "%s_file=%s\n%s_profile=%s\n",
                stage.key, stage.file, stage.key, stage.profile)
        end
        io.writefile(
            manifest_temp,
            contents .. "debug_symbols=" .. debug_symbols .. "\n")

        for index, output in ipairs(outputs) do
            os.tryrm(output)
            os.mv(temporary[index], output)
        end
        os.tryrm(manifest)
        os.mv(manifest_temp, manifest)
    end, {
        dependfile = depend_file,
        files = dependency_files,
        values = {
            spec.version,
            spec.source_hash,
            build_mode,
        },
    })
end

local function build_fxaa_shader_package(
    target, depend, os, path, io, get_config)
    local build_mode = get_config("mode") or "debug"
    local package_dir = path.join(target:targetdir(), "shaders")
    local vertex_blob = path.join(package_dir, "fxaa_vs.cso")
    local pixel_blob = path.join(package_dir, "fxaa_ps.cso")
    local manifest = path.join(package_dir, "fxaa.manifest")
    local depend_file = path.join(target:autogendir(), "fxaa_shader_package.d")

    -- A deleted/incomplete output must never be hidden by a still-valid
    -- dependency record.  Removing the record makes the package self-heal on
    -- the next Debug build.
    if not os.isfile(vertex_blob) or not os.isfile(pixel_blob) or
       not os.isfile(manifest) then
        os.tryrm(depend_file)
    end

    depend.on_changed(function ()
        os.mkdir(package_dir)
        local vertex_temp = vertex_blob .. ".tmp"
        local pixel_temp = pixel_blob .. ".tmp"
        local manifest_temp = manifest .. ".tmp"
        os.tryrm(vertex_temp)
        os.tryrm(pixel_temp)
        os.tryrm(manifest_temp)

        local common_flags = {"/nologo", "/Ges", "/Zpr"}
        if build_mode == "debug" or build_mode == "internal" then
            table.insert(common_flags, "/Zi")
            table.insert(common_flags, "/Od")
        else
            table.insert(common_flags, "/O3")
        end

        local function compile(entry, profile, output)
            local arguments = table.join(common_flags, {
                "/E", entry,
                "/T", profile,
                "/Fo", output,
                fxaa_shader_source,
            })
            os.vrunv("fxc", arguments)
        end

        compile("VSMain", "vs_5_0", vertex_temp)
        compile("PSMain", "ps_5_0", pixel_temp)

        local debug_symbols =
            (build_mode == "debug" or build_mode == "internal") and
            "embedded" or "none"
        io.writefile(manifest_temp, string.format(
            "format=GeneralsTDShaderPackage\n" ..
            "version=%d\n" ..
            "shader=fxaa\n" ..
            "source_sha256=%s\n" ..
            "vertex_file=fxaa_vs.cso\n" ..
            "vertex_profile=vs_5_0\n" ..
            "pixel_file=fxaa_ps.cso\n" ..
            "pixel_profile=ps_5_0\n" ..
            "debug_symbols=%s\n",
            fxaa_shader_package_version, fxaa_shader_source_hash,
            debug_symbols))

        -- Publish only complete artifacts; a failed compiler invocation leaves
        -- the previous valid package intact.
        os.tryrm(vertex_blob)
        os.tryrm(pixel_blob)
        os.tryrm(manifest)
        os.mv(vertex_temp, vertex_blob)
        os.mv(pixel_temp, pixel_blob)
        os.mv(manifest_temp, manifest)
    end, {
        dependfile = depend_file,
        files = {fxaa_shader_source, path.join(os.projectdir(), "src/engine/xmake.lua")},
        values = {
            fxaa_shader_package_version,
            fxaa_shader_source_hash,
            build_mode,
        },
    })
end

local function build_projectile_trail_shader_package(
    target, depend, os, path, io, get_config)
    local build_mode = get_config("mode") or "debug"
    local package_dir = path.join(target:targetdir(), "shaders")
    local vertex_blob = path.join(package_dir, "projectile_trail_vs.cso")
    local pixel_blob = path.join(package_dir, "projectile_trail_ps.cso")
    local manifest = path.join(package_dir, "projectile_trail.manifest")
    local depend_file = path.join(
        target:autogendir(), "projectile_trail_shader_package.d")

    if not os.isfile(vertex_blob) or not os.isfile(pixel_blob) or
       not os.isfile(manifest) then
        os.tryrm(depend_file)
    end

    depend.on_changed(function ()
        os.mkdir(package_dir)
        local vertex_temp = vertex_blob .. ".tmp"
        local pixel_temp = pixel_blob .. ".tmp"
        local manifest_temp = manifest .. ".tmp"
        os.tryrm(vertex_temp)
        os.tryrm(pixel_temp)
        os.tryrm(manifest_temp)

        local common_flags = {"/nologo", "/Ges", "/Zpr"}
        if build_mode == "debug" or build_mode == "internal" then
            table.insert(common_flags, "/Zi")
            table.insert(common_flags, "/Od")
        else
            table.insert(common_flags, "/O3")
        end
        local function compile(entry, profile, output)
            os.vrunv("fxc", table.join(common_flags, {
                "/E", entry,
                "/T", profile,
                "/Fo", output,
                projectile_trail_shader_source,
            }))
        end
        compile("VSMain", "vs_5_0", vertex_temp)
        compile("PSMain", "ps_5_0", pixel_temp)

        local debug_symbols =
            (build_mode == "debug" or build_mode == "internal") and
            "embedded" or "none"
        io.writefile(manifest_temp, string.format(
            "format=GeneralsTDShaderPackage\n" ..
            "version=%d\n" ..
            "shader=projectile_trail\n" ..
            "source_sha256=%s\n" ..
            "vertex_file=projectile_trail_vs.cso\n" ..
            "vertex_profile=vs_5_0\n" ..
            "pixel_file=projectile_trail_ps.cso\n" ..
            "pixel_profile=ps_5_0\n" ..
            "debug_symbols=%s\n",
            projectile_trail_shader_package_version,
            projectile_trail_shader_source_hash, debug_symbols))

        os.tryrm(vertex_blob)
        os.tryrm(pixel_blob)
        os.tryrm(manifest)
        os.mv(vertex_temp, vertex_blob)
        os.mv(pixel_temp, pixel_blob)
        os.mv(manifest_temp, manifest)
    end, {
        dependfile = depend_file,
        files = {
            projectile_trail_shader_source,
            path.join(os.projectdir(), "src/engine/xmake.lua"),
        },
        values = {
            projectile_trail_shader_package_version,
            projectile_trail_shader_source_hash,
            build_mode,
        },
    })
end

local function build_particle_shader_package(
    target, depend, os, path, io, get_config)
    local build_mode = get_config("mode") or "debug"
    local package_dir = path.join(target:targetdir(), "shaders")
    local outputs = {
        particle_vertex = path.join(package_dir, "particle_vs.cso"),
        particle_pixel = path.join(package_dir, "particle_ps.cso"),
        particle_alpha_test = path.join(
            package_dir, "particle_alpha_test_ps.cso"),
        smudge_vertex = path.join(package_dir, "particle_smudge_vs.cso"),
        smudge_pixel = path.join(package_dir, "particle_smudge_ps.cso"),
    }
    local manifest = path.join(package_dir, "particle.manifest")
    local depend_file = path.join(
        target:autogendir(), "particle_shader_package.d")
    local complete = os.isfile(manifest)
    for _, output in pairs(outputs) do
        complete = complete and os.isfile(output)
    end
    if not complete then os.tryrm(depend_file) end

    depend.on_changed(function ()
        os.mkdir(package_dir)
        local temporary = {}
        for key, output in pairs(outputs) do
            temporary[key] = output .. ".tmp"
            os.tryrm(temporary[key])
        end
        local manifest_temp = manifest .. ".tmp"
        os.tryrm(manifest_temp)

        local common_flags = {"/nologo", "/Ges", "/Zpr"}
        if build_mode == "debug" or build_mode == "internal" then
            table.insert(common_flags, "/Zi")
            table.insert(common_flags, "/Od")
        else
            table.insert(common_flags, "/O3")
        end
        local function compile(entry, profile, output)
            os.vrunv("fxc", table.join(common_flags, {
                "/E", entry, "/T", profile, "/Fo", output,
                particle_shader_source,
            }))
        end
        compile("ParticleVSMain", "vs_5_0", temporary.particle_vertex)
        compile("ParticlePSMain", "ps_5_0", temporary.particle_pixel)
        compile("ParticlePSAlphaTest", "ps_5_0", temporary.particle_alpha_test)
        compile("SmudgeVSMain", "vs_5_0", temporary.smudge_vertex)
        compile("SmudgePSMain", "ps_5_0", temporary.smudge_pixel)

        local debug_symbols =
            (build_mode == "debug" or build_mode == "internal") and
            "embedded" or "none"
        io.writefile(manifest_temp, string.format(
            "format=GeneralsTDShaderPackage\n" ..
            "version=%d\n" ..
            "shader=particle\n" ..
            "source_sha256=%s\n" ..
            "particle_vertex_file=particle_vs.cso\n" ..
            "particle_vertex_profile=vs_5_0\n" ..
            "particle_pixel_file=particle_ps.cso\n" ..
            "particle_pixel_profile=ps_5_0\n" ..
            "particle_alpha_test_file=particle_alpha_test_ps.cso\n" ..
            "particle_alpha_test_profile=ps_5_0\n" ..
            "smudge_vertex_file=particle_smudge_vs.cso\n" ..
            "smudge_vertex_profile=vs_5_0\n" ..
            "smudge_pixel_file=particle_smudge_ps.cso\n" ..
            "smudge_pixel_profile=ps_5_0\n" ..
            "debug_symbols=%s\n",
            particle_shader_package_version, particle_shader_source_hash,
            debug_symbols))

        for key, output in pairs(outputs) do
            os.tryrm(output)
            os.mv(temporary[key], output)
        end
        os.tryrm(manifest)
        os.mv(manifest_temp, manifest)
    end, {
        dependfile = depend_file,
        files = {
            particle_shader_source,
            path.join(os.projectdir(), "src/engine/xmake.lua"),
        },
        values = {
            particle_shader_package_version,
            particle_shader_source_hash,
            build_mode,
        },
    })
end

local function build_ground_projector_shader_package(
    target, depend, os, path, io, get_config)
    local build_mode = get_config("mode") or "debug"
    local package_dir = path.join(target:targetdir(), "shaders")
    local vertex_blob = path.join(package_dir, "ground_projector_vs.cso")
    local pixel_blob = path.join(package_dir, "ground_projector_ps.cso")
    local manifest = path.join(package_dir, "ground_projector.manifest")
    local depend_file = path.join(
        target:autogendir(), "ground_projector_shader_package.d")

    if not os.isfile(vertex_blob) or not os.isfile(pixel_blob) or
       not os.isfile(manifest) then
        os.tryrm(depend_file)
    end

    depend.on_changed(function ()
        os.mkdir(package_dir)
        local vertex_temp = vertex_blob .. ".tmp"
        local pixel_temp = pixel_blob .. ".tmp"
        local manifest_temp = manifest .. ".tmp"
        os.tryrm(vertex_temp)
        os.tryrm(pixel_temp)
        os.tryrm(manifest_temp)

        local common_flags = {"/nologo", "/Ges", "/Zpr"}
        if build_mode == "debug" or build_mode == "internal" then
            table.insert(common_flags, "/Zi")
            table.insert(common_flags, "/Od")
        else
            table.insert(common_flags, "/O3")
        end
        local function compile(entry, profile, output)
            os.vrunv("fxc", table.join(common_flags, {
                "/E", entry, "/T", profile, "/Fo", output,
                ground_projector_shader_source,
            }))
        end
        compile("VSMain", "vs_5_0", vertex_temp)
        compile("PSMain", "ps_5_0", pixel_temp)

        local debug_symbols =
            (build_mode == "debug" or build_mode == "internal") and
            "embedded" or "none"
        io.writefile(manifest_temp, string.format(
            "format=GeneralsTDShaderPackage\n" ..
            "version=%d\n" ..
            "shader=ground_projector\n" ..
            "source_sha256=%s\n" ..
            "vertex_file=ground_projector_vs.cso\n" ..
            "vertex_profile=vs_5_0\n" ..
            "pixel_file=ground_projector_ps.cso\n" ..
            "pixel_profile=ps_5_0\n" ..
            "debug_symbols=%s\n",
            ground_projector_shader_package_version,
            ground_projector_shader_source_hash, debug_symbols))

        os.tryrm(vertex_blob)
        os.tryrm(pixel_blob)
        os.tryrm(manifest)
        os.mv(vertex_temp, vertex_blob)
        os.mv(pixel_temp, pixel_blob)
        os.mv(manifest_temp, manifest)
    end, {
        dependfile = depend_file,
        files = {
            ground_projector_shader_source,
            path.join(os.projectdir(), "src/engine/xmake.lua"),
        },
        values = {
            ground_projector_shader_package_version,
            ground_projector_shader_source_hash,
            build_mode,
        },
    })
end

target("render_shader_package")
    set_kind("phony")
    set_group("engine")
    set_default(false)
    on_build(function (target)
        import("core.project.depend")
        build_fxaa_shader_package(
            target, depend, os, path, io, get_config)
        build_projectile_trail_shader_package(
            target, depend, os, path, io, get_config)
        build_particle_shader_package(
            target, depend, os, path, io, get_config)
        build_ground_projector_shader_package(
            target, depend, os, path, io, get_config)
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "typed_fx_world",
                version = typed_fx_world_shader_package_version,
                source = typed_fx_world_shader_source,
                source_hash = typed_fx_world_shader_source_hash,
                stages = {
                    {key = "vertex", file = "typed_fx_world_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "pixel", file = "typed_fx_world_ps.cso",
                     entry = "PSMain", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "particle_compute",
                version = particle_compute_shader_package_version,
                source = particle_compute_shader_source,
                source_hash = particle_compute_shader_source_hash,
                dependencies = {particle_compute_contract_source},
                stages = {
                    {key = "reset_compute",
                     file = "particle_compute_reset_cs.cso",
                     entry = "ResetCS", profile = "cs_5_0"},
                    {key = "retire_compute",
                     file = "particle_compute_retire_cs.cso",
                     entry = "ApplyRetireCS", profile = "cs_5_0"},
                    {key = "birth_compute",
                     file = "particle_compute_birth_cs.cso",
                     entry = "ApplyBirthCS", profile = "cs_5_0"},
                    {key = "integrate_compute",
                     file = "particle_compute_integrate_cs.cso",
                     entry = "IntegrateCS", profile = "cs_5_0"},
                    {key = "reset_alive_compact_compute",
                     file = "particle_compute_reset_alive_compact_cs.cso",
                     entry = "ResetAliveCompactCS", profile = "cs_5_0"},
                    {key = "alive_compact_compute",
                     file = "particle_compute_alive_compact_cs.cso",
                     entry = "CompactAliveCS", profile = "cs_5_0"},
                    {key = "visible_compact_reset_compute",
                     file = "particle_compute_visible_compact_reset_cs.cso",
                     entry = "ResetVisibleCompactCS", profile = "cs_5_0"},
                    {key = "visible_compact_compute",
                     file = "particle_compute_visible_compact_cs.cso",
                     entry = "CompactVisibleCS", profile = "cs_5_0"},
                    {key = "material_bin_reset_compute",
                     file = "particle_compute_material_bin_reset_cs.cso",
                     entry = "ResetMaterialBinsCS", profile = "cs_5_0"},
                    {key = "material_bin_count_compute",
                     file = "particle_compute_material_bin_count_cs.cso",
                     entry = "CountMaterialBinsCS", profile = "cs_5_0"},
                    {key = "material_bin_prefix_compute",
                     file = "particle_compute_material_bin_prefix_cs.cso",
                     entry = "PrefixMaterialBinsCS", profile = "cs_5_0"},
                    {key = "material_bin_scatter_compute",
                     file = "particle_compute_material_bin_scatter_cs.cso",
                     entry = "ScatterMaterialBinsCS", profile = "cs_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "particle_gpu_billboard",
                version = particle_gpu_billboard_shader_package_version,
                source = particle_gpu_billboard_shader_source,
                source_hash = particle_gpu_billboard_shader_source_hash,
                dependencies = {particle_compute_contract_source},
                stages = {
                    {key = "vertex", file = "particle_gpu_billboard_vs.cso",
                     entry = "GpuParticleBillboardVS", profile = "vs_5_0"},
                    {key = "additive_pixel",
                     file = "particle_gpu_billboard_additive_ps.cso",
                     entry = "GpuParticleBillboardPSAdditive", profile = "ps_5_0"},
                    {key = "alpha_test_pixel",
                     file = "particle_gpu_billboard_alpha_test_ps.cso",
                     entry = "GpuParticleBillboardPSAlphaTest", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "world_main",
                version = world_main_shader_package_version,
                source = world_main_shader_source,
                source_hash = world_main_shader_source_hash,
                stages = {
                    {key = "vertex", file = "world_main_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "pixel", file = "world_main_ps.cso",
                     entry = "PSMain", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "world_directional_shadow",
                version = world_directional_shadow_shader_package_version,
                source = world_directional_shadow_shader_source,
                source_hash = world_directional_shadow_shader_source_hash,
                stages = {
                    {key = "vertex", file = "world_directional_shadow_vs.cso",
                     entry = "ShadowVS", profile = "vs_5_0"},
                    {key = "pixel", file = "world_directional_shadow_ps.cso",
                     entry = "ShadowAlphaPS", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "screen_fade",
                version = screen_fade_shader_package_version,
                source = screen_fade_shader_source,
                source_hash = screen_fade_shader_source_hash,
                stages = {
                    {key = "vertex", file = "screen_fade_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "pixel", file = "screen_fade_ps.cso",
                     entry = "PSMain", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "black_and_white",
                version = black_and_white_shader_package_version,
                source = black_and_white_shader_source,
                source_hash = black_and_white_shader_source_hash,
                stages = {
                    {key = "vertex", file = "black_and_white_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "pixel", file = "black_and_white_ps.cso",
                     entry = "PSMain", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "motion_blur",
                version = motion_blur_shader_package_version,
                source = motion_blur_shader_source,
                source_hash = motion_blur_shader_source_hash,
                stages = {
                    {key = "vertex", file = "motion_blur_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "pixel", file = "motion_blur_ps.cso",
                     entry = "PSMain", profile = "ps_5_0"},
                },
            })
        build_stage_shader_package(
            target, depend, os, path, io, get_config, {
                name = "ui_2d",
                version = ui_2d_shader_package_version,
                source = ui_2d_shader_source,
                source_hash = ui_2d_shader_source_hash,
                stages = {
                    {key = "vertex", file = "ui_2d_vs.cso",
                     entry = "VSMain", profile = "vs_5_0"},
                    {key = "solid_pixel", file = "ui_2d_solid_ps.cso",
                     entry = "PSSolid", profile = "ps_5_0"},
                    {key = "textured_pixel", file = "ui_2d_textured_ps.cso",
                     entry = "PSTextured", profile = "ps_5_0"},
                },
            })
    end)

local function configure_engine_shard(name, files)
    target(name)
        set_kind("static")
        set_group("engine")
        set_pcxxheader("$(projectdir)/src/pch.h")
        -- Keep the historical bare include roots available while physical
        -- ownership is split. New cross-shard includes should use the full
        -- engine/... path so a later include-surface tightening is mechanical.
        add_includedirs("..",
                        "gui",
                        "gui/base",
                        "gui/core",
                        "gui/draw",
                        "gui/widget",
                        "renderer",
                        "renderer/runtime",
                        "renderer/d3d12",
                        "renderer/d3d12/runtime",
                        "renderer/ui",
                        "renderer/world",
                        "audio",
                        "font",
                        "input",
                        "resource",
                        "texture",
                        "system",
                        "fx",
                        "fx/runtime")
        add_files(table.unpack(files))
        add_deps("core")
    target_end()
end

-- CPU resource admission and worker scheduling have no renderer dependency.
configure_engine_shard("engine_resource", {
    "resource/*.cpp",
})
target("engine_resource")
    add_deps("taskflow")

-- Audio consumes renderer-neutral presentation contracts and the shared
-- resource scheduler; it does not depend on D3D12, GUI, fonts or textures.
configure_engine_shard("engine_audio", {
    "audio/*.cpp",
})
target("engine_audio")
    add_deps("presentation_contracts", "engine_resource", "miniaudio")
    if is_mode("release") then
        add_cxflags("/fp:fast", {force = true})
    end

-- SDL input and authored cursor state remain independent of render backend
-- ownership. Cursor image values are shared presentation/core contracts.
configure_engine_shard("engine_input", {
    "input/*.cpp",
})
target("engine_input")
    add_deps("presentation_runtime")
    -- AuthoredCursorRuntime.h exposes SDL_Cursor/SDL_Surface types.
    add_packages("sdl3", {public = true})

-- Renderer, GUI, font, texture and FX runtime currently form one real include
-- SCC. Keep that SCC explicit instead of pretending these are independent
-- libraries; later contract extraction can split it without touching audio or
-- resource ownership again.
configure_engine_shard("engine_render_runtime", {
    "gui/**/*.cpp",
    "renderer/runtime/*.cpp",
    "renderer/d3d12/runtime/*.cpp",
    "renderer/ui/*.cpp",
    "renderer/world/**/*.cpp",
    "font/*.cpp",
    "texture/*.cpp",
    "fx/**/*.cpp",
})
target("engine_render_runtime")
    -- The executable is an in-game host. Legacy out-of-game menu stack
    -- implementations are intentionally absent from the production graph.
    add_deps("presentation_runtime", "taskflow", "engine_resource",
             "render_shader_package")
    -- DX12Renderer.h exposes SDL_Window; FreeType remains implementation-only.
    add_packages("sdl3", {public = true})
    add_packages("freetype")
    if is_mode("release") then
        add_cxflags("/fp:fast", {force = true})
    end

-- Process-facing subsystem adapters compose the lower engine shards but are
-- never depended on by them.
configure_engine_shard("engine_system", {
    "system/*.cpp",
})
target("engine_system")
    add_deps("presentation_runtime", "engine_resource", "engine_audio",
             "engine_input", "engine_render_runtime")

-- Stable application-facing composition target.
target("engine")
    set_kind("phony")
    set_group("engine")
    add_deps("engine_resource", "engine_audio", "engine_input",
             "engine_render_runtime", "engine_system")

target("engine_render_runtime")
    add_defines(
        "TD_FXAA_SHADER_PACKAGE_VERSION=" .. fxaa_shader_package_version,
        "TD_FXAA_SHADER_SOURCE_SHA256=\"" .. fxaa_shader_source_hash .. "\"",
        "TD_PROJECTILE_TRAIL_SHADER_PACKAGE_VERSION=" ..
            projectile_trail_shader_package_version,
        "TD_PROJECTILE_TRAIL_SHADER_SOURCE_SHA256=\"" ..
            projectile_trail_shader_source_hash .. "\"",
        "TD_PARTICLE_SHADER_PACKAGE_VERSION=" ..
            particle_shader_package_version,
        "TD_PARTICLE_SHADER_SOURCE_SHA256=\"" ..
            particle_shader_source_hash .. "\"",
        "TD_GROUND_PROJECTOR_SHADER_PACKAGE_VERSION=" ..
            ground_projector_shader_package_version,
        "TD_GROUND_PROJECTOR_SHADER_SOURCE_SHA256=\"" ..
            ground_projector_shader_source_hash .. "\"",
        "TD_TYPED_FX_WORLD_SHADER_PACKAGE_VERSION=" ..
            typed_fx_world_shader_package_version,
        "TD_TYPED_FX_WORLD_SHADER_SOURCE_SHA256=\"" ..
            typed_fx_world_shader_source_hash .. "\"",
        "TD_UI_2D_SHADER_PACKAGE_VERSION=" ..
            ui_2d_shader_package_version,
        "TD_UI_2D_SHADER_SOURCE_SHA256=\"" ..
            ui_2d_shader_source_hash .. "\"",
        "TD_WORLD_MAIN_SHADER_PACKAGE_VERSION=" ..
            world_main_shader_package_version,
        "TD_WORLD_MAIN_SHADER_SOURCE_SHA256=\"" ..
            world_main_shader_source_hash .. "\"",
        "TD_WORLD_DIRECTIONAL_SHADOW_SHADER_PACKAGE_VERSION=" ..
            world_directional_shadow_shader_package_version,
        "TD_WORLD_DIRECTIONAL_SHADOW_SHADER_SOURCE_SHA256=\"" ..
            world_directional_shadow_shader_source_hash .. "\"",
        "TD_SCREEN_FADE_SHADER_PACKAGE_VERSION=" ..
            screen_fade_shader_package_version,
        "TD_SCREEN_FADE_SHADER_SOURCE_SHA256=\"" ..
            screen_fade_shader_source_hash .. "\"",
        "TD_BLACK_AND_WHITE_SHADER_PACKAGE_VERSION=" ..
            black_and_white_shader_package_version,
        "TD_BLACK_AND_WHITE_SHADER_SOURCE_SHA256=\"" ..
            black_and_white_shader_source_hash .. "\"",
        "TD_MOTION_BLUR_SHADER_PACKAGE_VERSION=" ..
            motion_blur_shader_package_version,
        "TD_MOTION_BLUR_SHADER_SOURCE_SHA256=\"" ..
            motion_blur_shader_source_hash .. "\"",
        "TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION=" ..
            particle_compute_shader_package_version,
        "TD_PARTICLE_COMPUTE_SHADER_SOURCE_SHA256=\"" ..
            particle_compute_shader_source_hash .. "\"",
        "TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION=" ..
            particle_gpu_billboard_shader_package_version,
        "TD_PARTICLE_GPU_BILLBOARD_SHADER_SOURCE_SHA256=\"" ..
            particle_gpu_billboard_shader_source_hash .. "\"")
