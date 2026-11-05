---
name: cocosdbg-perf-analysis
description: Analyzes DebugView-exported .cocosdbg recordings of Cocos2d-x + Lua 5.1 games (sealapp / 原始征途), aggregates per-frame Lua/C/C++ cost, and locates hot logic in the decrypted tree E:/YZST_lua_decrypted. Use when the user provides cocosdbg_capture_*.cocosdbg, asks to profile a capture, find Lua hotspots, explain frame drops, or asks how to decrypt YSZT luac from E:/YZST.
---

# cocosdbg 性能问题分析

对 DebugView 导出的 `.cocosdbg` 做离线聚合，交叉验证调用规律，再对照
`E:/YZST_lua_decrypted` 的明文定位逻辑反模式。
报告须含**期间性能波动时间线**，并用 `[问题Lua] 文件:行` 直接标记问题源；
带证据与修复优先级。

脚本只用 Python 标准库。从**仓库根**运行，报告写入 `build/generated/`
（大文件不要贴进聊天）。用 PATH 上的 `python`。

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/<script>.py <capture.cocosdbg> build/generated/<stem>_<tag>.txt
```

## 前置检查

1. 录制路径：通常 `build/cocosdbg_capture_YYYYMMDD_HHMMSS.cocosdbg`。
2. 源码按这个顺序找，行号用 profiler 的 `文件:行`：
   - `E:/YZST_lua_decrypted/` —— 已解密明文。先读这里。`src/` 是逻辑，`res/` 是 UI。
   - `E:/YZST` 里的 `.luac` 是加密容器，不要当文本打开。更新明文时跑解密脚本，见 [references/yszt-source.md](references/yszt-source.md)。
3. 数百 MB 流式解析可能几十秒，Shell `block_until_ms` ≥ 300000。

## 占用指标（先读）

新内核由构造保证：

**`lua_cpu_ms` = `lua_script_self_ms` + `lua_c_self_ms`**

| 量 | 含义 |
|---|---|
| Lua 总耗时 `lua_cpu_ms` | 窗口内脚本 self + C self。衡量占用用这个 |
| Lua 脚本 | 影子栈顶是 Lua / main |
| C | 从 Lua 调入的 C 函数，加上 `lua_pcall`/`lua_call`/`lua_resume` 空栈 VM（合成方法 `[C]:<vm>`） |
| `lua_wall_ms` | 多线程忙区间**并集**；单线程新录制应 `wall ≈ cpu`。不要当总耗时 |
| `lua_outside_ms` | 窗口外 Lua，不进总耗时 |
| C++ hook | `drawScene` / `Scene:render` / `Renderer:render` 及 Scheduler/贴图等子桩 **不进**恒等式 |

**旧录制**：若大量帧 `flags & 0x80`（`INTERVAL_LIMITED`），忙区间曾按每个 hook 切开并在 4096 条处截断，`lua_wall` 会严重偏小。那种文件**只用 `lua_cpu` / method self**。新内核不应再被 hook 密度打满 0x80。

方法表校验：`sum(kind≠C self) ≈ script`，`sum(kind=C self) ≈ c`（含 `[C]:<vm>`）。

细节见 [references/format.md](references/format.md)。sealapp 已定案模式见 [references/known-cases-sealapp.md](references/known-cases-sealapp.md)。

## 工作流

### 第 1 步：聚合统计

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/analyze_capture.py <capture.cocosdbg> build/generated/<stem>_analyze.txt
```

产出：恒等式摘要、帧耗时分布、帧率分桶、最差帧（按 `lua_cpu`）、Lua/C 方法榜、C++ 榜、行热点（v6 按 `line_ms`）。

### 第 2 步：解读聚合，形成候选清单

- 看 `lua_cpu` 的 p95 与均值 → 均匀慢还是周期性尖刺。
- **top by self_ms** → 每帧固定成本（轮询）。
- **top by call count**（看 avg_self_us）→ 调用压力：高频小函数 / format / tostring。
- **top by max_self**（started 很小但 max 很大）→ 单次尖刺：UI create、同步加载。
- **worst frames** → 尖刺瞬间在做什么。
- **行热点** → 文件:行号。v6 按 `line_ms` 排，不要按 `samples`（C 调用期间指令计数冻结）。

### 第 2.5 步：调用方归因（不需要 trace）

`incl_ms` 来自影子栈，父子嵌套是真实的：

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/caller_attribution.py <capture.cocosdbg> build/generated/<stem>_caller.txt [body=25 | self=substr,ms]
```

1. **inclusive 嵌套**：尖刺帧里 incl 很大且包含可疑叶子。
2. **稀有度**：`base` ≈ 尖刺帧数 → 与尖刺同源；`base ≈ 总帧数` → 通用祖先，不是调用方。

仍建议下次录制开事件流，用第 4 步复核。

### 第 3 步：时序关联，识别周期/调试逻辑

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/correlation_analysis.py <capture.cocosdbg> build/generated/<stem>_corr.txt [key=子串 ...]
```

- 无参数时自动检测「少数帧活跃、单帧成本高」的方法。门槛 `self >= 100ms`；Lua 总量很小时会 `(none)`，此时按相邻激活间隔 CV 重扫（CV≈0 即定时器）。
- `key=` 子串不要含 PowerShell 会吃掉的引号；用 `:61:Lua:dump_` 这类片段。
- 多 key 输出同帧共现。

### 第 3.5 步：渲染侧归因（掉帧必做）

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/spike_breakdown.py <capture.cocosdbg> build/generated/<stem>_spikes.txt [thr_ms]
python .cursor/skills/cocosdbg-perf-analysis/scripts/periodic_coverage.py <capture.cocosdbg> build/generated/<stem>_periodic.txt [thr_ms]
```

Lua 常常不是掉帧主因。花时间挖 Lua 之前先定性：

1. 算 `period` 与各分量相关。`r(period, cc.Scene:render)` 很高而 `r(period, lua_cpu)` 很低 → 掉帧与 Lua 无关，跳过第 5 步。
2. 拆 `drawscene_body`：`body - lua_cpu - Scene:render - Renderer:render` = 未插桩（Scheduler / ActionManager 等）。
3. **S/R = Scene:render / Renderer:render**。基线约 2.2×；尖刺若 20–60× 且 Renderer 调用次数不变 → 遍历变慢而非绘制变多（`Label::updateContent`、角色 `visit()`、`sortAllChildren`）。
4. 只有 body 突破 vsync 预算（约 16.67ms）才掉帧。
5. 尖刺列车：相邻 0.05–0.2s 是一次爆发跨 2 帧；`gap > 0.5s` 才参与节拍。节拍对不上 Lua 定时器档位 → 触发源在 Lua 之外。

| 形态 | 成因 |
|---|---|
| 单帧暴起、下一帧恢复，Lua 平坦 | 纹理上传 / shader 链接 / FBO |
| Scene:render 暴起、Renderer 平（S/R 20–60×），Lua/内存平坦 | 遍历变慢，不是节点变多 |
| 尖刺后基线持续抬高 | 常驻节点没回收 |
| Lua 同步暴涨 | UI create / addSpriteFrames |
| 单点 C self 极高、base 几帧 | `createEntity` / `fullPathForFilename` |
| 单帧内存大幅跳变 | 批量分配 / 资源加载 |

### 第 4 步：trace 事件流（开着时用）

```
python .cursor/skills/cocosdbg-perf-analysis/scripts/trace_analysis.py <capture.cocosdbg> build/generated/<stem>_trace.txt [topN]
```

event_count 全 0 时改用第 2.5 步，并在报告里注明「调用方基于 incl 嵌套」。

### 第 5 步：对照解密源码

用第 2 步的 `文件:行号` 读 `E:/YZST_lua_decrypted` 里的 `.lua`。路径映射、游戏目录和解密命令见 [references/yszt-source.md](references/yszt-source.md)。dump 里没有该文件时不要停在「源码未对照」；先按映射去解密树。重点：

| 反模式 | 信号 |
|---|---|
| 每帧轮询本应事件驱动的状态 | started ≈ 帧数 × 对象数，self 均匀 |
| miss 路径 | 如 getPanelName 次数 ≈ getPanel × 2 |
| 查询里 string.format/拼接 | 高频 + format/tostring 同量级 |
| 循环内不变量转换 | 循环体 tonumber/tostring |
| 无索引线性扫描 | O(n) × 每次查询 |
| pairs 扫静态配置 | 每帧重复展开 |
| 调试函数未摘除 | dump/print/format 周期活跃；`dump_` self>1ms 且 base≈尖刺帧数 |
| 主线程同步加载 | fullPathForFilename / addSpriteFrames 尖刺 |
| Lua 包装原生函数 | 全局 tonumber 被重定义 |

### 第 6 步：产出报告

写入 `build/generated/capture_analysis_<stem>.md`。报告必须让读者**一眼看出录制期间何时抖、抖成什么样**，以及**哪些 Lua 是问题源**（带文件:行号，不是只写方法榜名）。

区分「数据证实」与「静态推断」；推断项写验证方法。

#### 强制结构（按序，勿省略）

**§0 元信息**（短列表）

- 录制路径、时长、帧数、脚本输出前缀、解密树（`E:/YZST_lua_decrypted`）或 dump 目录、归因方式（trace / incl 嵌套）

**§1 总体概况**

- 恒等式、flags（尤其 `METHOD_LIMITED` / `INTERVAL_LIMITED`）
- 分位数表：`period` / `body` / `lua_cpu` / `lua_script` / `lua_c`
- body 拆分均值与占比；`r(period, …)` 定性（Lua / 渲染 / 混因）
- 帧率分桶（&lt;16.7 / 16.7–20 / ≥20 / &gt;33）

**§2 期间性能波动（必写，放在问题清单之前）**

目的：把「整段录制怎么抖」画清楚，避免只堆静态榜。至少包含：

1. **基线 vs 异常门槛**  
   用 p50/p95 定义基线；明确掉帧/尖刺门槛（常用 `body≥20` 或 `period≥20`，并写帧数与占比）。

2. **时间线表**（按墙钟 `t=` 排序，覆盖主要尖刺与尖刺列车）  

   | t(s) | seq | period | body | lua_cpu | SceneR | 形态标签 | 一句话原因 |
   |---|---|---|---|---|---|---|---|

   - 形态标签从：`单帧暴起` / `尖刺列车` / `基线抬高` / `周期抖动` / `打开卡顿` / `Lua驱动` / `渲染驱动` / `混因` 中选。
   - 相邻 0.05–0.2s 的多帧合并成**一条列车**写一行（注明跨几帧、峰值），不要拆成碎条目淹没时间线。
   - `gap>0.5s` 的尖刺各自成行。大录制可只列 top N（建议 ≥12 或覆盖全部 body 前 1%），但**必须覆盖**最大 body、最大 lua_cpu、最大 SceneR。

3. **波动摘要**（3–6 条 bullet）  
   例如：何时打开某 UI 拉高；是否存在约 Ns 周期；基线 body 是否贴 vsync；Lua 平坦时是否仍有渲染尖刺。有 `periodic_coverage` / `correlation` 结果时写上节拍与 CV。

4. **可选：分时段对比**  
   若录制明显分段（进图前/后、开战前后），用两段 avg/p95 对照，标出抬升来自哪一段。

**§3 问题清单（按影响排序）**

每条问题用 `### P#.`，且**必须直接标记问题 Lua**（渲染/C++ 主因也要写「同帧可见的 Lua，非主因」以免误挖）。

每条固定四块：

1. **标记行（标题下第一行，强制）**  
   - 问题 Lua：`` `[问题Lua] path/File.lua:行 函数名 — 一句话症状` ``  
     可多行；每条一个文件:行号。行号来自 dump 明文，与 profiler `NNN` 对齐。  
   - 若主因不是 Lua：`` `[非Lua主因] …` ``，另起一行写同帧相关 Lua（可标 `[伴生Lua]`）。  
   - 证据等级：`` `[数据证实]` `` 或 `` `[推断]` ``。

2. **证据** — 帧 seq/t、self/incl/calls、S/R、共现、稀有度 base 等数字。

3. **代码位置** — dump 路径 + 关键关键片段说明（反模式表归类）；链用 `A ⊃ B ⊃ C`。

4. **修复建议** — 可执行改动；推断项附**验证方法**。

同类小尖刺可收进一张「已定位单点」表，但表内仍须有 **Lua 文件:行号** 列，不得只写 C 符号。

**§4 问题 Lua 速查表（必写）**

在优先级之前放一张总表，方便扫读与跳转 dump：

| 标记 | 文件:行 | 函数 | 影响 | 录制信号 | P# |
|---|---|---|---|---|---|
| 问题Lua / 伴生Lua | `app/…/X.lua:123` | `foo` | 每帧 / 尖刺 / 打开 | 次数、self ms、活跃帧 | P3 |

只列入本报告认定的项；基线噪音（若仍建议修）可标 `基线` 影响。

**§5 修复优先级**

| 优先级 | 动作（点名文件:行或系统） | 预期收益（ms 或掉帧帧数） |
|---|---|---|

**§6 局限**

事件流是否开启、METHOD_LIMITED 对榜的影响、wall 是否可用、哪些是推断。

## 资源

- `scripts/cocosdbg_reader.py` —— 流式解析器（v5/v6）
- `scripts/analyze_capture.py` —— 第 1 步
- `scripts/caller_attribution.py` —— 第 2.5 步
- `scripts/spike_breakdown.py` —— 第 3.5 步
- `scripts/periodic_coverage.py` —— 尖刺节拍 + Lua 定时器对照
- `scripts/correlation_analysis.py` —— 第 3 步
- `scripts/trace_analysis.py` —— 第 4 步
- [references/format.md](references/format.md) —— 二进制布局与指标
- [references/known-cases-sealapp.md](references/known-cases-sealapp.md) —— sealapp 判据速查
- [references/yszt-source.md](references/yszt-source.md) —— `E:/YZST`、解密树、luac 解密与路径映射
