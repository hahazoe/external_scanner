# .cocosdbg 录制格式参考（version 5 到 9）

格式权威来源：`debugview/recording_io.cpp`（kMagic `CCDBREC1`）。全部小端序。
当前写出的版本是 **9**；读取端必须同时接受 5、6、7、8、9（旧文件里较新的字段视为 0）。

## 文件布局

```
magic     8 bytes  "CCDBREC1"
version   u32      = 5、6、7、8 或 9
count     u64      帧数
frames    count × Frame
img_cnt   u32      预览图数量（可忽略，分析时读到帧结束即可）
images    img_cnt × Image
```

**version 6 相对 5 的两处差异**（都在中间插入，不追加在末尾 —— 解析器按版本分支，
读错了整帧会错位）：
1. Frame 头在 `lua_c_self_ms` 之后插入 `lua_outside_ms(f32)`。
2. 行采样在 `samples(u32)` 之后追加 `line_ms(f32)`。

**version 7 相对 6**：Frame 头在 `flags` 之后、四个 count 之前插入
`actor_x(f32) actor_y(f32) actor_valid(u32)`。曾用于主角像素坐标，现已停采，
新录制恒为 0；旧文件仍可读。v6 及更早读出来这三个字段为 0。

**version 8 相对 7**：Frame 头在 `actor_valid` 之后、四个 count 之前插入
`proc_ws_kb(f32) proc_private_kb(f32)`。`proc_ws_kb` 是任务管理器进程列表「内存」列的活动专用工作集，`proc_private_kb` 是私有提交，单位都是 KB。
v7 及更早读出来这两个字段为 0。

**version 9 相对 8**：在 CppMethod 列表之后、行采样之前插入
`file_io_count(u32)` + `file_io_count × FileIoSample`（每帧 `FileUtilsWin32:getData` 按路径聚合）。
v8 及更早读出来 `file_io` 为空。

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
| lua_wall_ms | f32 | 多线程忙区间并集，不是总耗时 |
| lua_cpu_ms | f32 | **Lua 总耗时** = script + C（由构造） |
| lua_script_self_ms | f32 | 影子栈顶为 Lua / main 的独占时间 |
| lua_c_self_ms | f32 | 从 Lua 调入的 C 函数 + 空栈 VM |
| lua_outside_ms | f32 | **仅 v6**：窗口外 Lua，不进 lua_cpu_ms、不归属任何方法 |
| lua_mem_kb | f32 | Lua 堆 KB |
| hook_events | u32 | |
| flags | u32 | |
| actor_x | f32 | **仅 v7**：遗留字段，新录制恒 0 |
| actor_y | f32 | **仅 v7**：遗留字段，新录制恒 0 |
| actor_valid | u32 | **仅 v7**：遗留字段，新录制恒 0 |
| proc_ws_kb | f32 | **仅 v8**：活动专用工作集 KB，与任务管理器「内存」列相同 |
| proc_private_kb | f32 | **仅 v8**：进程私有提交 KB |
| lua_count | u32 | LuaMethod 数 |
| cpp_count | u32 | CppMethod 数 |
| line_count | u32 | 行采样数 |
| event_count | u32 | trace 事件数 |

恒等式（drawScene 窗口内已记账时间片）：

- `lua_cpu_ms` = `lua_script_self_ms` + `lua_c_self_ms`
- `sum(kind≠C 的 method self)` ≈ script；`sum(kind=C 的 method self)` ≈ C
- 空栈且已进入 `lua_pcall` / `lua_call` / `lua_resume` → 记入 C，合成方法 `[C]:<vm>`
- `cc.Director:drawScene` / `Scene:render` / `Renderer:render` **不进**恒等式

### LuaMethod（每条）

`name(str) kind(u32) flags(u32) self_ms(f32) incl_ms(f32) max_self_ms(f32)
max_incl_ms(f32) calls_started(u32) calls_completed(u32) active_at_end(u32)`

- 数值均为**本帧内**的增量；跨帧聚合直接求和。
- `self_ms` = 函数自身耗时；`incl_ms` = 含被调函数。递归函数的 incl 求和会重复计数，只作参考。
- `kind`：1=Lua，2=C，3=main。
- `name` 形如 `[string "app/scene/SceneNpc.luac"]:947:Lua:run`（Lua）或 `[C]:format`（C）；
  空栈 VM 为 `[C]:<vm>`。C 函数经变量调用时 getinfo 拿不到名字，显示 `[C]:<anonymous>`。

### CppMethod

`name(str) total_ms(f32) max_ms(f32) last_ms(f32) calls(u32) call_sequence(u32)`

### FileIoSample（仅 v9，紧接 CppMethod 之后）

`path(str) total_ms(f32) max_ms(f32) last_ms(f32) calls(u32)`

- 本帧内 `FileUtilsWin32:getData` 按传入路径聚合；总耗时仍看 CppMethod 里的 `FileUtilsWin32:getData`。
- 路径数超限时多余路径合并到 `<truncated>`，帧 `flags` 含 `FILE_IO_LIMITED`（bit8）。

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
未开启时用 `incl_ms` 嵌套 + 稀有度做调用方归因。

## `lua_cpu` 与 `lua_wall`

**衡量 Lua 占用用 `lua_cpu_ms`（= script + C）。`lua_wall_ms` 只是忙区间并集。**

### 新内核（一次 Lua 运行一段）

忙区间只在「开始跑 / yield / 根 RET / pause」开关，不再每个 hook 切一段。
单线程时 `lua_wall ≈ lua_cpu`；多 `lua_State` 并行时 cpu 是求和，可以大于 wall。
`INTERVAL_LIMITED`（flags bit7 = 0x80）不应再被 hook 密度打满。

### 旧录制（hook 密度截断）

旧内核把每个 hook 事件做成一段忙区间，`LP_LOCAL_INTERVAL_CAP = 4096` 超限就静默丢弃
并置 0x80。hook_events 常见 5k–190k/帧，wall 会被压到约 1.5–2.4ms，与真实负载无关。

| 条件 | 含义 |
|---|---|
| 大量帧 `flags & 0x80` | 旧截断：`lua_wall` 偏小，**只看 `lua_cpu`** |
| 0x80 稀疏或为 0 | 按新内核解读：wall 是并集参考，占用仍用 cpu |
| `lua_cpu - lua_wall > 0` 且无 0x80 | 多线程重叠（cpu 求和 > 并集），不是截断 |

不要用 `r(period, lua_wall)` 判定「掉帧与 Lua 无关」；用 `r(period, lua_cpu)`。

## 帧 flags 位含义（`common/profiler_ipc.h`）

| 位 | 值 | 枚举 | 含义 |
|---|---|---|---|
| 0 | 0x01 | EVENT_DROPPED | trace 事件有丢弃（调用方归因会不准）|
| 1 | 0x02 | METHOD_LIMITED | 方法表或影子栈触顶，per-method 明细**被截断** |
| 2 | 0x04 | THREAD_LIMITED | 线程数触顶 |
| 3 | 0x08 | LINE_LIMITED | 行采样触顶 |
| 4 | 0x10 | LINE_CAPTURED | 本帧开了行采样 |
| 5 | 0x20 | STACK_DESYNC | 影子栈失步（数据可疑）|
| 6 | 0x40 | STATE_CONCURRENT | 同一 Lua state 被多 OS 线程跑 |
| 7 | 0x80 | INTERVAL_LIMITED | 忙区间表触顶；旧录制上表示 wall 不可信 |

分析任何录制时先统计这些位：`0x01/0x20/0x40` 全为 0 时 per-method 数据无结构性风险；
`0x02` 置位的帧方法表被截断，不能拿它的「top 方法榜」下结论。

## 方法名 → 源码定位

- profiler 名 `[string "app/xxx/yyy.luac"]:NNN:` 中的 NNN 是**已加载 chunk 的行号**，
  与 `E:/YZST_lua_decrypted` 明文 `.lua` 行号 1:1 对应。
- 磁盘上的 `.luac` 是加密容器；对照明文见 [yszt-source.md](yszt-source.md)。

## 环境注意事项（Cursor）

- 用 PATH 上的 `python`；报告写到 `build/generated/`，UTF-8。
- 大文件流式解析可能几十秒，Shell `block_until_ms` ≥ 300000。
- 给 `correlation_analysis.py` 传 watch 子串时不要带双引号（方法名形如
  `[string "xxx.luac"]:42:Lua:f`）。用不含引号的片段，如 `:61:Lua:dump_`。
- 文件枚举用 Glob / Grep 或 PowerShell `Get-ChildItem`。
