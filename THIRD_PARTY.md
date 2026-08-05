# Third-Party Libraries

All dependencies are managed as git submodules.

## Clone with dependencies

```bash
git clone --recursive <repo-url>
```

## Update dependencies

```bash
git submodule update --init --recursive
```

## Library list

| Library | Source | Version | Purpose |
|---------|--------|---------|---------|
| D3D12MemoryAllocator | [GPUOpen](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) | v3.2.0 | D3D12 GPU memory allocation |
| DirectX-Headers | [Microsoft](https://github.com/microsoft/DirectX-Headers) | v1.721.2-preview | D3D12 / DXR / DirectSR headers |
| DirectXMath | [Microsoft](https://github.com/microsoft/DirectXMath) | may2026 | SIMD math library |
| enet | [lsalzman](https://github.com/lsalzman/enet) | v1.3.15 | Networking (reliable UDP) |
| entt | [skypjack](https://github.com/skypjack/entt) | v3.16.0 | ECS framework |
| fmt | [fmtlib](https://github.com/fmtlib/fmt) | 12.2.0 | String formatting |
| freetype | [freedesktop](https://gitlab.freedesktop.org/freetype/freetype.git) | VER-2-14-3 | Font rendering |
| mimalloc | [Microsoft](https://github.com/microsoft/mimalloc) | v2.3.2 | Memory allocator |
| miniaudio | [mackron](https://github.com/mackron/miniaudio) | 0.11.22 | Audio playback |
| SDL3 | [libsdl-org](https://github.com/libsdl-org/SDL) | release-3.4.0 | Windowing / input |
| spdlog | [gabime](https://github.com/gabime/spdlog) | v1.2.1 | Logging |
| taskflow | [taskflow](https://github.com/taskflow/taskflow) | v4.1.0 | Task-based parallelism |
| tracy | [wolfpld](https://github.com/wolfpld/tracy) | v0.13.1 | Profiling |
| unordered_dense | [martinus](https://github.com/martinus/unordered_dense) | v4.8.1 | Hash map |
| vectorclass | [vectorclass](https://github.com/vectorclass/version2) | v2.02.03 | SIMD vector math |
| yaml-cpp | [jbeder](https://github.com/jbeder/yaml-cpp) | 0.9.0 | YAML parsing |
| zlib | [madler](https://github.com/madler/zlib) | v1.3.2 | Compression |
