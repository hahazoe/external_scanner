# .cocosdbg 录制格式参考（version 5 与 6）

格式权威来源：`debugview/recording_io.cpp`（kMagic `CCDBREC1`）。全部小端序。
当前写出的版本是 **6**；读取端必须同时接受 5 和 6（v5 文件里 v6 的新字段视为 0）。

## 文件布局

```
magic     8 bytes  "CCDBREC1"
version   u32      = 5 或 6
count     u64      帧数
frames    count × Frame
img_cnt   u32      预览图数量（可忽略，分析时读到帧结束即可）
images    img_cnt × Image
```

**version 6 相对 5 的两处差异**（都在中间插入，不追加在末尾 —— 解析器按版本分支，
读错了整帧会错位）：
1. Frame 头在 `lua_c_self_ms` 之后插入 `lua_outside_ms(f32)`。
2. 行采样在 `samples(u32)` 之后追加 `line_ms(f32)`。

字符串统一为 `u32 长度 + 字节`（UTF-8，最长 1MB）。

## Frame 布局（按序）

| 字段 | 类型 | 含义 |
|---|---|---|
| publish_sequence | u64 | 帧序号 |
| frame_begin/end_time_seconds | f64 ×2 | 帧起止时间 |
| lua_frame_index | u32 | |
| render_frame_sequence | u32 | |
| frame_period_ms | f32 | 帧周期 |
| drawscene_body_ms | f32 | drawScene 钩子体内耗时 |
| lua_wall_ms | f32 | 本帧 Lua 墙钟 |
| lua_cpu_ms | f32 | 本帧所有 Lua 线程 CPU 累计（可 > wall）|
| lua_script_self_ms | f32 | Lua 脚本自耗 |
| lua_c_self_ms | f32 | C 函数自耗 |
| lua_outside_ms | f32 | **仅 v6**：窗口外 Lua —— 上一帧结束到本帧窗口打开之间的 Lua 墙钟。不进 lua_cpu_ms、不归属任何方法 |
| lua_mem_kb | f32 | Lua 堆 KB |
| hook_events | u32 | |
| flags | u32 | |
| lua_count | u32 | LuaMethod 数 |
| cpp_count | u32 | CppMethod 数 |
| line_count | u32 | 行采样数 |
| event_count | u32 | trace 事件数 |

### LuaMethod（每条）

`name(str) kind(u32) flags(u32) self_ms(f32) incl_ms(f32) max_self_ms(f32)
max_incl_ms(f32) calls_started(u32) calls_completed(u32) active_at_end(u32)`

- 数值均为**本帧内**的增量；跨帧聚合直接求和。
- `self_ms` = 函数自身耗时；`incl_ms` = 含被调函数。递归函数的 incl 求和会重复计数，只作参考。
- `name` 形如 `[string "app/scene/SceneNpc.luac"]:947:Lua:run`（Lua）或 `[C]:format`（C）；
  `Lua:`/`C:` 前缀后的 `kind` 字段同样区分。C 函数经变量调用时 getinfo 拿不到名字，显示 `[C]:<anonymous>`。

### CppMethod

`name(str) total_ms(f32) max_ms(f32) last_ms(f32) calls(u32) call_sequence(u32)`

### 行采样

`source(str) function_name(str) line(u32) samples(u32) [line_ms(f32)]` —— 末项仅 v6。

- `samples` 是 **VM 指令批次数**（每 4000 条指令一次 LUA_MASKCOUNT），不是毫秒。
  C 调用期间指令计数器冻结，所以「整行开销都在某个 C 调用里」的行 samples 会接近 0。
- `line_ms` 是**真实墙钟**，含从该行发起的 C 调用耗时。**分析时按 `line_ms` 排序，
  不要按 `samples`。**

### trace 事件

`event_sequence(u32) qpc(u64) time_seconds(f64) frame_sequence(u64)
lua_thread_id(u32) os_thread_id(u32) method_id(u32) method_name(str)
depth(u32) type(u32) flags(u32)`

type（见 common/profiler_ipc.h）：
`1=CALL 2=RETURN 3=TAIL_RETURN 4=RESUME 5=SUSPEND 6=FRAME_BEGIN 7=FRAME_END`

**注意**：trace 事件流默认可能未开启（录制文件内 event_count 全为 0）。
调用方归因依赖它；未开启时只能靠调用率交叉验证 + 静态源码推断。

## `lua_wall_ms` 截断（务必先读）

**结论：衡量 Lua 占用用 `lua_cpu_ms` / `self_ms`，不要用 `lua_wall_ms`。**

- `lua_wall_ms` = "该帧所有线程忙区间**求并集**"（`injection/lua_profiler.c:864
  lp_union_thread_intervals`，在 `frame_end` 里 :1369 调用）。
- 忙区间是**每个 hook 事件一个**：每次 hook 进入时 `lp_close_busy_locked`(:756-757)
  追加区间 `[busy_start, entry_qpc]`，退出时重开 `busy_start = exit_qpc`(:838-841)。
  开行采样/追踪时 `exit_qpc = lp_now()`(:834-837) ≠ `entry_qpc`，相邻区间不满足
  `begin <= last->end` 因而不合并 → 一帧的区间数 ≈ 该帧 hook 事件数。
- `LP_LOCAL_INTERVAL_CAP = 4096`(:15)；超限时 `lp_append_local_interval`(:566-569)
  **直接 return 丢弃该区间**并置 `INTERVAL_LIMITED`(flags bit7 = 0x80)。
- 实测 hook_events 典型 5000–190000/帧，绝大多数帧超限。

**判据（可直接用来自检）**

| 条件 | 含义 |
|---|---|
| `flags & 0x80 == 0` | 未截断：`lua_wall == lua_cpu`，两者都可信（差值恒为 0） |
| `flags & 0x80 != 0` | `lua_wall` 被截断、偏小；**只看 `lua_cpu`** |
| `lua_cpu - lua_wall > 0` | 等价于 `flags & 0x80 != 0`（实测 r(cpu-wall, hook_events) ≈ +0.93，`(cpu-wall)/hook_events` 稳定 0.29–0.51µs）|

截断后的 `lua_wall` ≈ `4096 × (相邻 hook 事件平均间隔)` ≈ 1.5–2.4ms，与真实负载无关
（实测过 10–21 倍的偏差）。副作用：`r(period, lua_wall)` 会被严重稀释
（例如 body 相关 0.931、lua_cpu 相关 0.844，而 lua_wall 只有 0.324），
**不要据此得出"掉帧与 Lua 无关"的结论**。

## 帧 flags 位含义（`common/profiler_ipc.h:50-59`）

| 位 | 值 | 枚举 | 含义 |
|---|---|---|---|
| 0 | 0x01 | EVENT_DROPPED | trace 事件有丢弃（调用方归因会不准）|
| 1 | 0x02 | METHOD_LIMITED | 方法表或影子栈触顶（2048 / STACK_CAP），per-method 明细**被截断** |
| 2 | 0x04 | THREAD_LIMITED | 线程数触顶 |
| 3 | 0x08 | LINE_LIMITED | 行采样触顶（每线程每帧 64 行）|
| 4 | 0x10 | LINE_CAPTURED | 本帧开了行采样（即 `exit_qpc = lp_now()`）|
| 5 | 0x20 | STACK_DESYNC | 影子栈失步（数据可疑）|
| 6 | 0x40 | STATE_CONCURRENT | 同一 Lua state 被多 OS 线程跑 |
| 7 | 0x80 | INTERVAL_LIMITED | **忙区间截断 → `lua_wall` 不可信** |

分析任何录制时先统计这几个位：`0x01/0x20/0x40` 全为 0 时 per-method 数据无结构性风险；
`0x02` 置位的帧方法表被截断，不能拿它的"top 方法榜"下结论。

## 方法名 → 源码定位

- profiler 名 `[string "app/xxx/yyy.luac"]:NNN:` 中的 NNN 是**已加载 chunk 的行号**，
  与 luac dump 出的明文 `.lua` 行号 1:1 对应（dump 就是运行时 loadbuffer 的原文）。
- 磁盘上的 `.luac`（E:\YZST 等）是加密容器（头 `YSZTMB2CP!`，密钥流无短周期，
  已知明文攻击无效），**不要**试图离线解密，直接用 dump 的明文。
- dump 目录有两个：
  - `build/luac_dump/sealapp/` —— 早期会话完整 dump（含 app/scene、main、text_format 等启动期文件）
  - `build/generated/luac_dump/sealapp/` —— 晚期会话 dump（仅 hook 安装后加载的文件，缺核心文件）
  - 找不到文件时先查两处的 `luac_dump_map.tsv`（chunk 名 → 明文路径映射）。

## 环境注意事项（本机）

- Bash 工具不可用；PowerShell stdout 不回传 → 一律 `*> out.txt` 重定向，
  `Out-File` 默认 UTF-16，读之前 `Get-Content | Set-Content -Encoding utf8` 转码。
- **不要用 `Read` 直接看 `Out-File` 的产物**（中文会变乱码 `瀹氭椂鍣?`），必须先转码。
- Python 用托管版绝对路径：`C:\Users\xuhao2\.workbuddy\binaries\python\versions\3.13.12\python.exe`。
- 大文件（数百 MB）流式解析耗时几十秒，PowerShell 命令设置 timeout ≥ 300000ms。
- **PowerShell 传参坑**：给 `correlation_analysis.py` 传 watch 子串时不要出现双引号
  （方法名形如 `[string "xxx.luac"]:42:Lua:f`）。`"a.luac\"]:42"` 里的 `\"` 会被
  PowerShell 解析掉，导致子串匹配不到、报告里 watch 段为空（**不会报错，只会静默无输出**）。
  改用不含引号的子串，例如 `:42:Lua:msg_format_indent`、`:61:Lua:dump_`。
- Windows 侧 `Bash` 工具里 `find`/`ls`/`head`/`dirname` 都不可用，文件枚举请用
  Glob / Grep 工具或 PowerShell `Get-ChildItem -Recurse`。
