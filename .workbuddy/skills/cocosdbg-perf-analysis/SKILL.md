---
name: cocosdbg-perf-analysis
description: 分析 Cocos2d DebugView 录制的 .cocosdbg 性能日志，并结合运行时 luac dump 的明文源码，定位 Cocos2d-x + Lua 5.1 游戏进程（如 sealapp）中 Lua 耗时严重的逻辑问题。当用户提供 cocosdbg_capture_*.cocosdbg 录制文件，或要求"查性能问题 / 分析卡顿 / 找 Lua 热点 / 分析性能日志"时使用本技能。
agent_created: true
---

# cocosdbg 性能问题分析

## 概述

对 DebugView 录制的 `.cocosdbg` 逐帧性能数据做离线聚合，交叉验证调用规律，
再对照 luac dump 的明文 Lua 源码定位逻辑反模式，产出带证据（数据 + 代码行号）
和修复优先级的分析报告。

## 前置检查

1. 确认录制文件路径（通常在 `build/cocosdbg_capture_YYYYMMDD_HHMMSS.cocosdbg`）。
2. 确认明文源码 dump 位置（磁盘上的 `.luac` 是加密容器，不可解密，只能用运行时 dump）：
   - `build/luac_dump/sealapp/` —— 完整 dump（含启动期加载的 app/scene 等核心文件）
   - `build/generated/luac_dump/sealapp/` —— 晚期会话 dump（缺核心文件）
   - 不确定时查两处的 `luac_dump_map.tsv`（chunk 名 → 明文路径）。
3. 运行环境（本机约束）：
   - Bash 不可用；PowerShell stdout 不回传 → 脚本输出一律 `*> out.txt` 重定向，
     `Out-File` 结果是 UTF-16，用 Read 读之前先
     `Get-Content x.txt | Set-Content -Encoding utf8 x_u8.txt` 转码。
   - Python 用托管版：`C:\Users\xuhao2\.workbuddy\binaries\python\versions\3.13.12\python.exe`。
   - 数百 MB 文件流式解析需几十秒，PowerShell 调用设 timeout ≥ 300000。

## 工作流

### 第 1 步：聚合统计

```
python scripts/analyze_capture.py <capture.cocosdbg> [report.txt]
```

产出：帧耗时分布（avg/p50/p95/p99/max）、帧率分桶、最差帧及其帧内热点、
按 self/调用次数/单帧尖刺排序的 Lua 方法榜、C++ 方法榜、行采样热点、按源文件汇总。

### 第 2 步：解读聚合报告，形成候选问题清单

> **先看 `lua_wall` 能不能用**：`lua_wall` 是"每帧所有线程忙区间求并集"，
> 而忙区间是**每个 hook 事件一个**（开行采样时 `exit_qpc = lp_now()`，相邻区间不合并），
> `LP_LOCAL_INTERVAL_CAP = 4096` 超限就**静默丢弃**。hook_events 常见的 5k–190k/帧远超此限，
> 于是 `lua_wall` 被压到 1.6–2.4ms 而与真实负载无关。**判据：`lua_cpu - lua_wall > 0`
> 当且仅当 `flags & 0x80`（INTERVAL_LIMITED）置位**；不带 0x80 的帧两者恒等、都可信。
> 结论：**衡量 Lua 占用用 `lua_cpu` / `self_ms`，不要用 `lua_wall`**；
> `r(lua_cpu, hook_events)` 可达 0.9 以上，别把它当成"Lua 越忙掉帧越多"的证据。
> （详见 references/format.md「lua_wall 截断」）

- 看 `lua_cpu` 的 p95 与均值差距 → 判断是"每帧均匀慢"还是"周期性尖刺"。
- **top by self_ms** → 每帧固定成本大户（轮询型逻辑）。
- **top by call count**（注意 avg_self_us）→ 调用压力：每帧几千次的小函数 + 高频
  string.format/tostring/tonumber 分配，通常是包装税或无索引查找。
- **top by max_self**（started 很小但 max 很大）→ 单次性尖刺：UI create、同步资源加载。
- **worst frames** → 尖刺帧的帧内 top 方法，直接指向卡顿瞬间在做什么。
- **行采样热点** → 拿到精确文件:行号。

### 第 2.5 步：调用方归因（不需要 trace 事件流）

`incl_ms` 来自 profiler 的影子栈，父子嵌套是**真实的**，所以即使 trace 未开也能重建调用链：

```
python scripts/caller_attribution.py <capture.cocosdbg> [report.txt] [body=25 | self=substr,ms]
```

核心是两个信号：

1. **inclusive 嵌套**：尖刺的真正调用方在尖刺帧里 inclusive 很大，且**包含**可疑叶子
   （如 `dump_` incl 93.4ms ⊃ `dump` incl 41.8ms ⊃ `msg_format_indent` incl 40.0ms）。
   闭区间嵌套 + 调用次数相等（如 `text_format:msg_format` 调用数 == `dump()` 调用数）
   就能把"谁调用了谁"钉死。
2. **稀有度**：统计方法在全部帧中"出现过"的帧数 `base`。**只在尖刺帧活跃**
   （`base == 尖刺帧数`，如 base=20/969）的方法即与尖刺同源；`base ≈ 总帧数` 的
   （`[C]:xpcall`、`Callback:_P_CALL`、`DataManager:update`）只是通用祖先，不是调用方。

两个信号交叉后，调用方归因从"静态推断"升级为"数据证实"。**仍然建议下次录制开启事件流**
（`trace_analysis.py`）做最终确认，但不再是不开就无能为力。

### 第 3 步：时序关联，识别周期性/调试型逻辑

```
python scripts/correlation_analysis.py <capture.cocosdbg> [report.txt] [key=子串 ...]
```

- 无参数时自动检测"嫌疑周期逻辑"：只在少数帧活跃但单帧成本高的方法
  （调试日志、批量处理的典型特征），并输出触发间隔直方图。
- **阈值坑**：自动检测的门槛是 `self >= 100ms`。Lua 总量小的录制（例如 18s 里 Lua 只 0.67s）
  会直接输出 `(none)`，此时**不代表没有周期逻辑**，要自己按倍率/间隔 CV 重扫。
  推荐的做法是扫全部方法的"相邻激活间隔的变异系数 CV"，CV 接近 0 的就是周期定时器
  （实测能一次列出 1.0s / 2.011s / 5.0s / 6.0s 四档）。
- 传 `key=子串`（方法名子串匹配）可追踪指定函数的每帧调用率与触发间隔，
  多个 key 之间输出同帧共现矩阵——两个调试函数 100% 同帧出现即可断定同一触发源。
- 典型案例：`dump_`（调试打印）与 `msg_format_indent`（protobuf 格式化）每 ~2s
  同帧触发 → 补丁框架的网络消息日志。

### 第 3.5 步：渲染侧归因（掉帧问题必做）

> 用 `scripts/spike_breakdown.py <capture> [out.txt] [thr_ms]` 一次跑完本步的全部计算。

Lua 常常**不是**掉帧主因（实测过 18.7s 录制里 Lua 只占 3.6%，掉帧全在渲染侧）。
在花时间挖 Lua 之前，先用这一步定性：

1. 算 `period` 与各分量的相关系数。`cc.Scene:render`（场景图遍历+节点绘制）相关系数
   ≈0.93 而 `lua_wall` ≈0.19，就可以直接判定掉帧与 Lua 无关，跳过第 5 步的 Lua 源码排查。
2. 拆 `drawscene_body`：`body - lua - Scene:render - Renderer:render` = 未插桩部分
   （C++ Scheduler / ActionManager 等），实测能占到 33%。
3. **看"遍历 / 提交"比值 `S/R = cc.Scene:render / cc.Renderer:render`（最有用的一条）**。
   `cc.Scene:render` 挂钩在真实导出 `?render@Scene@cocos2d@@`，函数体 = `visit()` 递归
   + 一次 `renderer->render()`；而 `cc.Renderer:render` 才是真正的 GL 提交。
   - 基线比值约 **2.2×**（sealapp 实测 SceneR 均值 4.5ms / RenderR 2.07ms）。
   - 尖刺帧若出现 **20–60×** 而 `cc.Renderer:render` 仍是 2ms 左右、调用次数也恒为 2，
     那就等于证明了"**绘制量没变，变慢的是遍历本身**"——这是**纯 CPU 逐节点工作**，
     既不是 Lua，也不是"节点变多了"（节点变多会让提交量同步上升）。
   - 此时要查的是 `visit()` 内的逐节点逻辑：`Label::updateContent()`（惰性文本排版，
     `setString` 只置 `_contentDirty`，真正的排版推迟到 visit，**这正好解释"Lua 侧完全看不出来"**）、
     角色类的 `visit()`/`draw()` 覆盖（骨骼姿态、挂点、换装）、`Node::sortAllChildren()`。
   - **零成本先行验证**：让玩家侧在尖刺帧用 Lua 递归统计 `node:getChildrenCount()`。
     节点数暴增 → 查创建/回收逻辑；节点数不变却慢 30–60× → 直接锁定"单节点内部工作"。
4. 看节流器行为：`body < 16.67ms → period ≈ 16.7`；`body > 16.67ms → period ≈ body`
   （实测两者差 0.1ms）。所以只有 body 突破 16.67ms 的帧才掉帧，基线再高也不掉。
5. 找尖刺列车：列出 `body >= 阈值` 的帧，算相邻时间差。**注意成对尖刺**：若一堆间隔在
   0.05–0.2s（即相邻 1–3 帧）而另一批在 1s 以上，那是"一次爆发跨了 2 帧"，不是两个节拍；
   只有 `gap > 0.5s` 的那批才参与节拍统计。间隔高度一致（CV<0.35）就是周期性任务，
   用长 lag（120–260）自相关交叉验证。工具的 `scripts/periodic_coverage.py` 会自动分开这两批。
   **把实测节拍与第 3 步列出的 Lua 定时器档位对比：对不上就说明触发源在 Lua 之外**
   （C++ schedule / 异步加载完成回调），不要再在 Lua 里找。

**尖刺成因判别表**（靠形态区分，不需要调用栈）：

| 形态 | 成因 |
|---|---|
| 单帧暴起、下一帧立即恢复，Lua 平坦 | 渲染资源首次使用的一次性开销：纹理上传 / shader 编译链接 / FBO 分配 |
| **`cc.Scene:render` 暴起但 `cc.Renderer:render` 平（S/R 20–60×），Lua 平坦，内存平坦** | **遍历变慢而非绘制变多**：`Label::updateContent` 惰性排版 / 角色类 `visit()` 覆盖 / `Node::sortAllChildren`（纯 CPU，见第 3.5 步第 3 条） |
| 尖刺后基线持续抬高 | 新建了一批常驻节点（UI、特效）没回收 |
| Lua 同步暴涨 | UI create / addSpriteFrames / plist 解析（见第 5 步） |
| Lua 单点 self 极高、base 只有几帧（<10） | 同步创建实体 / 首次加载（`[C]:createEntity`、`fullPathForFilename`） |
| 单帧内存大幅跳变 | 批量分配 / 资源加载；内存匀速增长且无跳变则可排除 GC 与批量分配 |

若判定为"渲染资源首次开销"但 Lua 侧找不到周期触发器（Lua 定时器档位对不上），
触发源通常在 **C++ 侧**（schedule / 异步加载完成回调），Lua profiler 看不见。
此时不要继续猜 Lua，改为建议给 `Texture2D::initWithImage`、`GLProgram::link`、
`RenderTexture::begin/end` 补 C++ 插桩计时来定案。

### 第 4 步：调用方归因（trace 事件流开着时用）

```
python scripts/trace_analysis.py <capture.cocosdbg> [report.txt] [topN]
```

- 从 trace 事件重建每线程调用栈，输出 topN 热点的调用方分布；
  按 method_id 拆开 `[C]:<anonymous>`。
- 若事件数为 0（默认录制常未开启），报告会提示；**改用第 2.5 步的
  `caller_attribution.py`**（incl 嵌套 + 稀有度）拿到调用链，并在最终报告里注明
  "调用方归因基于 incl 嵌套，建议下次录制开启事件流复核"。

### 第 5 步：对照源码定位逻辑问题

用第 2 步拿到的 `文件:行号` 到 dump 目录读代码，重点核查这些反模式：

| 反模式 | 信号 |
|---|---|
| 每帧轮询本应事件驱动的状态 | 方法 started ≈ 帧数 × 对象数，self 均匀 |
| 调用率比值暴露 miss 路径 | 如 getPanelName 次数 = getPanel × 2 → 几乎全走 miss 级联 |
| 查询函数内 string.format/拼接 | 高频调用 + format/tostring 调用数同量级 |
| 循环内循环不变量转换 | 循环体内 tonumber/tostring 基准值 |
| 无索引线性扫描 | O(n) 遍历 × 每次查询，包/列表数据源 |
| pairs 扫描静态配置表 | 每次(帧)重复展开本可缓存为位掩码/数组 |
| 调试函数未摘除 | dump/print/format 类函数周期性活跃（第 3 步检出）；**强判据：`functions.luac:61 dump_` self>1ms 且 base 帧数≈尖刺帧数，每次命中调用数完全恒定**（见下）|
| 同一查询一帧多次 | 同名 C 查询出现两次的条件+取值模式 |
| 主线程同步资源加载 | fullPathForFilename/addSpriteFrames/getSpriteFrameByID 尖刺 |
| Lua 包装原生函数 | 全局 tonumber/getServerTime 之类被重定义成 Lua 函数 |

### 第 6 步：产出报告

写入 markdown 报告（建议放 `build/generated/`），结构：

1. 总体概况（帧率分桶、Lua vs 渲染占比）
2. 问题清单按影响排序，每条含：**证据**（数据 + 交叉验证结论）、**代码位置**
   （dump 相对路径:行号）、**修复建议**
3. 修复优先级路线表（动作 → 预期收益）
4. 方法说明与局限（事件流是否开启、调用方归因是数据还是推断）

报告必须区分"数据证实的"与"静态推断的"结论；推断项注明验证方法。

## 资源

- `scripts/cocosdbg_reader.py` —— 共享流式解析器（格式 v5），其余脚本依赖它
- `scripts/analyze_capture.py` —— 聚合统计（第 1 步）
- `scripts/caller_attribution.py` —— **trace 未开时用 incl 嵌套 + 稀有度做调用方归因（第 2.5 步）**
- `scripts/spike_breakdown.py` —— **渲染侧分量拆解 + 分类 + 遍历/提交比（第 3.5 步）；判断"该不该继续挖 Lua"的第一站**
- `scripts/periodic_coverage.py` —— **尖刺节拍统计 + Lua 定时器档位对照 + 覆盖率反查（第 3.5 步第 5 条、第 3 步）**
- `scripts/correlation_analysis.py` —— 周期逻辑检测 + watch 共现分析（第 3 步）
- `scripts/trace_analysis.py` —— trace 事件调用栈归因（第 4 步）
- `references/format.md` —— 二进制格式规范、方法名与源码行号映射规则、
  加密 .luac 与 dump 目录说明、本机环境坑
- `references/known-cases-sealapp.md` —— sealapp 上已定案的尖刺模式与判据速查

脚本均只用 Python 标准库；从任意 cwd 运行均可（自动把自身目录加入 sys.path）。
