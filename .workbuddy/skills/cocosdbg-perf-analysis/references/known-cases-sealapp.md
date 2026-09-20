# sealapp 已定案尖刺模式速查

对 `sealapp` 进程做分析时，先拿这张表对号入座。每条都注明**判据**（数据特征）
与**证据等级**（数据证实 / 推断），避免重新从数据猜、也避免把不同录制的问题混为一谈。

---

## C1. 队伍邀请列表的残留 `dump()` —— 阶段性尖刺头号成因

> **状态：已于 2026-09-18 修复并在 173501 录制中验证消除。**
> 验证判据：`dump_` / `dump` / `msg_format_indent` / `print_field` / `[C]:print` /
> `[C]:traceback` / `[C]:sort` 以及整个 `UITeamInvite*` 家族在 173501 里**零命中**；
> 同阈值（body≥20ms）下 dump 链尖刺从 20 帧降到 0，问题帧占比 3.20% → 1.85%。
> 后续录制若再次出现这些方法，说明是**新引入**的同类残留，按本节判据重查。
> （本节内容保留作为"残留调试打印"这一类问题的判据模板。）

**判据（数据特征）**

- `[string "cocos/cocos2d/functions.luac"]:61:Lua:dump_` 的 `self_ms > 1ms` 只在少数帧出现，
  且**这些帧的 `base`（该帧内其他方法在全部帧中的活跃帧数）里 `dump_` 的 base ≈ 尖刺帧数**
  （例如 20/969）→ `dump_` 只与尖刺同源。
- 每次命中**调用数完全恒定**：12 次 `functions.luac:52 dump`、223 次 `:61 dump_`、
  126 次 `text_format.luac:42 msg_format_indent`、474 次 `text_format:44 print_field`、
  1200 次 `text_format:73 write`、2833 次 `[C]:tostring`、12 次 `[C]:traceback`、259 次 `[C]:print`。
  恒定性说明打印的是同一种固定结构（N 个队员 × 3 次 dump）。
- `[C]:tostring` 的 inclusive **包含** `text_format.luac:71 msg_format` 的 inclusive，
  且两者调用数相等（12）→ 被 dump 的值里有 protobuf message，其 `__tostring`
  展开成整段 text_format 文本（invariant 见 [推断] 一节）。

**根因与位置**

- 打印点：`app/gui/UITeamInviteList.lua:354 dump(mem)`、`:357 dump(g_GameData:getMyCountry())`、
  `:363 dump(color)`（都在 `:345 refreshChar` 内）。同函数 346/353/356 行的
  `print("yx -- ...")` 已被注释 → 典型的"清 print 漏删 dump"。
- 触发源 1（定时器，约 11/20 帧）：`app/gui/UITeamInvite.lua:26`
  `self:registerTickEvent("lowUpdate",1,handler(self,self.onMemUpdate))`
  → `UIPanel:registerTickEvent`(`UIPanel.lua:711`) → `GameData:addTickEvent`(`GameData.lua:729`)
  → `GameData:update`(`:794-818`) 按 serverTime 每秒回调 → `onMemUpdate`(`UITeamInvite.lua:93`)
  → `refreshAllMem`(`:139`)。
- 触发源 2（网络消息，约 8/20 帧）：`DataManager:dispatchMsg`(`DataManager.lua:367`)
  → `TeamsData.luac:1831` → `UITaskTraceDlg.luac:1536 refreshTeamMem` → `refreshAllMem`。
- 两者叠加 ⇒ 间隔 CV ≈ 0.85，**不是固定周期**，所以表现为"阶段性"而非规则抖动。
  想找固定周期定时器（第 3 步的 CV 扫描）在本案会落空，别据此排除。

**为什么贵**

`cocos/cocos2d/functions.lua:52-107` 的 `dump()`：`debug.traceback`（每次一次，
实测 ~110µs）+ 每个键一次 `string.format`/`tostring` + 带 `tostring` 比较器的 `table.sort`
+ 每行一次 `print` + 建 keys/values 两张临时表。

**量级（164046 录制实测）**：20/969 帧（2.1%）命中，占全部掉帧（40 帧）的 50%、
`body>=25ms`（29 帧）的 20 帧；每帧多烧 24.1ms body；单帧 `refreshChar` incl 12–43ms；
合计 +481ms body / +443.6ms lua_cpu。

**修法**：删三行 `dump()`（唯一必需改动）；`refreshAllMem` 从 1s 全量 tick 改脏标记驱动。

**次生开销**（删 dump 后仍存在）：`Config/MapConfig.luac:5924 getMapNameByMapId`
（O(n) 扫配置表，4 次/帧 × 0.57–1.13ms）由 `UITeamInviteList:94 updateMapInfo` 调起。

---

## C2. `PackageData` 线性扫描 + 被 patch 的全局 `tonumber` —— 单帧 70–90ms

**判据**：`app/data/PackageData.luac:394 getObjectThisIDByBaseID` 单帧 114 次调用
（self 6.7–7.5ms）、`:922 GetObjectNumByBaseId` 12 次；同帧
`E:/YZST/config/Patch/src/main.luac:266:Lua:tonumber` 9400+ 次（self 5–6ms），
`[C]:lua_old_tonumber` 与之等次数。行采样热点固定在 `main.luac:267`/`:275`
（patch 的 tonumber 包装体）与 `PackageData.luac:396`。

**调用链**：`CountryData.luac:726` → `PackageData:394`。
**修法**：`baseId → objectIndex` 建索引；调用方 `local tonumber = tonumber` 取原始函数。

---

## C3. drawScene 内未插桩的 C++ 开销 —— Lua 平坦的单帧暴起

**判据**：`drawscene_body` 21–43ms，但帧内 `lua_cpu` 仅 2.3–4.8ms、
`cc.Scene:render` 3.1–4.7ms、`cc.Renderer:render` 1.8–2.8ms → 17–33ms 无归属；
且 `hook_events` 低于均值（≈5000 vs 6855）→ **可排除 profiler 自身开销**。
形态为"单帧或连续数帧暴起、下一帧立即恢复基线"。

**口径提醒**：`drawscene_body_ms` 确认为 drawScene 钩子体本身
（`injected_runtime.c:877-885`，`body_ms = end_qpc - g_frame.begin_qpc`，不含 present/vsync），
所以这些时间确实花在 drawScene 内部，不是 vsync 假象。

**未定案，需补 C++ 插桩**（按序）：`Texture2D::initWithImage` / `TextureCache::addImage`
→ `GLProgram::link` / `GLProgramCache::getOrCreateWithGLProgramName`
→ `RenderTexture::begin/end` → `SpriteFrameCache::addSpriteFramesWithFile`。

---

## C4. 误导性关联：`cc.Scene:render` 的相关系数

同一批数据里，`r(period, cc.Scene:render)` 可以为 **+0.076**（164046 录制，掉帧在 Lua 侧）
也可以为 **+0.931**（161318 录制，掉帧在渲染侧）。**必须先算相关系数再决定往哪挖**，
不要凭"游戏卡顿多半是渲染"的先验直接跳到第 3.5 步，也不要因为一次录制是渲染侧
就认定所有录制都是渲染侧。三条录制的形态对比：

| 录制 | 掉帧主因 | 判据 |
|---|---|---|
| 152818 | 基线就重 + Lua 也爆 | body p50 15.7ms、max 1146ms、43 次 >25ms、多次 Lua 主导 |
| 161318 | 渲染侧（≈3.1s 周期，未定案） | `r(period, Scene:render)=+0.931`，Lua 仅占 3.6%，帧内 Lua 完全平坦 |
| 164046 | Lua 侧残留 dump() | `r(period, body)=+0.931` 但 `r(period, Scene:render)=+0.076`；50% 掉帧含 dump 链 |
| 173501 | 渲染侧遍历（1.32s 节拍，未定案）+ 6 帧 Lua 型 | `r(period, Scene:render)=+0.979`、`r(period,lua_cpu)=+0.263`；S/R 比 20–60× |

**判据模板**：先看 `r(period, lua_cpu)` 与 `r(period, cc.Scene:render)` 谁大 &gt;0.8；
两者都低才去看未插桩部分。用 `scripts/spike_breakdown.py` 一次算完。

---

## C5. `cc.Scene:render` 遍历尖刺 —— 绘制量没变、遍历变慢（本轮最大，未定案）

**判据（决定性的一条是"遍历 / 提交"比值）**

- 尖刺帧 `cc.Scene:render` 56–136ms，而 `cc.Renderer:render` 只有 1.8–4.6ms。
- 基线 **S/R = 2.2×**（SceneR 4.5ms / RenderR 2.07ms）；尖刺帧 S/R = **20–60×**。
- `cc.Renderer:render` 的**调用次数恒为 2**（正常帧也是 2）→ 渲染趟数没变。
- 帧内 `lua_cpu` 仅 1.9–7.5ms；`lua_mem` 平坦（±0–2MB）；`hook_events` 正常偏低。
- **"出现在尖刺帧 ±2 帧内的稀有 Lua 方法"覆盖率最高只有 3/27** —— 不存在稳定的 Lua 触发器。
  此时 `caller_attribution.py` 的 B 段（稀有度过滤）会输出 `(none)`，**这是正常的，别据此说
  "没有成因"**，要改用 `periodic_coverage.py` 的覆盖率视角复核。
- 尖刺间隔中位数 1.321s（16 个 >0.5s 间隔，CV 小），另有 10 次成对出现（间隔均值 0.143s，
  即"一次爆发跨 2 帧"）。Lua 侧定时器档位只有 1.00/1.02/1.07/1.99/0.53s，**对不上**。

**推论（为什么这是纯 CPU 遍历问题）**

`cc.Scene:render` 挂钩在真实导出 `?render@Scene@cocos2d@@UAEXPAVRenderer@2@@Z`，
函数体 = `visit()` 递归 + 一次 `renderer->render()`。若尖刺是"同屏节点变多"，提交量必然
同比例上升；实测提交量不变 ⇒ **变慢的是遍历本身，且是纯 CPU 逐节点工作**。

**推断候选**（按"纯 CPU、不增 GL 提交、可被单帧批量触发"筛出，尚未直接观测）

| 候选 | 为什么符合 |
|---|---|
| `Label::updateContent()` | `setString` 只置 `_contentDirty`，真正的排版/字形栅格化推迟到 `visit()` —— **这正好解释"Lua 侧完全看不出来"** |
| 角色类 `visit()`/`draw()` 覆盖 | 角色是 C++ 对象（`sealp.cCharacterExt`），骨骼姿态/挂点/换装若写在自身 visit 里就对 Lua 不可见 |
| `Node::sortAllChildren()` | 纯 CPU 排序，只在 `_reorderChildDirty` 时触发，不改提交量 |

**零成本先行验证（推荐先做这个，再决定插不插桩）**

在尖刺帧用 Lua 递归统计场景节点总数：`node:getChildrenCount()` 逐层累加后打印。
- 节点数**暴增** → 回到创建/回收逻辑（见 C6）。
- 节点数**不变**却慢 30–60× → 锁定"单节点内部的逐节点工作"，此时再插桩可一次命中。

**待插桩点**：`Label::updateContent` / `Label::visit`、`sealp.cCharacterExt` 的 visit/draw 入口、
`Node::sortAllChildren`。

**量级（173501 实测）**：27/35 个 >20ms 帧（77%），其中 6 帧 body >100ms；
27 帧 body 合计 2031.7ms，Scene:render 合计 1860.2ms（91.6%），
超基线部分 ≈1.74s（占 33.29s 录制的 5.2%）。

---

## C6. 角色实体首次创建同步加载 —— 单次 11.5–45ms

**判据**

- `[C]:createEntity` `max_self` 高（45.01ms）而 `started` 极小（全录制只有 **9 次**）→
  典型"单点尖刺"，不是高频分摊。
- 同帧调用链（`incl` 闭区间嵌套，实测）：
  `GameScene.luac:120 update` → `SceneManager.luac:1336 update` →
  `SceneNpc.luac:259 refreshPresentation` → `:331 createEntity` → `[C]:createEntity`。
  seq=3265 实测：45.94 ⊇ 45.82 ⊇ 45.01。
- **单帧只调一次也能烧 45ms**（`calls=1`）→ 个数类预算挡不住，必须降单次成本。

**根因**：`app/scene/SceneNpc.lua:331 M:createEntity` 里
`self.m_character = sealp.cCharacterExt:create()`
然后 `self.m_character:createEntity(objType, thisId, bodyid, headid, weaponl, weaponr,
hairColor, coat, shadow, cloakId, ...)`（NPC 分支在 `:399`，HumanNPC 分支在 `:390`）。
C++ 侧按整套造型参数（体型/头/武器/披风/骑乘）同步装配，各部件贴图与骨骼**首次使用时
同步加载**，不同造型不共享缓存 ⇒ 单价 11.5–45ms（差 4 倍 = 首次部件数不同）。
`refreshPresentation` 在 `g_GameScene:getSceneLoaded()` 后逐实体创建，同帧多个就串行累加。

**已有机制（别重复发明）**：`SceneManager` 的创建循环用
`g_GlobalConf.MaxEntityCreatePerFrame` 卡每帧**个数**
（`SceneManager.lua:1405 / 1442 / 1464 / 1503`；掉落用 `MaxEntityDropCreatePerFrame`；
Pet 若是 `SceneDropItem` 时放宽到 `×3`，见 `:1498`）。
`SceneManager.lua:2343-2355` 有一段被整块注释掉的「分帧创建」循环，说明这块改过一轮。

**修法**：① 造型资源预热；② 预算由"个数"改"耗时"（每帧给创建留 4ms）；
③ 屏外实体延后创建（`NpcScreenManager` 已有 detach 概念）；④ 部件级缓存池。

---

## C7. FXManager 按需加载 shader —— 单次 10.5ms

**判据**：帧内出现
`FXManager.luac:45 apply → :154 createState → :568 loadProgram → :558 _shaderFileExists ×2`
→ `[C]:fullPathForFilename self 10.49ms`（全录制只 34 次调用、单次最高 10.49ms）。
`FXManager.luac:572-573` 对 vs/fs 各做一次存在性检查。

**修法**：`_shaderFileExists` 结果按文件名缓存（进程内不变）；shader 预编译。
`loadProgram` 调用点分散在 `GameScene:2535`、`SceneDynamicObj:894`、`UILogin`、
`UICreateRole2:2280` 等十余处，建议收敛成按 key 缓存的统一入口。

---

## C8. 可交互 NPC UI 与 `getServerTime` —— 基线固定成本

**判据**：`UIInterctiveNpc.luac:61 Update` / `:73` / `:86` / `:152` 与
`InteraCtiveNpc.luac:44/48/79` 只在 259/1893 帧活跃，却覆盖 26/27 个渲染尖刺（96%）。
这是**覆盖率视角**（`periodic_coverage.py` 的 C 段）才能看到的信号，
稀有度视角看不到。注意区分：这些是"场景内容相关"的伴生信号，不等于成因本身。

`[C]:getServerUTCTime` / `Util.luac:306 getServerTime` 每帧各 12.2 次、
单次 3.4–4.9µs，合计约 0.10ms/帧。

**行采样第一名**：`UIFolderData.luac` 479 samples + `UIManager.luac` 285 ≈ 全量 44.5%，
其中 `getPanelName`(`:657-665`) 单独 416、`getPanel`(`:632-661`) 约 302。
miss 级联判据仍然成立：`getPanelName` 243675 次 ≈ **1.96 ×** `getPanel` 124141 次。

**C 调用占比**：`lua_c_self 0.967 / lua_cpu 2.850 = 33.9%` —— 三分之一的 Lua 时间在 C 调用里；
大头是 `[C]:<anonymous>` 646.6 次/帧、`(for generator)` 311.2、`type` 176.6、`format` 129.0，
单次 0.2–0.6µs 但合计 0.40ms/帧。

---

## [推断] protobuf message 的 `__tostring` 触发 text_format

**依据**：`[C]:tostring`(incl) ⊇ `text_format.luac:71:format`(incl)，两者调用数相等（12）；
`protobuf.luac:604:Lua:<anonymous>` 夹在二者之间（20 帧累计 incl 164.05ms）。
**无法直接阅读**：`protobuf.lua` 不在 `build/luac_dump/sealapp/` 也不在
`build/generated/luac_dump/sealapp/` 中（未被 dump），`protobuf.luac:604` 源码拿不到。
**验证方法**：在 `dump()` 里临时加 `if type(v)=="userdata" then print(debug.traceback()) end`；
或下次录制开启 trace 事件流后用 `trace_analysis.py` 直接读 `text_format` 的调用方。
