# wperf M2-M7 执行路线

本文是 docs/PLAN.md 的可执行拆解。M0/M1 已落地；M2/M3 的核心 CPU profile + PDB report 已落地，M4/M5 的 stat、snapshot top、script、trace/sched 事件以及 M6/M7 的 PMU、annotate、diff、archive 也已落地，细节见 [M2_M3.md](M2_M3.md)、[M4_M5.md](M4_M5.md) 和 [M6_M7.md](M6_M7.md)。后续仍按里程碑逐项补齐 real-time、精确关联和平台验证。

## 总体策略

1. **纵向切片**：每个阶段必须形成 `capture -> decode -> analyze -> report` 闭环，不能只增加底层模块。
2. **golden trace**：所有核心解析和聚合结果与 xperf/WPA 生成的基准 ETL 对比。
3. **能力门控**：PMC、LBR、context register 等平台能力必须运行时探测，失败时明确降级。
4. **保真度标签**：支持、近似、unsupported 必须出现在 CLI 和文档中。
5. **前后兼容**：classic ETW event schema 按 Windows build 维护适配层。
6. **先 Native C++**：JIT/托管语言栈放到 v1.x/v2。

```text
M0 -> M1 -> M2 -> M3 -> M4 -> M5 -> M6 -> M7
                    \-> symbol/report shared services
```

M2/M3/M4 是 CPU profiler 主路径，M5 扩展系统 tracing，M6 扩展 PMU/LBR，M7 补齐高级命令和发布工程。

## M2：CPU sampling 核心

### 目标

实现可用的：

```powershell
wperf record -F 999 -g -o cpu.etl -- app.exe args
wperf report -i cpu.etl --sort dso,symbol
```

### 交付物

- `record -- command`：controller 启动普通权限 target，worker 负责 ETW。
- `record -p PID`：attach 已存在进程，报告阶段按 PID/线程过滤。
- ETL consumer：`OpenTraceW + ProcessTrace + CloseTrace`。
- classic system event decoder：
  - Process/Thread DCStart/Start/End
  - ImageLoad/ImageDCStart/ImageUnload
  - SampledProfile
  - StackWalk
- profile interval 和采样权重。
- StackWalk 与 sample 的关联：
  - `EventTimeStamp`
  - `StackThread`
  - 必要时校验 PID
- CPU 聚合模型：
  - self samples
  - inclusive samples
  - CPU%
  - per-process/thread/CPU/module/symbol
- `report` 基础表格和 CSV/JSON。
- trace quality：
  - samples
  - events lost
  - stack missing
  - kernel stack partial

### 技术任务

1. `src/io/etl_reader.*`：封装 `OpenTraceW/ProcessTrace`。
2. `src/etw/classic_events.*`：Classic event 布局与版本表。
3. `src/etw/stackwalk.*`：StackWalk payload 解码。
4. `src/model/sample.*`、`process.*`、`thread.*`、`image.*`。
5. `src/analysis/cpu_report.*`：flat aggregation。
6. `src/render/table.*`、`json.*`、`csv.*`。
7. `commands.cpp` 增加 target process launch/attach。
8. 增加 M2 golden trace 测试负载和对比脚本。

### 验收门槛

- 单线程和多线程 CPU burn 都能生成 sample + stack。
- 报告中能看到 PID、TID、CPU、module、symbol。
- 与 xperf `-a profile -detail` 比较，样本总数差异可解释。
- top-N symbol 集合基本一致。
- 丢失事件、缺失栈和未知模块必须可见。
- 无符号时仍输出 module+offset。

### 依赖与范围

- 依赖 M0 的管理员 xperf baseline。
- M2 不做 PDB 符号化，只输出 RVA/module/address；函数名在 M3 完成。
- M2 不做 `top` 实时 UI；M4 完成。

## M3：PDB 符号化和 report 体验

### 目标

把 M2 的地址报告变成可用的函数级 profiler：

```powershell
wperf report -i cpu.etl --sort dso,symbol
wperf report -i cpu.etl --folded > stacks.folded
wperf report -i cpu.etl --call-graph caller
```

### 交付物

- PE debug directory 解析：
  - timestamp
  - image size
  - checksum
  - CodeView GUID/PDB age
- 模块地址映射：VA -> module -> RVA。
- DbgHelp 接入：
  - SymInitializeW
  - SymLoadModuleExW
  - SymFromAddrW
  - SymGetLineFromAddrW64
- symbol path：
  - 本地 PDB
  - `_NT_SYMBOL_PATH`
  - Microsoft symbol server
- 磁盘 symbol cache：`%LOCALAPPDATA%\wperf\symbols`。
- 符号 worker 和串行 DbgHelp 保护。
- module+offset 降级。
- inline frame 基础支持。
- report 功能：
  - sort
  - percent-limit
  - pid/tid/comm/dso/symbol 过滤
  - caller/callee call tree
  - folded stack
  - CSV/JSON
- `buildid-list` 基础版本。

### 验收门槛

- 有 PDB 时输出函数名；可用时输出文件/行号。
- 无 PDB 时稳定输出 `module+offset`。
- 符号缓存命中后不重复加载 PDB。
- 同一份 trace 重复 report 结果稳定。
- folded stack 可被 FlameGraph 类工具消费。
- 大 trace 符号化不会一次性加载全部事件。

## M4：实时 `top`、`stat` 和 `script`

### 目标

形成交互和快速统计工作流：

```powershell
wperf top -p 1234
wperf stat -p 1234
wperf script -i cpu.etl --fields pid,tid,dso,symbol
```

### 交付物

- ETW real-time consumer。
- bounded MPSC ring buffer。
- 实时样本聚合窗口。
- `top` ANSI 表格：
  - self overhead
  - process/comm/dso/symbol
  - 1 秒刷新
- 实时符号解析 worker；未解析时显示 module+offset。
- `stat`：
  - elapsed
  - user/sys
  - task-clock
  - cycles / thread-cycles
  - context switch/page-fault 计数（通过 ETW）
- `script`：
  - 字段选择
  - 简单过滤
  - text/CSV/JSON
- Ctrl-C、窗口 resize、终端无 VT 的降级。

### 验收门槛

- `top` 在 1 kHz 采样下持续刷新且可 Ctrl-C 退出。
- worker/guard 正常清理。
- `stat` 时间数据与系统 API 交叉验证。
- 实时消费不阻塞 ETW callback。
- ring 满时显示 dropped events，不静默丢样本。

## M5：调度、syscall 和 I/O tracing

### 目标

复刻常见 `perf trace/sched/off-CPU` 工作流：

```powershell
wperf trace -e sched,syscall,pagefault -p 1234
wperf sched -i trace.etl
wperf report -i trace.etl --kind=io
```

### 交付物

- CSwitch / ReadyThread 解码。
- 每 CPU 调度时间线。
- 每线程：
  - running
  - ready/runnable
  - blocked
  - wait reason
- wakeup -> run latency。
- off-CPU stack（按需为 CSwitch 开栈）。
- Syscall enter/exit 解码。
- PageFault 解码。
- Disk/File IO init/complete 关联。
- Network/Registry/ALPC/DPC/ISR 基础事件。
- `trace` CLI：
  - 事件别名
  - PID/TID 过滤
  - live text
  - JSON/CSV。
- `report --kind=sched/io`。
- TDH manifest provider 解码和 `--provider GUID`。

### 验收门槛

- CSwitch/ReadyThread 事件可构建稳定时间线。
- 调度延迟、blocked time 可输出。
- I/O init/complete 无法关联时显示 orphan。
- syscall 报告明确只展示 ETW 暴露的字段。
- 高流量事件可限制时长和过滤。
- 与 xperf `cswitch/readythread/diskio` 基准报告对比可解释。

## M6：PMC、LBR 和硬件能力

### 目标

实现 perf-like 的硬件计数实验能力：

```powershell
wperf list --source
wperf stat --pmu -e cycles,instructions -p 1234
wperf record -b -o branch.etl -- app.exe
```

### 交付物

- `TraceProfileSourceListInfo` / `TraceMaxPmcCounterQuery`。
- `TracePmcSessionInformation` / `TracePmcCounterOwners` 冲突检测。
- `TraceProfileSourceConfigInfo`。
- `TraceSampledProfileIntervalInfo`。
- `TracePmcEventListInfo` / `TracePmcCounterListInfo`。
- `stat --pmu`：
  - source 映射
  - 单位
  - sample/estimated/total 标签
  - strict/降级模式。
- LBR：
  - `TraceConfigureLastBranchRecord`
  - from/to 地址解码
  - branch filter
  - symbolization。
- context register：
  - `TraceContextRegisterInfo` 能力探测
  - IP/SP 补充样本。
- VM/无 PMU 环境的安全降级。

### 验收门槛

- 运行时可列出本机 PMC source 和 max counters。
- PMC 被占用时给出明确冲突信息。
- `stat --pmu` 不把近似值伪装成精确 total。
- LBR 功能在不支持的平台明确 unsupported。
- 所有全局配置在停止时恢复。
- 1 kHz profile 下无异常句柄/session 泄漏。

## M7：高级分析、兼容和发布

### 目标

补齐日常 perf 工具链并达到可发布状态。

### 交付物

- `annotate`：
  - symbol/address
  - instruction heat
  - 可选 Capstone/Zydis 反汇编
- `diff`：
  - 两次 report 比较
  - symbol/process/module 变化。
- `archive`：
  - 收集 PE/PDB/module identity
  - 可选 bundle。
- `buildid-list` 完整版。
- `provider` 列表与过滤。
- Windows 10/11 x64 兼容矩阵。
- JSON schema 版本化。
- 性能优化：
  - 并行解码
  - mmap/索引
  - stack intern
  - symbol cache。
- fuzz、恶意 trace、权限和崩溃恢复测试。
- 单 EXE 发布、README、CLI 文档、troubleshooting。

### 验收门槛

- CPU profile + symbol + folded/calltree 完整工作流。
- sched/trace/stat/script/annotate/diff 命令可组合使用。
- 无 WPA 安装也能完成离线报告。
- 1 GB 级 trace 不一次性加载全部事件。
- 普通权限 report/annotate 可用。
- 不写驱动、不修改目标程序。

## 里程碑与命令对应

| 阶段 | 新增主要命令 | 主要能力 |
|---|---|---|
| M2 | `record -- cmd`, `report -i` | sample + stack + module attribution |
| M3 | `report --folded/--call-graph`, `buildid-list` | PDB + function/report |
| M4 | `top`, `stat`, `script` | real-time + quick counters |
| M5 | `trace`, `sched`, provider tracing | scheduling/syscall/io |
| M6 | `stat --pmu`, `record -b` | PMC/LBR/context |
| M7 | `annotate`, `diff`, `archive` | advanced analysis/release |

## 保真度策略

| 保真度 | 含义 | 例子 |
|---|---|---|
| exact/direct | 使用对应系统 API，语义接近 | CPU timer sample、sched 事件 |
| approximate | 结果可用于定位，但与 Linux 语义不同 | PMC stat、syscall 参数、LBR |
| extended | 需要 provider 或 instrumentation | .NET/JIT、custom tracing |
| unsupported | 不写驱动/不改程序无通用路径 | uprobe、kprobe、完整 perf mem/c2c |

每个新命令必须在 `--help` 和 README 中标注保真度。

## 测试和对比策略

### Golden trace

- 使用 xperf 生成 CPU、sched、I/O、PMC trace。
- 每个 milestone 保留一份小型、可重复的基准 trace。
- 运行报告后比较：
  - 事件数
  - sample 数
  - top-N symbol
  - 调度延迟分布
  - I/O 延迟聚合
- 不要求内部二进制逐事件完全一致，但差异必须解释。

### 回归测试

- 单元：schema、时间、栈关联、聚合、缓存。
- 集成：真实 ETL replay。
- 性能：1 kHz、8 线程、16 逻辑 CPU。
- 异常：无符号、无 PDB、无 PMC、无 LBR、截断 ETL。
- 权限：普通 report、管理员 capture、UAC worker。

## 团队和时间

- 单人：M2 3 周、M3 2 周、M4 2 周、M5 4 周、M6 4 周、M7 4 周，约 5～6 个月。
- 两人：按 M2/M3、M5/M6 两条线并行，约 3～4 个月。
- 时间估算不包含 .NET/Java/V8 JIT 和驱动级 instrumentation。

## M2 立即执行顺序

1. 完成 Issue #1 的管理员 M0/M1 验收。
2. 实现 ETL reader 和 replay 测试。
3. 实现 classic process/thread/image/profile/stack decoder。
4. 实现 sample/stack 合并和 trace quality。
5. 扩展 `record -- command` 和 `record -p PID`。
6. 实现 flat CPU report。
7. 用 xperf golden trace 做 top-N 对比。
8. 合并 M2，随后进入 PDB 符号化 M3。

## 明确后置的能力

以下不阻塞 M2-M7：

- 完整 `perf mem` / `perf c2c`。
- kprobe/uprobe。
- Intel PT 连续分支记录。
- 内核驱动 instrumentation。
- 所有 JIT 语言栈。
- GUI/Web UI。
- 分布式采集。
