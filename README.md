**English** · [简体中文](README.zh-CN.md)

# TD_Hour — Command & Conquer: Generals / Zero Hour Engine Reset

> A reimplementation of the *Command & Conquer: Generals / Zero Hour* engine, based on the GPLv3-licensed source code released by Electronic Arts.
> This is an independent fan project. It is not affiliated with or endorsed by Electronic Arts Inc. or its affiliates.
> This repository does not include any original game assets; game data files must be provided by the user.

**License**: GNU GPL v3 + EA Additional Terms | **Stack**: C++20 · Direct3D 12 · SDL3 · ECS | **Scale**: ~1,700 source files / ~480k lines of code

---

## Background

In 2025, Electronic Arts released the engine source code of *Generals / Zero Hour* under the GNU GPL v3 license. The codebase dates from 2003 and depends on legacy tooling that is no longer maintained (Visual C++ 6.0) and on several proprietary third-party libraries (DirectX 9 SDK, STLport, Miles Sound System, Bink Video, etc.), making it difficult to build and evolve with modern compilers, graphics APIs, and multiplayer environments.

TD_Hour preserves the original rules, data formats, and game design while rewriting the entire runtime engine on a modern architecture, targeting current toolchains and rendering models and providing a foundation for long-term maintenance and extension.

---

## Technical Highlights

### 1. Modern Engine Rewrite

- Built with C++20 and the xmake build system; ~480k lines of code across ~1,700 source files
- Modern Direct3D 12 rendering pipeline with a GPU memory allocator, DirectXMath, and a composable world-rendering pipeline
- No dependency on any discontinued proprietary SDK

### 2. Layered Architecture

```
src/app           Host layer: windowing, frame pacing, input dispatch, UI shell
src/core          Foundation: math, containers, ECS, compression, debug tooling
src/engine        Engine subsystems: rendering, audio, GUI, networking, resources, textures, video
src/game          Game domain: commands, AI, navigation, objects, combat, production, scripts, scenarios
src/presentation  Presentation layer: camera, render extraction, FX, UI projection
```

Each layer depends only on the layers beneath it and communicates through explicit contracts. The simulation layer is unaware of rendering frames, and the presentation layer does not access ECS internals, establishing clear responsibility boundaries between systems.

### 3. Deterministic Simulation

Multiplayer synchronization, replay, and save/load all rely on determinism as a foundational property. Determinism has been a first-order design constraint since the project's inception:

- Lockstep frame buffering with a matching packet codec (`LockstepFrameBuffer` / `LockstepPacketCodec`)
- All orders commit only on confirmed ticks; execution results are merged in stable `ObjectId` / `ActionId` order
- Simulation state is recorded through layered snapshots and journals covering orders, AI, weapons, and presentation events
- Native replay and save/load support (`ReplayStorage` / `ReplayFileCodec`); action queues and domain runtimes are fully serializable

The core constraint is that rendering frames, wall clocks, and thread completion ordering must never drive logical progression. Identical input sequences produce identical results at different frame rates (e.g., 30 FPS vs. 200 FPS), providing a reliable basis for multiplayer synchronization and replay.

### 4. Data-Driven Performance

- Hot paths use an ECS (entt) with structure-of-arrays (SoA) layouts; AI state families, shadow batching, and avoidance kernels are cache-friendly SoA implementations
- Parallel workloads are scheduled through taskflow task graphs; Tracy is integrated for profiling
- Built on mimalloc (memory allocator), spdlog (logging), and fmt (formatting) as base components

### 5. Generalization over Special-Casing

- Paths are modeled as deterministic action sequences rather than coordinate lists. Each action is started, run, and terminated by its owning domain (movement, combat, ability, construction); the sequencer advances the queue only upon a terminal outcome, eliminating action-stitching failures, stuck nodes, and lost commands at the root
- Target facts (`TargetFacts`) are separated from weapon/ability decision policy: target-state queries are decoupled from termination decisions, anti-air weapons do not misjudge ground targets, and in-flight projectiles are not rolled back when an order's target is lost
- Sustained and limited behaviors (e.g., flame-wall sustained fire, MiG limited ammunition) are expressed through generic rules (`MaxShotsToFire`, action terminal semantics) rather than unit-name or ability-name special-casing

### 6. Presentation / Simulation Decoupling

UI and rendering consume only confirmed, immutable snapshots; they neither read the ECS nor determine action completion:

```
Simulation action queue → confirmed tick → frame extractor → confirmed snapshot / journal → UI / Renderer
```

- Trails, explosions, animations, and audio are delivered with exactly-once semantics — neither lost nor duplicated
- Allied paths are projected through presentation-extraction policies and filtered by observer visibility, keeping decision logic out of the UI

### 7. Engineering & Maintenance

- All 17 third-party dependencies are managed as git submodules with pinned versions; see [THIRD_PARTY.md](THIRD_PARTY.md)
- One-command builds via xmake, with no dependence on legacy toolchain versions
- Key design decisions are frozen in architecture documents (see [docs/](docs/)) so subsequent development follows the established architecture

### 8. License Compliance

- The GNU GPL v3 license and EA additional terms are preserved in full (see [LICENSE.md](LICENSE.md)); this is a legitimate derivative work
- The repository contains no original game assets and therefore carries no asset-licensing risk; as a fork of the upstream repository, its lineage is directly traceable
- This is a modified version of the upstream codebase and is never presented as the original program

---

## Building

```bash
git clone --recursive https://github.com/MayeAmiya/TD_Hour.git
cd TD_Hour
xmake
```

Third-party dependencies are managed as submodules; `--recursive` is required when cloning.

> This repository does not ship game assets. Running the game requires the *Generals / Zero Hour* game data files (e.g., the C&C Ultimate Collection).

---

## Current Status

The engine runs; core closed loops for commands, navigation, presentation, and audio have been established in an initial form. A number of runtime defects remain and are currently being addressed through controlled regression fixes; progress and confirmed issues are tracked in the project ledger.

---

## License & Acknowledgment

This project is a derivative work of [electronicarts/CnC_Generals_Zero_Hour](https://github.com/electronicarts/CnC_Generals_Zero_Hour), released under the **GNU GPL v3** and **EA Additional Terms**; the full license text is available in [LICENSE.md](LICENSE.md).

Per the additional terms: modified versions must not be presented as the original program; EA trademarks such as "Command & Conquer" must not be used to imply authorization, affiliation, or endorsement; and the program is provided "as is" without warranty of any kind.
