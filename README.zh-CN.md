[English](README.md) · **简体中文**

# TD_Hour — Command & Conquer: Generals / Zero Hour 引擎重置项目

> 基于 Electronic Arts 以 GPLv3 许可开源的《Command & Conquer: Generals / Zero Hour》源码进行的引擎重实现。
> 本项目为独立同人项目,与 Electronic Arts Inc. 及其关联方无任何关联,亦不代表其立场。
> 本仓库不包含任何原版游戏资产,运行需用户自行提供游戏数据文件。

**许可**: GNU GPL v3 + EA 附加条款 | **技术栈**: C++20 · Direct3D 12 · SDL3 · ECS | **规模**: 约 1,700 个源文件 / 约 48 万行代码

---

## 项目背景

EA 于 2025 年以 GPLv3 许可公开了《将军 / 绝命时刻》的引擎源码。该代码库完成于 2003 年,依赖已停止维护的构建工具链(Visual C++ 6.0)及多项专有第三方库(DirectX 9 SDK、STLport、Miles Sound System、Bink Video 等),在现代编译器、图形 API 与多人游戏环境下难以直接沿用。

TD_Hour 保留了原版的规则体系、数据格式与玩法设计,同时将整个运行引擎重写为现代架构,以适应现行的工具链与渲染模型,并为长期维护与功能扩展提供基础。

---

## 技术特点

### 1. 现代化引擎重写

- 基于 C++20 与 xmake 构建系统,代码量约 48 万行,分布于约 1,700 个源文件
- 采用 Direct3D 12 渲染管线,集成 GPU 显存分配器、DirectXMath 数学库与可组合的世界渲染管线
- 不依赖任何已停止维护的专有 SDK

### 2. 分层架构

```
src/app           宿主层:窗口管理、帧节奏、输入分发、UI 壳
src/core          基础层:数学、容器、ECS、压缩、调试工具
src/engine        引擎层:渲染、音频、GUI、网络、资源、纹理、视频
src/game          游戏域:命令、AI、导航、对象、战斗、生产、脚本、场景
src/presentation  表现层:相机、渲染提取、特效、UI 投影
```

各层仅依赖其下层,层间以显式契约(contracts)交互。模拟层不感知渲染帧,表现层不访问 ECS 细节,从而在系统间建立明确的职责边界。

### 3. 确定性模拟

多人同步、回放与存读档以确定性为根本前提。引擎自设计之初便将确定性列为首要约束:

- 采用锁步帧缓冲与对应包编解码(`LockstepFrameBuffer` / `LockstepPacketCodec`)
- 所有订单仅在 `confirmed tick` 提交,执行结果按稳定的 `ObjectId` / `ActionId` 顺序归并
- 通过快照与日志分层记录模拟状态,覆盖订单、AI、武器与表现事件
- 原生支持回放与存读档(`ReplayStorage` / `ReplayFileCodec`),动作队列及各域运行时均可序列化

核心约束为:禁止以渲染帧、墙钟或线程完成时序决定逻辑推进。相同操作序列在不同帧率(如 30 FPS 与 200 FPS)下产生完全一致的结果,为多人同步与回放提供了可靠基础。

### 4. 数据驱动与性能设计

- 基于 ECS(entt)与结构体数组(SoA)布局组织热点数据,AI 状态族、影子批处理、避让内核等均采用缓存友好的 SoA 形式
- 以 taskflow 任务图调度并行计算,集成 Tracy 进行性能剖析
- 采用 mimalloc 内存分配器、spdlog 日志库与 fmt 格式化库作为基础组件

### 5. 通用化设计

- 路径被建模为确定性动作序列而非坐标序列。每个动作由其所属领域(移动、战斗、技能、建造)负责开始、运行与终止,序列器仅在收到终态后推进队列,从根源上避免动作衔接断裂、节点卡死与命令丢失等问题
- 将目标事实(`TargetFacts`)与武器/技能判定策略分层:目标状态查询与终止决策分离,防空武器不误判地面目标,已发射投射物不因订单目标失效而回滚
- 持续型与有限型行为(如火墙持续攻击、米格有限弹药)均通过通用规则(`MaxShotsToFire`、动作终态语义)表达,不采用基于单位名或技能名的特殊化实现

### 6. 表现与模拟解耦

UI 与渲染仅消费已确认的不可变快照,不读取 ECS,也不判断动作完成:

```
模拟动作队列 → confirmed tick → 帧提取器 → 确认快照 / Journal → UI / Renderer
```

- 轨迹、爆炸、动画与音频以 exactly-once 语义交付,不丢失、不重复
- 队友路径通过表现提取策略投影,并按观察者可见性过滤目标信息,避免将判定逻辑复制至 UI

### 7. 工程与维护

- 17 项第三方依赖均以 git submodule 管理并锁定版本,详见 [THIRD_PARTY.md](THIRD_PARTY.md)
- 通过 xmake 完成一键构建,不再依赖特定版本的遗留工具链
- 关键设计决策以架构文档形式固化(见 [docs/](docs/)),后续开发遵循既有架构演进

### 8. 许可合规

- 完整保留 GPLv3 及 EA 附加条款(见 [LICENSE.md](LICENSE.md)),作为合法衍生作品发布
- 仓库不包含任何原版游戏资产,无资产版权风险;作为上游仓库的 fork,继承关系可直接追溯
- 本项目为对上游代码的修改版,不以任何形式标识为原版程序

---

## 构建

```bash
git clone --recursive https://github.com/MayeAmiya/TD_Hour.git
cd TD_Hour
xmake
```

第三方依赖以 submodule 管理,克隆时必须使用 `--recursive`。

> 本仓库不含游戏资产。运行需要自备《将军 / 绝命时刻》的游戏数据文件(例如 C&C 终极合集)。

---

## 当前状态

引擎可运行,命令、导航、表现、音频的核心闭环已初步落地。当前仍存在较多运行时缺陷,正处于受控回归修复阶段;进度与已确认问题记录于项目账本。

---

## 许可与致谢

本项目为 [electronicarts/CnC_Generals_Zero_Hour](https://github.com/electronicarts/CnC_Generals_Zero_Hour) 的衍生作品,依照 **GNU GPL v3** 及 **EA 附加条款**发布,完整许可文本见 [LICENSE.md](LICENSE.md)。

依据附加条款:修改版不得被标识为原版程序;不得使用 "Command & Conquer" 等 EA 商标暗示授权、关联或背书;程序按"原样"提供,不附带任何担保。
