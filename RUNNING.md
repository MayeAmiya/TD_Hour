**English** · [简体中文](RUNNING.zh-CN.md)

# Running TD_Hour

Build, launch, command-line options, and configuration for TD_Hour.

---

## Building

```bash
git clone --recursive https://github.com/MayeAmiya/TD_Hour.git
cd TD_Hour
xmake                # release build
xmake f -m debug && xmake    # debug build
```

Third-party dependencies are managed as git submodules; `--recursive` is required when cloning.

## Launch

The executable target is `generals_td`, output to `Bin/<mode>/`:

| Build | Executable |
|---|---|
| Release | `Bin/release/generals_td.exe` |
| Debug | `Bin/debug/generals_td.exe` |

Run it directly, or through xmake:

```bash
xmake run generals_td
```

The binary is built as a console subsystem application in all configurations: the operational Info/Warning/Error log is printed to the console and mirrored to `generals.log` beside the executable.

## Command-line parameters

**Syntax**: `--key=value`, `--key value`, or `-key value`. A bare flag (no value) is treated as `true`. Parameter keys are case-insensitive.

### Display

| Parameter | Meaning |
|---|---|
| `--resolution=WxH` | Window resolution, e.g. `--resolution=1920x1080` |
| `--render-width=N` | Render width override |
| `--render-height=N` | Render height override |
| `--fxaa` / `--fxaa=false` | Enable/disable FXAA |
| `--texture-filter=N` | Texture filter level (default 2) |
| `--anisotropy=N` | Anisotropy level (default 2) |
| `--maximum-particles=N` | Maximum particle count (default 2500) |
| `--texture-reduction=N` | Texture reduction factor (default 0) |

### Audio

| Parameter | Meaning |
|---|---|
| `--nosound` | Disable the audio playback device |

### Content / mods

| Parameter | Meaning |
|---|---|
| `--mod=<path>` | Override the mod data path; relative paths resolve under the user data directory |

### Launcher integration

These are used when launching from an external launcher (e.g. matchmaking/session bootstrap):

| Parameter | Meaning |
|---|---|
| `--session-descriptor=<path>` | Load a launcher bootstrap descriptor file to start a session |
| `--session-ticket=<ticket>` | Session ticket; defaults to the descriptor filename stem; the launch outcome is written to `<ticket>.outcome.ini` |

### Debug / developer (debug builds only)

Compiled in only under `TD_DEBUG_ENABLED`:

| Parameter | Meaning |
|---|---|
| `--direct-start` | Direct campaign start |
| `--debug-world-map=<map>` | Load a debug world map |
| `--exit-after-frames=N` | Exit after N frames (automation) |
| `--debug-world-only` | Debug world only |

> **Note**: `--windowed`, `--fullscreen`, `--nomusic`, `--dev`, `--loglevel`, and `--savedir` are reserved parameter names declared by the command-line layer but are **not yet consumed** by any system. Do not rely on them.

## Configuration file — `GameOptions.ini`

At startup, `GlobalData` loads `GameOptions.ini`. Lookup is relative to the **executable path**, never the current working directory, so a shortcut or launcher cannot mount an unrelated content tree:

1. Beside the executable (`Bin/<mode>/`)
2. One directory above (`Bin/`)
3. Two directories above (installation root)

The first file found is adopted.

**Format**: flat `key=value` lines; keys are case-insensitive.

| Key | Purpose |
|---|---|
| `generalsdatapath` | Generals data path |
| `zerohourdatapath` | Zero Hour data path |
| `moddatapath` | Mod data path |
| `localedatapath` | Locale data path |
| `userdatapath` | User data path |
| `savedatapath` / `savepath` | Save data path |
| `replaydatapath` / `replaypath` | Replay data path |
| `mapsearchpaths` / `mappath` / `localmappath` | Map search paths |
| `adjustclifftextures` | Adjust cliff textures (boolean) |

## Notes

- This repository ships no game assets. The game data paths in `GameOptions.ini` must point to a *Generals / Zero Hour* installation (e.g., the C&C Ultimate Collection) before the game can load content.
