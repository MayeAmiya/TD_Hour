# TODO — 未完成工作清单

> 状态基准:2026-08-06。按系统分组记录已确认的未完成项;状态以代码为准。
>
> 状态图例:**完成** / **部分完成** / **几乎未开始**(仅基础层 / 未接线)

---

## 1. 寻路与导航

状态:**部分完成**

- [x] 确定性 ObjectOrderQueue、Shift 追加、普通命令替换
- [x] typed order(Move / Attack / Build / SpecialPower 等)
- [x] confirmed waypoint snapshot、动态对象节点表现过滤
- [x] 连续 FollowPath revision 的第一轮修正
- [ ] 通用 Action Sequencer 与统一 `ActionOutcome`(Running/Completed/Cancelled/Invalid)
- [ ] 连续 Move:到达 A 的确认帧内完成 A → 激活 B → 用预求路线继续,不无条件停一帧
- [ ] 编队与群组移动:群组 identity、成员 ordinal、相对偏移、共享走廊、局部避让重规划
- [ ] Build / Containment / 技能接近产生的内部移动子动作
- [ ] 异步寻路的确定性屏障(未在屏障前完成则等待或确定性回退)
- [ ] 路径与动作序列架构冻结文档的验收场景覆盖(§19)

## 2. AI 与脚本系统

状态:**部分完成**

- [x] AI 订单准入与确认事务基础(`ObjectAIOrderAdmission`)
- [x] AI 状态族 SoA 存储(`AIStateFamilySoAStorage` / `ObjectAIShadowBatch`)
- [ ] AI 的 Enter / 技能等行为接入正式 containment/movement 后的动态回归
- [ ] 脚本运行时(script bridge / contracts / runtime)端到端接线
- [ ] 战役 / 遭遇战 AI 的确认订单与状态机衔接
- [ ] 脚本 AI 的语音 / 视频门禁回归验证(MOVIE_PLAY、natural completion)

## 3. 地形

状态:**部分完成**

- [x] 地形逻辑与地图高度场加载(`TerrainLogic` / `MapHeightfieldLoader`)
- [x] 地形可见性权威(`MapVisibilityAuthority`)
- [x] 地形建造查询(`TerrainConstructionQuery`)
- [ ] 地形相关未完成项的回归验证与缺陷修复
- [ ] 客户端地形对象持久化(`ClientTerrainObjectSaveGameCodec`)在存档体系下的完整性

## 4. 单位 / 武器 / 战斗

状态:**部分完成**(动作终态与目标判定待补全,详见架构冻结文档 §17)

- [x] `FIRE_WEAPON` 的 `MaxShotsToFire` 与 `shotsFired`
- [x] SpecialAbility 的 Unpacking / Preparing / Packing 运行时
- [ ] 通用动作终态审计:所有开放式与有限动作的完成条件
- [ ] `TargetFacts` 与 Weapon/Ability `Target Policy` 分层
- [ ] `FIRE_WEAPON` forcedWeaponSlot 延迟安装(排在 Move 后不提前切换武器槽)
- [ ] SpecialAbility 队列执行屏障(运行期间阻挡后续队列,直到 finish/cancel/invalid)
- [ ] 冷却边界统一:武器射击间隔、Clip 装填、`readyTick`、动作完成四者区分
- [ ] 已发射投射物的独立追踪 / 最后坐标 / 自毁策略,订单目标丢失不回滚
- [ ] 持续型技能(火墙)与有限技能(米格)的完成 / 取消审计
- [ ] Builder 本地规划与 confirmed Build 的最终边界

## 5. 网络

状态:**几乎未开始**(仅基础层,端到端未接线)

- [x] enet 传输层(`EnetGameTransport`)
- [x] 锁步帧缓冲与包编解码(`LockstepPacketCodec`)
- [ ] 完整多人会话流:创建 / 加入 / 房间与设置协商(MultiplayerData / GameSettings 未接线)
- [ ] 客户端输入 → 主机确认 → 广播的端到端链路
- [ ] 断线处理 / 重连 / 同步校正
- [ ] 多机确定性验证(同输入序列在两台机器一致推进)

## 6. 存档

状态:**几乎未开始**(仅存储层)

- [x] 存档存储层(`SaveGameStorage`)
- [ ] 完整游戏状态序列化:动作队列、Active Action identity、各域运行时(架构文档 §17 明确 **未覆盖**)
- [ ] 存档 / 读档 UI 流程接线(GameWndLayer 仅引用)
- [ ] 读档后的确定性恢复验证

## 7. 录像 / 回放

状态:**几乎未开始**(仅编解码层)

- [x] 回放文件编解码与存储(`ReplayFileCodec` / `ReplayStorage`),命令流捕获有代码
- [ ] 回放播放控制(播放 / 暂停 / 快进 / 拖动)
- [ ] 回放与存档共享确定性快照的完整覆盖(Active Action)
- [ ] 队友路径表现提取策略(按观察者可见性过滤目标)

---

## 依赖关系建议

网络、存档、录像三者共享同一地基:**确定性动作序列与已确认快照**(TODO #1/#4)。建议按架构冻结文档 §18 的顺序推进:先补 Action Sequencer 与终态审计,再上网络 / 存档 / 录像,避免在未稳定的推进语义上重复返工。
