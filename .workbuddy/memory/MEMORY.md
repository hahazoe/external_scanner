# external_scanner 项目长期备忘

## 项目一句话
Windows 32 位 Cocos2d-x + Lua 5.1 进程的外部逐帧性能分析器（Cocos2d DebugView）。
viewer（debugview.exe，ImGui/DX9）注入 DLL 到目标进程，经共享内存
`Local\CocosDebugProfiler_v<ver>_<pid>`（magic 0x42444343）交换逐帧数据。
录制格式 `.cocosdbg`（magic CCDBREC1）。版本号以代码为准。

## 构建与验证（本机环境特殊）
- 构建：`build.bat`（32 位 MinGW，Lua 源码在 `../Lua_5_1_4/src`）。含 4 个测试：
  ipc_selftest / lua_profiler_core / lua51 集成 / recording_io。
- 本机 Bash 工具完全不可用；PowerShell stdout 不回传。
  验证法：PowerShell `& ".\build.bat" *> build_log.txt`，日志 UTF-16，
  需 `Get-Content | Set-Content -Encoding utf8` 转码后再 Read。PowerShell 禁止调 cmd.exe。
- gcc = `C:\MinGW\bin\gcc.exe`（MinGW.org GCC 6.3.0-1，原生 32 位）。
- 单模块快验：gcc 编 `temp/<探针>.c + injection/lua_profiler.c + injection/lua_api_hooks.c + 30 个 Lua .c`，
  输出 `*> log.txt` 再转码 Read。测试桩需 `-DCOCOS_DEBUG_LUA_PROFILER_TESTING`。

## 代码约定（改代码时遵守）
- injected_runtime / lua_api_hooks 共用 `common/x86_insn.h`
  的解码与重定位；新增 hook 不要再复制解码器。
- 打补丁必须包 `injection/patch_freeze.h` 的 freeze_begin/end，VirtualAlloc 必须在 freeze 之前。
- 遇到 EB/jcc 短跳转要拒绝安装（x86_reloc_copy 返回 0），不能静默写坏。
- 远程 Lua 只能在 Cocos Scheduler 上下文执行，禁止在 drawScene/C++ hook 里 pcall。
- 注入过 profiler 的进程不能二次注入，必须重启目标进程（SHUTDOWN 不卸 hook，是刻意的）。
- viewer 帧快照字符串一律走 `debugview/name_pool.h` 全局池（const std::string*），
  池 append-only 永不清空，snapshot 里不要放 std::string。
- **时间守恒不变量**：任何推进时间的路径都必须让每个 delta 同时进 `thread->cpu_qpc`、
  `script_qpc` 或 `c_qpc`、`top->frame_self_qpc` 和方法 acc 的 `self_qpc`。"丢弃时间"的路径
  （如 `lp_drop_frames_locked`）要转移而非删除，保持 `sum(method self)==lua_cpu` 且
  `script + c == cpu` —— lua51 集成测试会断言这两条。
- 目标样本 sealapp.exe；对照明文用 `E:/YZST_lua_decrypted`。

## lua_profiler 失真点（2026-09-18 审计）
### 已修（有回归测试 + 反向验证）
- **P0-1 错误展开**：pcall 接住错误时不发 RET hook → 影子栈残留死帧。修法：hook `lua_pcall`
  （`lua_api_hooks.c`，install 返回位掩码 bit2=4），protected_enter/leave 记录/恢复栈深，
  失败时 `lp_drop_frames_locked` 截断。**`injected_runtime.c` 里 pcall hook 装不上就报 error 拒绝出数据**。
  不要改 `lp_rebuild_stack_locked` 做全量重同步：`lua_getstack` 跳过 tail call，会误删合法尾调用帧。
- **P0-2 同帧多根调用 inclusive 少算**：`thread->logical_qpc=0` 移到 is_call 分支、push 之前。
- **P1-3 字典 key ≥255 字符永不去重**：`telemetry_writer.intern_method` 改为先截断再 hash/比较。
  行采样不再 intern 原始 `ar->source`，改 `"@src:"+short_src`。

## 2026-09-18 新增的辅助指标（IPC v9 / 录制 v6）
- **行级墙钟**：`CocosDebugIpcLineSample.qpc`（行结构 16→24 字节）。行采样原本是指令数采样，
  指令计数器在 C 调用里冻结 → 现在按真实时钟记账，C 调用时间落到调用点那一行。
  viewer 行表新增 "Time ms" 列，**按它排序**，别按 Samples。
- **窗口外 Lua**：`lua_outside_ms` = 上一帧结束到本帧窗口打开之间的 Lua。
  **刻意不进 lua_cpu、不归属任何方法**（会破坏 `sum(method self)==lua_cpu`）。
- **C 匿名归属**：匿名 C 的 key 改为 `[C]:<anonymous>@<caller_id>`，成本落到驱动它的 Lua 函数。
  名字缓存必须跳过匿名 C（缓存键区分不出调用点）。
- **viewer 跨帧面板**：命中帧数 / 命中率 / self 总量 / 每次命中均值 / **p95** / 尖刺 Δself /
  覆盖率 / 选中方法的 self-per-frame 曲线。**命中少+p95 高=周期尖刺；Δ高+覆盖率低=间歇成因。**
- **已采集未显示**：`lua_mem_kb`（看每帧增量，锯齿=健康 GC，单调上升=泄漏）、
  `hook_events × 0.23µs` = 观察器自身开销（进 body 不进 lua_cpu，判断前先减）。
- 录制 v6 向后兼容读 v5。**注意：外部 Python 解析器 `cocosdbg_reader.py` 的帧头格式串要同步改。**

### 未修（按设计保留，报告时标注）
- **P1-4 `lua_wall_ms` 多数帧被静默截断**：忙区间是"每 hook 事件一个区间"，
  `LP_LOCAL_INTERVAL_CAP=4096` 超限即丢弃并置 `INTERVAL_LIMITED(0x80)`。
  **判据：`lua_cpu - lua_wall > 0` 当且仅当 0x80 置位。分析时用 lua_cpu，别用 lua_wall。**
- **P1-5 hook 自身开销** ≈0.23µs/事件（2 万事件/帧 ≈ +4.6ms body）。是否计入 lua_cpu 取决于
  `g_line_capture || g_trace_enabled`（为真时 `exit_qpc=lp_now()`，hook 开销落在 body）。
- **P2-6 行采样是指令数采样**（LUA_MASKCOUNT，每 4000 条 VM 指令一次），不是时间采样。
- **P2-7 `lua_cpu_ms` 是"在 Lua 里的墙钟"**按线程求和；帧窗口 = drawScene body，窗口外 Lua 全丢。

### 已澄清 / 曾经的误判
- `g_method_meta_count` 在 `start()` 不清零**不是 bug**（writer 字典也不清），不要再"修"。
- 回归测试：`tests/lua_profiler_core_test.c`（two_root_calls_in_one_frame / protected_error_unwind）、
  `tests/lua_profiler_lua51_test.c`（真 Lua + 真 pcall hook 出错续跑）、
  `tests/ipc_selftest.c`（899 字符长 key 去重）。

## 崩溃栈分析工具链（与逐帧 profiler 无关的另一条线）
- `analyze_dump_stack.py`：用 Breakpad `.sym` 符号化**单条** dump.log（支持 GUI/CLI 两用）。
  `analyze_dump_stack_gui.py` 是 tkinter 窗口，frozen 时用 `sys.executable` 自调 + CLI 参数转 `engine.main()`。
- `scripts/parse_crash_md.py`：批量解析飞书《PC崩溃通知》md（多条「PC宕机上报」），
  复用上面引擎符号化 → 按「异常码+前6层符号」聚合 → python-docx 出 Word 报告。
  用法见 skill `~/.workbuddy/skills/crash-md-report/`。
- 符号包 `breakpad_syms.zip` 是 WinZip AES（压缩方法 99），密码 `SealBreakpadSym`，必须 `pyzipper` 解。
- 托管 venv `C:\Users\xuhao2\.workbuddy\binaries\python\envs\default` 已装 python-docx、pyzipper。

## 目标样本 sealapp 的已知反模式
详见 `.workbuddy/memory/sealapp-antipatterns.md`（内容多，单独文件）。
核心判据：
- **【判据 0】先算"遍历/提交比" S/R**：基线 ≈2.2×；尖刺帧 S/R 20–60× 而 RenderR 不变
  → 绘制量没变、纯 CPU 逐节点工作（查 `Label::updateContent` / C++ visit / sortAllChildren）。
- **【判据 1】看 `base`（活跃帧数）是否 ≈ 尖刺帧数**：稀有度视角在异质尖刺上会输出 `(none)`，
  此时改用覆盖率视角，别下"没成因"的结论。
- 用 `scripts/spike_breakdown.py` 一次算完 r(period, 各分量) + 分类 + S/R。
