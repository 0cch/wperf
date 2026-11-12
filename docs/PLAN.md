# wperf 完整技术方案

> 目标：在 Windows 上实现一个尽量接近 Linux `perf` 的轻量级 CLI profiling / tracing 工具。
>
> 约束：不编写内核驱动、不修改 Windows 内核、不依赖 WPA GUI；优先复用 ETW、Win32、DbgHelp 和系统暴露的硬件计数器。
>
> 基线：Windows 10 21H2+ / Windows 11 x64。ARM64、x86/WOW64 后续处理。

## 1. 方案摘要

`wperf` 是单 EXE、CLI 优先工具，分为四类能力：

1. CPU 采样：`perf record/top/report`，使用 ETW sampled profile、stack walk 和 PDB 符号化。
2. 系统 tracing：`perf trace/sched`，使用进程、线程、上下文切换、系统调用、页错误、I/O 等 ETW 事件。
3. 计数器统计：`perf stat`，优先使用进程/线程时间和 cycle API，硬件 PMU 使用 ETW PMC。
4. 离线分析：`perf report/script/annotate/diff`，解析 ETL 或后续 `.wperf` 格式。

最终不依赖 WPA/WPR 作为运行时组件。开发阶段使用 xperf 生成 golden trace。

## 2. 目标与非目标

### 目标

- `record/top/stat/trace/report/script/annotate/list/diff/archive/buildid-list`。
- 单进程、指定 PID、全系统采集。
- CPU profile、栈、PID/TID/CPU/模块/符号归属。
- 调度、系统调用、页错误、磁盘、文件、网络、注册表、ALPC、DPC/ISR。
- ETW PMC、LBR 的运行时能力发现和明确降级。
- 表格、CSV、JSON、folded stack 输出。
- 离线分析不要求管理员权限。

### 非目标

以下能力在不写驱动、不修改目标程序时不能承诺完整支持：

- kprobe/uprobe。
- 完整 `perf_event_open` per-process PMU 语义。
- `perf mem` 的精确访问地址和访问类型。
- `perf c2c`。
- Intel PT 级连续分支 trace。
- 无 instrumentation 的函数级完整调用图。
- 所有 JIT 语言自动栈展开。

## 3. 设计原则

1. ETW 是第一底座。
2. 捕获与符号化分离。
3. 先 ETL，后 `.wperf`。
4. 能力运行时发现，不硬编码 CPU 型号。
5. 所有降级可见：丢失事件、缺失符号、PMC 不可用必须显示。
6. 离线命令不需要管理员权限。
7. 核心保持单 EXE，不依赖 .NET、Qt 或 WPA。
8. 先正确性后性能，默认 999 Hz/1 kHz。

## 4. Windows 能力边界

使用：

- `StartTraceW` / `ControlTraceW`
- `EnableTraceEx2`
- `TraceSetInformation` / `TraceQueryInformation`
- `OpenTraceW` / `ProcessTrace` / `CloseTrace`
- `EVENT_TRACE_FLAG_*`
- `CLASSIC_EVENT_ID`
- `TRACE_PROFILE_INTERVAL`
- `PROFILE_SOURCE_INFO`
- `TraceConfigureLastBranchRecord`
- `EVENT_TRACE_FLAG_PROFILE`
- `TraceStackTracingInfo`
- `TraceSampledProfileIntervalInfo`
- `TraceProfileSourceListInfo`
- `TraceMaxPmcCounterQuery`
- `TracePmcCounterOwners`
- `TracePmcSessionInformation`

权限：

| 命令 | 权限 |
|---|---|
| report/list/annotate | 普通用户 |
| record/top/trace | 管理员或提权 worker |
| stat 时间统计 | 普通用户，受目标进程权限限制 |
| stat --pmu | 通常管理员 |

不写驱动的硬边界：

- 不能直接 RDMSR/WRMSR。
- 不能任意内核插桩。
- 不能保证所有 PMU 事件可见。
- 不能实现完整精确 PEBS/IBS 数据。

## 5. perf 能力映射

| perf 能力 | Windows 实现 | 等级 |
|---|---|---|
| `perf list` | profile source + ETW provider 枚举 | 直接支持 |
| `perf record -g` | PROFILE + StackWalk | 直接支持 |
| `perf top` | ETW real-time consumer | 直接支持 |
| `perf report` | ETL/`.wperf` + DbgHelp | 直接支持 |
| 时间统计 | GetProcessTimes/GetThreadTimes | 直接支持 |
| cycles | QueryProcessCycleTime/QueryThreadCycleTime | 可近似 |
| PMC stat | ETW PMC | 可近似 |
| trace | SystemTraceProvider/TDH | 可近似 |
| sched/off-CPU | CSwitch/ReadyThread | 直接支持 |
| syscall | SYSTEMCALL | 可近似 |
| page fault/I/O | 对应 ETW flags | 直接支持 |
| DPC/ISR | DPC/INTERRUPT | 直接支持 |
| LBR | TraceConfigureLastBranchRecord | 可近似 |
| annotate | DbgHelp + Capstone/Zydis | 可近似 |
| script | 字段脚本/表达式 | 可近似 |
| diff/archive/buildid | 自有分析 | 直接支持 |
| perf mem/c2c | 无通用精确数据 | 暂不支持 |
| kprobe/uprobe | provider/instrumentation | 暂不支持通用动态探针 |
| lock/kmem | WPT 控制接口/事件近似 | 可近似 |
| cgroup | Job Object 后过滤 | 可近似 |
| .NET/JIT | Runtime ETW/EventPipe | 扩展支持 |

## 6. 总体架构

```text
CLI
  -> Capture Controller / privilege / process lifecycle
  -> ETW system or provider backend
  -> Decoder
  -> Normalized event model
  -> Symbolizer / trace storage / live aggregator
  -> Reports (flat, call tree, folded, sched, io, annotate)
```

目录建议：

```text
src/
  common.* logger.* state.* etw.* ipc.* elevation.* crash.*
  commands.* main.cpp
tests/
  unit_tests.cpp workloads/cpu_burn.cpp
scripts/
  m0_probe.ps1 m1_smoke.ps1
```

技术选型：

- C++20、MSVC、CMake。
- Windows SDK 10.0.19041+，用 10.0.26100 编译并动态探测新 API。
- 链接 `advapi32`、`tdh`、`dbghelp`、`psapi`、`shell32`、`bcrypt`。

### 提权进程模型

```text
普通权限 controller
  -> UAC / ShellExecuteEx(runas)
  -> 高权限 headless worker
  -> session guard
```

- controller：UI、目标进程、stdout/stderr、退出码、IPC。
- worker：只做 ETW 捕获、flush、清理。
- guard：worker 崩溃时停止 orphan ETW session。
- target 默认由 controller 启动，保持普通权限，不允许跟随 worker 被提权。
- worker 不创建窗口、不占控制台、不等待 stdin。

## 7. Capture 设计

### Profile

| Profile | flags | 用途 |
|---|---|---|
| cpu | PROCESS/THREAD/IMAGE_LOAD/PROFILE | CPU 采样 |
| cpu-sched | 上述 + CSWITCH/DISPATCHER | CPU+调度 |
| syscall | PROCESS/THREAD/SYSTEMCALL | 系统调用 |
| vm | PAGE_FAULTS/VIRTUAL_ALLOC/VAMAP | 页错误 |
| io | DISK_IO/FILE_IO/FILE_IO_INIT | I/O |
| kernel | DPC/INTERRUPT/DRIVER | 内核延迟 |
| pmu | PROFILE/PmcProfile | PMC |
| lbr | PROFILE+LBR | 分支记录 |

### 采样流程

1. 检查权限。
2. 查询 profile source/PMC/LBR。
3. 创建唯一 session。
4. `StartTraceW`。
5. 设置 profile interval 和 stack tracing。
6. 启动/attach target。
7. 消费 ETL 或实时事件。
8. Ctrl-C/duration/target exit。
9. flush、stop、close。
10. 输出质量指标。

### 时间与栈

- `ClientContext=1` 使用 QPC。
- 使用 `EVENT_TRACE_USE_GLOBAL_SEQUENCE`。
- 统一纳秒时间。
- StackWalk 按 `(EventTimeStamp, StackThread)` 关联。
- 栈长度按事件实际大小解析，不假设固定层数。
- 默认只给 Profile 开栈，CSwitch/I/O 栈按需开启。
- 使用 `TraceStackCachingInfo` / `TraceUnifiedStackCachingInfo` 减少 trace 体积。
- 对 x64 内核栈不完整显示 `kernel_stack_partial`。

### 调度、I/O、PMC、LBR

- CSwitch/ReadyThread 构造运行、就绪、阻塞时间线。
- system call 只承诺 ETW 暴露的参数，不承诺完整 Linux 参数语义。
- I/O init/complete 关联计算延迟。
- PMC 启动前查询 source、max counters、owners/session；保存并恢复 profile interval。
- LBR 深度有限，明确不是 Intel PT。
- context register 可用时补充 IP/SP，但不替代 StackWalk。
- 用户 ETW provider 使用 `EnableTraceEx2` 和 TDH 解码。

### 无管理员 fallback

可选 attach sampler：SuspendThread/GetThreadContext/StackWalk64。默认关闭，标记 experimental。

## 8. 事件模型

```text
timestamp_ns, sequence, cpu, pid, tid,
provider_guid, event_id, opcode, level, keyword, payload
```

对象：

- ProcessInfo
- ThreadInfo
- ImageInfo
- SampleInfo
- StackInfo
- SchedInfo
- SyscallInfo
- IoInfo
- PmcInfo
- LbrInfo

栈和字符串 intern；事件、元数据和索引分开存储。

## 9. 符号化

- PDB + symbol server + 本地 cache。
- PE/PDB identity：timestamp、image size、CodeView GUID、PDB age。
- DbgHelp API：SymInitializeW、SymLoadModuleExW、SymFromAddrW、SymGetLineFromAddrW64。
- DbgHelp 串行调用。
- 无符号时显示 `module+offset`，不能丢弃样本。
- 第一版不做 source server 命令执行。
- 默认 cache：`%LOCALAPPDATA%\wperf\symbols`。

## 10. 存储

第一阶段使用 ETL：

- 便于 xperf/WPA 对照。
- `OpenTraceW + ProcessTrace` 读取。
- `record` 输出 `.etl`。

第二阶段 `.wperf`：

```text
Header + section table + metadata + process/thread/image tables
+ interned strings + stack table + event table + indexes
```

索引：timestamp、pid/tid、event type、CPU、module。压缩可选 zstd，不改变读取语义。

## 11. CLI

```text
wperf record [--elevate] [-F HZ] [-g] [-p PID] [--duration MS] [-o FILE] [--profile cpu] -- command
wperf top [-p PID] [-e EVENT] [--sort FIELD] [--interval 1s]
wperf stat [-e LIST] [-p PID] [--per-thread] [--] command
wperf trace [-e LIST] [-p PID] [--duration TIME]
wperf report -i FILE [--sort ...] [--call-graph ...] [--folded] [--json] [--csv]
wperf script / annotate / diff / archive / buildid-list
wperf list / doctor / recover / test / version
```

M1 当前已实现：`list/doctor/test/recover/record/version`。

## 12. 报告算法

- sample 权重默认等于 profile interval。
- self = leaf frame；inclusive = 栈中出现过的 symbol。
- cpu% 按采样覆盖时间归一化。
- 支持按 pid/tid/comm/cpu/dso/symbol 分组。
- 支持 caller/callee 树、folded stack、CSV/JSON。
- 调度报告：running/ready/blocked、wakeup latency、wait reason、阻塞栈。
- I/O 报告：次数、字节数、延迟、orphan init/complete。
- stat 区分 total、sample-derived、estimated。

## 13. 并发与错误

```text
ETW callback -> bounded MPSC ring -> decoder -> writer/aggregator
                                              -> symbol workers
```

- 回调内不分配大对象、不做 DbgHelp、不做文件 I/O。
- ring 满时计数并丢弃，质量报告中显示。
- DbgHelp 串行调用。
- 错误码：2 参数、3 UAC/权限、4 ETW、5 能力、6 trace、9 内部。
- target 非零退出码默认优先返回。

## 14. 安全

- 不执行 source server 任意命令。
- 不自动加载 trace 中任意 DLL/PDB。
- 严格校验 ETL payload。
- 使用 CreateProcessW，不经过 shell。
- 不自动杀未知进程。
- 提权 worker headless，target 默认保持普通权限。
- IPC 需要 nonce、父进程校验和受限 ACL。
- 诊断包默认 redaction。

## 15. 测试

工作负载：单/多线程 CPU burn、锁竞争、递归栈、DLL load/unload、有/无 PDB、页错误、I/O、无 PMC/LBR 的 VM。

测试层次：

- 单元：quoting、state round-trip、ETW query、worker IPC failure path。
- 集成：xperf ETL golden trace、profile/stack 对比。
- UAC：见第 21 章。
- 异常：见第 22 章。
- 性能：1 kHz、8 线程目标开销 < 5%。

## 16. 里程碑

| 阶段 | 内容 | 时间 |
|---|---|---|
| M0 | ETW/xperf 能力验证 | 1 周 |
| M1 | CLI、worker/guard、状态/日志、ETW lifecycle | 2～3 周 |
| M2 | CPU sample + StackWalk + report | 3 周 |
| M3 | DbgHelp 符号化和 folded/calltree | 2 周 |
| M4 | top + stat + live ring | 2 周 |
| M5 | sched/syscall/pagefault/I/O | 4 周 |
| M6 | PMC/LBR/context register | 4 周 |
| M7 | annotate/diff/archive/发布 | 4 周 |

单人约 5～6 个月完成 v1.0，两人约 3～4 个月。

M2-M7 的可执行任务拆分、每阶段交付物、测试门槛和 M2 立即执行顺序见 [ROADMAP.md](ROADMAP.md)。里程碑表只是总览，具体实现不以总览表替代。
## 17. 风险

| 风险 | 应对 |
|---|---|
| classic event schema 变化 | 版本化 schema、golden trace |
| 栈不完整 | 质量标志、module+offset |
| PMC 被占用 | owners/session 检查、strict 模式 |
| LBR 平台差异 | 运行时探测、明确 unsupported |
| 高采样丢事件 | buffer 调整、stack cache、丢失计数 |
| 符号慢 | 后台、超时、磁盘 cache |
| 恶意 trace | 边界检查、fuzz |
| 目标被提权 | controller 启动 target，worker 只采集 |
| worker 崩溃留 session | guard + recover |
| 诊断泄露 | redaction、nonce 不落盘 |

## 18. v1 后置项

- .NET/Java/V8 JIT 栈。
- 驱动级 instrumentation。
- 任意 uprobe/kprobe。
- 完整 PEBS/IBS、perf mem、perf c2c、perf kvm。
- GUI/Web UI。
- 分布式采集。
- 低权限长期 profiling daemon。

## 19. 参考

- <https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-tracesetinformation>
- <https://learn.microsoft.com/en-us/windows/win32/etw/stackwalk-event>
- <https://learn.microsoft.com/en-us/windows-hardware/test/wpt/xperf-command-line-reference>
- <https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryprocesscycletime>

## 20. v1.0 验收

1. CPU sampling + stack + symbolization + per-symbol report。
2. 能展示权限、丢失事件、符号缺失等 Windows 能力差异。
3. `top/stat/trace/sched/report/script/annotate/diff` 可组成 CLI 工作流。
4. 不依赖 WPA/WPR 运行时，不写驱动，不修改目标程序。
5. 对无法实现的能力明确标记 unsupported。
## 21. 附录 A：普通权限下的提权测试（MVP）

### 21.1 范围

第一版只验证核心架构风险：

1. UAC 后普通权限 controller 能否控制 headless worker。
2. target 是否保持普通权限。
3. UAC 取消是否无副作用。
4. controller/worker 异常退出是否留下 session。

不覆盖域账号、SSH、服务上下文、UNC、多用户并发、1000 次 soak。

### 21.2 环境

| 环境 | 用途 |
|---|---|
| T1：Windows 11 x64 + UAC Admin Approval Mode | 人工 UAC 验收 |
| T2：管理员测试 runner | 跳过 UAC 测 worker |

### 21.3 控制拓扑

```text
controller (medium)
  -> creates named pipe before UAC
  -> ShellExecuteExW(runas, SEE_MASK_NOCLOSEPROCESS)
  -> worker (high, headless)
  -> IPC: HELLO/READY/PHASE/LOG/STOP/FINISHED
```

- controller 保存 `hProcess`，不依赖事后 `OpenProcess(worker_pid)`。
- worker 不创建 UI、不接管控制台、不等待 stdin。
- `top` 等 UI 始终留在 controller。
- 测试构建支持 `QUERY_STATE/GET_SESSION/STOP/INJECT_CRASH/GET_EXIT`，正式版不编译。

### 21.4 核心用例

| 编号 | 场景 | 预期 |
|---|---|---|
| UAC-01 | UAC 同意 | 收到 READY，可停止 |
| UAC-02 | UAC 取消 | 返回 3，无 target/session |
| UAC-03 | record -- target | target 保持 medium |
| UAC-04 | stdout/stderr/退出码 | 透传 |
| UAC-05 | Ctrl-C | flush、停止、无残留 |
| UAC-06 | 强杀 controller | worker 自清理 |
| UAC-07 | 强杀 worker | guard 清理 |
| UAC-08 | 注入 worker crash | dump + guard cleanup |
| UAC-09 | 伪造 IPC/nonce | 拒绝 |
| UAC-10 | 正常/异常各 10 次 | 无残留 |

### 21.5 自动化

- 日常：管理员 runner 直接启动 worker，用测试 controller 走 IPC。
- 真实 UAC：人工确认；不从 guest 内 medium 进程自动点击 secure desktop。
- 每次 UAC 后运行 `wperf doctor` 检查残留。

### 21.6 验收

- 5 秒内收到 `WORKER_READY`。
- target 与 controller 同 integrity level。
- UAC 取消无 target/worker/session。
- controller/worker 异常退出无 `wperf-*` 残留。
- worker 崩溃生成 dump，guard 能停止 session。
## 22. 附录 B：异常与崩溃排查（MVP）

### 22.1 范围

只保证：

1. 明确错误信息。
2. 稳定退出码。
3. correlation ID + 状态文件 + JSONL 日志。
4. ETW session 可清理/恢复。
5. 未处理异常生成 minidump。
6. target 崩溃不丢 trace、不覆盖退出码。

WER/full memory dump/自动上传/大规模 soak 后置。

### 22.2 状态与日志

```text
%LOCALAPPDATA%\wperf\sessions\<id>.json
%LOCALAPPDATA%\wperf\logs\<id>-controller.jsonl
%LOCALAPPDATA%\wperf\logs\<id>-worker.jsonl
%LOCALAPPDATA%\wperf\logs\<id>-guard.jsonl
```

phase：

```text
CREATED -> ELEVATING -> WORKER_READY -> CAPTURING
        -> STOPPING -> FLUSHING -> FINISHED
```

异常：`ELEVATION_FAILED/WORKER_LOST/TARGET_FAILED/TRACE_INCOMPLETE/INTERNAL_ERROR`。

### 22.3 清理

| 异常 | 处理 |
|---|---|
| controller 退出 | worker 检测父进程并停止 |
| worker 崩溃 | guard 调 ControlTraceW |
| guard 崩溃 | 下次 wperf recover |
| target 崩溃 | flush trace，保留 target exit |
| trace 损坏 | partial report |
| 符号失败 | module+offset |

### 22.4 Crash dump

- `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`。
- 默认 `MiniDumpNormal + ThreadInfo + UnloadedModules`。
- 写 `%LOCALAPPDATA%\wperf\crashes`。
- 不在 handler 中加载符号或访问网络。
- 失败时至少写 metadata。

### 22.5 命令与错误码

```text
wperf doctor
wperf doctor --bundle DIR
wperf recover --session NAME
wperf recover --all-stale
wperf crash-info DUMP
```

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 参数错误 |
| 3 | UAC/权限 |
| 4 | ETW session |
| 5 | 能力不可用 |
| 6 | trace 不完整 |
| 9 | 内部错误 |

### 22.6 核心异常测试

| 编号 | 场景 | 预期 |
|---|---|---|
| ERR-01 | worker 崩溃 | dump + guard cleanup |
| ERR-02 | 强杀 controller | worker flush |
| ERR-03 | 强杀 worker | guard cleanup |
| ERR-04 | guard 一起崩溃 | recover 清理 |
| ERR-05 | target 崩溃 | trace 保留 |
| ERR-06 | 截断 ETL | partial/error，不崩溃 |
| ERR-07 | 重复 10 次 | 无残留 |

### 22.7 后置项

- WER LocalDumps/full dump。
- 大规模 fault injection/soak。
- debugger/crash watcher 自动附加。
- 细粒度 SEH 分类。
- 自动上传诊断包。
