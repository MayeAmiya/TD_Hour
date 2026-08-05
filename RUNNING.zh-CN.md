[English](RUNNING.md) · **简体中文**

# 运行 TD_Hour

构建、启动方式、可执行文件启动参数与配置文件说明。

---

## 构建

```bash
git clone --recursive https://github.com/MayeAmiya/TD_Hour.git
cd TD_Hour
xmake                        # Release 构建
xmake f -m debug && xmake    # Debug 构建
```

第三方依赖以 git submodule 管理,克隆时必须使用 `--recursive`。

## 启动方式

可执行目标为 `generals_td`,构建产物位于 `Bin/<mode>/`:

| 构建 | 可执行文件 |
|---|---|
| Release | `Bin/release/generals_td.exe` |
| Debug | `Bin/debug/generals_td.exe` |

可直接运行该可执行文件,也可通过 xmake 运行:

```bash
xmake run generals_td
```

所有构建配置均以控制台子系统方式链接:运行级 Info/Warning/Error 日志输出到控制台,并镜像到可执行文件旁的 `generals.log`。

## 可执行文件启动参数

**语法**:`--key=value`、`--key value` 或 `-key value`。不带值的裸标志视为 `true`。参数名不区分大小写。

### 显示

| 参数 | 含义 |
|---|---|
| `--resolution=WxH` | 窗口分辨率,如 `--resolution=1920x1080` |
| `--render-width=N` | 渲染宽度覆盖 |
| `--render-height=N` | 渲染高度覆盖 |
| `--fxaa` / `--fxaa=false` | 开启/关闭 FXAA |
| `--texture-filter=N` | 纹理过滤级别(默认 2) |
| `--anisotropy=N` | 各向异性级别(默认 2) |
| `--maximum-particles=N` | 最大粒子数(默认 2500) |
| `--texture-reduction=N` | 纹理降级系数(默认 0) |

### 音频

| 参数 | 含义 |
|---|---|
| `--nosound` | 禁用音频播放设备 |

### 内容 / 模组

| 参数 | 含义 |
|---|---|
| `--mod=<path>` | 覆盖模组数据路径;相对路径在用户数据目录下解析 |

### 启动器集成(由外部启动器调用)

| 参数 | 含义 |
|---|---|
| `--session-descriptor=<path>` | 从外部启动器写入的 bootstrap 描述文件启动会话 |
| `--session-ticket=<ticket>` | 会话票据;缺省取描述文件名主干;启动结果写入 `<ticket>.outcome.ini` |

### 调试 / 开发(Debug 构建专用)

仅在 `TD_DEBUG_ENABLED` 下编译:

| 参数 | 含义 |
|---|---|
| `--direct-start` | 直接开始战役 |
| `--debug-world-map=<map>` | 加载调试世界地图 |
| `--exit-after-frames=N` | 运行 N 帧后退出(自动化) |
| `--debug-world-only` | 仅调试世界 |

> **注意**:`--windowed`、`--fullscreen`、`--nomusic`、`--dev`、`--loglevel`、`--savedir` 是命令行层预留的参数名,**当前尚无任何系统消费**,请勿依赖。

## 配置文件 `GameOptions.ini`

启动时 `GlobalData` 加载 `GameOptions.ini`。查找路径以**可执行文件位置**为基准(而非当前工作目录),避免快捷方式或启动器挂载到无关内容树:

1. 可执行文件同目录(`Bin/<mode>/`)
2. 上一级目录(`Bin/`)
3. 上上级目录(安装根目录)

采用第一个找到的文件。

**格式**:扁平 `key=value` 行,键名不区分大小写。

| 键 | 用途 |
|---|---|
| `generalsdatapath` | Generals 数据路径 |
| `zerohourdatapath` | Zero Hour 数据路径 |
| `moddatapath` | 模组数据路径 |
| `localedatapath` | 本地化数据路径 |
| `userdatapath` | 用户数据路径 |
| `savedatapath` / `savepath` | 存档数据路径 |
| `replaydatapath` / `replaypath` | 回放数据路径 |
| `mapsearchpaths` / `mappath` / `localmappath` | 地图搜索路径 |
| `adjustclifftextures` | 调整悬崖贴图(布尔) |

## 注意事项

- 本仓库不含任何游戏资产。运行前须将 `GameOptions.ini` 中的数据路径指向《将军 / 绝命时刻》游戏安装目录(如 C&C 终极合集),否则无法加载内容。
