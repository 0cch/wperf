# wperf 进一步完善计划

> 依据：`docs/REVIEW_PLAN.md` 的初步结论。  
> 约束：不编写驱动、不访问 MSR、不修改 Windows 内核、不对目标程序做未声明 instrumentation。  
> 当前定位：已经完成 M0-M7 的纵向闭环，但距离完整 Linux perf 仍有明显 fidelity 缺口。  
> 执行原则：先提高数据可信度，再做实时交互，最后做硬件特性和高级分析。

## 1. 总目标

把 wperf 从“能采、能报告”提升为“对 CPU、调度、系统事件和硬件计数给出可信边界”的轻量 Windows profiler/tracer：

1. 采样和栈结果可重复、可对照、可解释。
2. live `top/trace` 不依赖反复写入 ETL，而是使用持续 ETW consumer 和 bounded ring。
3. 所有 unsupported/approximate 能力在 CLI、报告和文档中标明。
4. 继续保持单 EXE、CLI 优先、无 WPA/WPR 运行时依赖。
5. 继续使用系统能力：ETW、Win32、DbgHelp、Toolhelp、realtime API。

## 2. 不写驱动的硬边界

以下能力不纳入本计划，不作为“待实现功能”：

- 直接 RDMSR/WRMSR 或自行编程 PMU。
- Intel PEBS、AMD IBS 的原始数据解析。
- 任意 kprobe/uprobe 动态插桩。
- 连续 Intel PT 级分支 trace。
- 任意 memory access 地址和 cacheline sharing 分析。
- 完整 Linux `perf_event_open` 的 per-process PMU 组调度语义。
- 不修改目标程序就获得完整函数调用图。

ETW 暴露不到的字段只能：
- 标为 unsupported；
- 使用 ETW 的近似数据；
- 或者使用显式可选的应用 ETW provider/instrumentation。

## 3. 优先级定义

- **P0：正确性和回归基础**，不修就会让后续功能建立在不可信数据上。
- **P1：核心体验和 fidelity**，决定是否真正接近 perf 日常使用。
- **P2：硬件特性和高级分析**，受平台影响大，保持实验性。
- **P3：可选优化**，等真实需求出现再做。

## 4. P0：正确性和回归基础

### P0.1 建立 golden ETL fixture

保存一组小型、可重复、许可清晰的 ETL：

- Native CPU sample + stack
- cpu-sched
- pagefault
- syscall
- file/disk I/O
- PMU sample
- 有/无 PDB 的 native executable
- 32-bit/WOW64 样本
- malformed/truncated ETL

要求：

- 不依赖管理员权限即可 replay。
- 每个 fixture 带 `expected.json`：事件数、sample 数、stack 覆盖率、模块数、top-N symbol。
- 对比结果允许版本差异，但必须记录差异原因。

### P0.2 StackWalkRef / StackWalkDef 精确关联

当前 sample 与 stack 关联使用 leaf IP 和线程/CPU fallback，存在误关联风险。

实现：

- 解析现代 `StackWalkRef/User`、`StackWalkRef/Kernel`。
- 解析 `StackWalkDef/Create`、`StackWalkDef/Delete`、`StackWalkDef/Rundown`。
- 使用可靠 key/StackId 关联 sample、stack definition 和引用事件。
- 移除或降级 leaf-IP/FIFO heuristic。
- 输出 stack quality：
  - `stack_complete`
  - `stack_missing`
  - `stack_ambiguous`
  - `stack_key_mismatch`

验收：

- golden CPU trace 的 stack 覆盖率达到可解释水平。
- folded/calltree 不再出现明显错误的调用父子关系。
- 没有 stack 的样本明确计数。

### P0.3 模块身份和 PDB 校验全链路

- 模块 key 使用：
  - PID
  - image base
  - image size
  - PE timestamp
  - checksum
  - PDB GUID/age
- 模块卸载/重载后旧地址不能被新模块符号命中。
- DbgHelp 加载失败明确返回 module+offset。
- 符号缓存 key 和报告缓存 key 一致。
- 增加 PDB 错误/过期/缺失测试。

### P0.4 Trace decoder hardening

- 所有 event payload 读取验证长度和 alignment。
- 对 32-bit/64-bit pointer size 分流测试。
- 对未知 provider/class/opcode 不崩溃。
- 对截断、恶意长度、异常 GUID 做 fuzz。
- 每个 Windows build 的 classic event schema 维护版本适配表。

### P0.5 命令和代码模块化

当前 `commands.cpp` 过大，采集、解析、UI、报告和高级命令混在一起。

拆分方向：

```text
commands/
  record.cpp
  report.cpp
  stat.cpp
  top.cpp
  trace.cpp
  sched.cpp
  annotate.cpp
  diff.cpp
  archive.cpp
```

同时抽离：

- `trace/event_decoder.*`
- `trace/trace_quality.*`
- `live/ring_buffer.*`
- `symbol/symbol_cache.*`

验收：新增功能不需要继续修改一个巨型源文件。

## 5. P1：Native profiling fidelity

### P1.1 Inline frame 和行号

- 使用 DbgHelp inline API 展开 inline frame。
- 保留 outer/self/inclusive 关系。
- 报告中显示 source file、line、inline depth（可用时）。
- 无 PDB 时仍稳定输出 module+offset。

### P1.2 符号缓存

- 先实现 in-memory cache。
- 再实现可选磁盘 cache，key 使用模块身份而不是路径。
- cache 失效条件：
  - timestamp/size/checksum 改变；
  - PDB GUID/age 改变；
  - symbol path 改变。
- 默认不做 source server 命令执行。

### P1.3 报告权重和调用树

- sample 权重使用实际 interval。
- 对丢失事件、缺失栈、未知符号进行质量归一。
- calltree 支持：
  - self/inclusive
  - caller/callee
  - recursive dedup
  - min hits/percentage filter
- `diff` 先支持 weighted symbol diff，再支持 calltree diff。

### P1.4 进程、线程、模块归属

- 解析 PID/TID 生命周期，避免线程 ID 复用误归属。
- CSwitch/ReadyThread 用线程开始/结束时间解析 PID。
- 对系统线程、Idle、System 做专门命名。
- I/O 和 syscall 的归属无法确定时显示 unknown，而不是猜测。

验收：

- golden trace 的 top-N symbol 与本机 xperf 对照可解释。
- native target 输出 `Burn` 等真实热点。
- 无效/过期 PDB 不会产生错误函数名。

## 6. P1：Live consumer 和交互体验

### P6.1 持续 real-time ETW consumer

在 elevated headless worker 内：

- 使用 `EVENT_TRACE_REAL_TIME_MODE`。
- 使用 `OpenTraceW + ProcessTrace` 持续消费。
- producer callback 只做 bounded ring enqueue。
- consumer 线程聚合样本。
- controller（medium）通过 IPC 接收聚合快照，保持 UI 在普通权限侧。

### P6.2 `top` 改为真实实时

- 取代当前 snapshot capture。
- 默认 1 秒窗口。
- 支持 `-p PID`、`-e`、排序、百分比阈值。
- 未解析符号先显示 module+offset，后台符号化后替换。
- ring 满时显示 `events_dropped`。
- Ctrl-C 只停止 consumer，不留下 ETW session。

### P6.3 `trace` 支持 live

- `wperf trace -e sched/syscall/pagefault/io -p PID`
- 实时 stdout/CSV/JSON。
- `--duration`、`--max`、事件过滤。
- 离线 `-i` 继续保留。

### P6.4 稳定性

- 1 小时持续运行测试。
- 句柄、内存、ETW buffer、session guard 检查。
- 终端无 VT 时降级为滚动输出。
- worker/controller 断开时自动停止 capture。

## 7. P1：调度、syscall、I/O fidelity

### P7.1 调度

- CSwitch 解析 New/Old TID，并归属对应 PID。
- ReadyThread 关联目标 TID。
- 计算：
  - running
  - ready
  - blocked
  - wait reason
  - wakeup latency
- 对无法归属的系统线程保留 raw TID 和 unknown 状态。

### P7.2 Syscall

- 解码 `SysCallEnter.SysCallAddress`。
- syscall exit 只显示 ETW 提供的状态/数据。
- 明确不承诺 Linux 参数级语义。
- 若 provider 没有 PID/TID，不伪造归属。

### P7.3 I/O

- File/Disk init/complete 关联。
- 计算延迟、字节数、操作类型。
- 无法关联的事件计入 orphan。
- 支持 `trace -e io` 和 `report --kind=io`。
- 对网络、注册表、ALPC 逐步加入同类关联。

验收：

- 页错误栈可选开启。
- sched 报告没有无解释的 unknown PID 统计。
- I/O 报告有 init/complete/orphan 三项质量指标。

## 8. P2：PMC、context register、LBR

### P8.1 PMC/PMU

- 查询 `TraceProfileSourceListInfo`。
- 查询 `TraceMaxPmcCounterQuery`。
- 检查 `TracePmcCounterOwners`、`TracePmcSessionInformation`。
- `stat --pmu` 标明 sample-derived/estimated/total。
- 不能保证精确总量时，不使用 `perf stat` 同义字段冒充精确值。
- 停止后恢复 Timer source。

### P8.2 Context register

- 增加 `--context-registers` 可选配置。
- 使用 `TraceContextRegisterInfo`。
- 捕获 IP/SP/控制/整数寄存器（平台支持时）。
- 失败时明确降级，不阻塞普通采样。

### P8.3 LBR

- 使用 `TraceConfigureLastBranchRecord`。
- 先做 capability probe。
- 在支持的 Intel 平台解析 from/to 地址和分支过滤。
- 不支持的平台返回 `unsupported`。
- 明确 LBR 深度有限，不是 Intel PT。

验收：

- 当前 AMD 机器 LBR unsupported 是合法结果。
- 有支持设备的测试记录。
- context register/LBR 不会被标记为默认稳定能力。

## 9. P2：高级分析和发布

### P9.1 Annotate

- 第一阶段保留 address heat。
- 第二阶段引入可选 Capstone/Zydis：
  - 指令反汇编
  - instruction heat
  - source line
  - inline frame
- 反汇编库作为可选构建特性，不阻塞基础 EXE。

### P9.2 Diff

- weighted symbol diff
- calltree diff
- process/module filter
- JSON/CSV 输出

### P9.3 Archive

- 默认只归档实际引用模块。
- 支持 `-p PID`。
- 增加体积上限、PDB identity、symbol path 记录。
- manifest 可复现实验环境。

### P9.4 Provider 和自定义 tracing

- `wperf list providers`
- `wperf trace --provider GUID`
- TDH schema/字段输出
- EventSource/WPP provider 基础支持

### P9.5 发布

- `build_release.ps1` 生成 EXE/PDB/README/release manifest。
- SHA256 和 version 写入 release。
- Windows 10/11、x64 基础兼容矩阵。
- 单 EXE、无 WPA/WPR 运行时依赖。

## 10. 自有 `.wperf` 格式的决策计划

### 结论

**当前不做主格式迁移。**  
ETL 仍是原始采集格式；`.wperf` 只能作为后续可选派生格式。

### 推荐阶段

1. 先稳定 ETL replay、StackWalkRef、符号身份和 live consumer。
2. 观察真实 trace 的 report 时间、符号重复解析成本和随机访问需求。
3. 再实现 `wperf import`：ETL -> `.wperf`。
4. `.wperf` 作为 report/annotate/diff 的快速索引和可移植缓存。
5. 永远保留 ETL 导入/导出，避免失去 xperf/WPA/PerfView 互操作。

### 触发自有格式的条件

满足任一条件才建议投入：

- 1 GB 以上 ETL 重复 report 时间明显影响使用。
- 符号化在每次报告重复产生明显延迟。
- annotate/diff 需要频繁随机访问。
- 需要把 trace、模块身份、符号元数据打包交付。
- 需要跨机器分析且 ETL 路径/符号服务不稳定。

### `.wperf` 设计草案

```text
Header
  magic/version/endianness
  Windows build/architecture
  capture metadata
Section table
  process/thread/image tables
  interned strings
  interned stacks
  event table
  sample table
  trace quality
  symbol cache
Indexes
  timestamp
  pid/tid
  cpu
  event type
  module
```

压缩可选 zstd。格式升级必须向后兼容，并保留损坏检测。

## 11. 推荐执行顺序

| 阶段 | 内容 | 估算 |
|---|---|---:|
| R0 | golden ETL、StackWalkRef、模块身份、decoder fuzz、commands 拆分 | 3～4 周 |
| R1 | inline frame、symcache、报告权重、PID/TID 归属 | 2～3 周 |
| R2 | real-time top/trace、bounded ring、live 稳定性 | 3～4 周 |
| R3 | sched、syscall、I/O init/complete、trace quality | 3～4 周 |
| R4 | PMC 语义、context register、LBR probe/decode | 3～4 周 |
| R5 | annotate 反汇编、weighted diff、archive/provider | 3～4 周 |
| R6 | 兼容性、安全、发布、文档 | 1～2 周 |

单人约 4～6 个月；两人可将 R2/R3 与 R4/R5 并行，约 3～4 个月。R4 的 LBR/PMU 验证依赖硬件。

## 12. Issue 映射

- Issue #3：纳入 R0/R1；其中 StackWalkRef 优先于 inline/symcache。
- Issue #4：纳入 R2/R3；real-time 是 M4 的主线。
- Issue #5：纳入 R4/R5；保持硬件实验性质。
- 新建议 Issue：
  - P0 golden ETL regression
  - P0 exact StackWalkRef
  - P1 real-time consumer
  - P1 I/O init/complete
  - P1 inline/symcache
  - P2 PMC semantic calibration
  - P2 LBR hardware validation

## 13. 验收指标

### 正确性

- golden trace 的 sample/event 数量稳定且可解释。
- stack 缺失和 unknown 明确统计。
- top-N symbol 与 xperf 对照一致或差异可解释。
- 过期 PDB 不产生错误符号。
- 截断/恶意 ETL 不崩溃。

### 性能

- 1 kHz CPU sampling，8 线程负载采集开销目标 < 5%。
- 默认采集无事件丢失；有丢失时显示 `events_dropped`。
- 1 GB trace report 采用流式处理，不一次性加载全部事件。
- symbol cache 命中后不重复解析 PDB。

### Live

- `top` 持续 1 小时无 session/handle 泄漏。
- Ctrl-C 后无 `wperf-*` session 残留。
- controller/worker 异常退出时 guard/recover 可清理。

### 兼容性

- Windows 10 21H2+、Windows 11 x64。
- Intel/AMD/虚拟机分别记录 PMC/LBR/context register 支持状态。
- 32-bit/WOW64 至少 replay 不崩溃，完整符号化另列支持范围。

## 14. 明确不做

- 不通过驱动补足 ETW 缺失能力。
- 不在第一阶段实现完整 `.wperf` 写入器。
- 不在当前 AMD 环境伪造 LBR 支持。
- 不把 PMU sample 结果说成精确 cycles/instructions total。
- 不为 `perf mem/c2c` 生成误导性报告。
- 不把 JIT、uprobe、kprobe 作为 M0-M7 的既有承诺。

## 15. 建议下一步

先执行 R0，不先做实时 UI 或自有格式：

1. 建立 golden ETL fixture。
2. 解析 StackWalkRef/StackWalkDef。
3. 固化模块身份校验和 symbol failure 统计。
4. 增加真实 ETL replay CI。
5. 拆分 `commands.cpp` 的边界。

R0 完成后再决定是否先做 R1（符号质量）还是 R2（实时 top/trace）。在 R0 之前引入 `.wperf`，很可能只是把未验证的解析结果固化下来，收益低、维护成本高。
## 16. R0 当前状态

R0 的核心正确性基线已经落地，详见 `docs/R0_STATUS.md`：

- CPU sample + stack 的普通权限 golden ETL 与自动 replay 断言。
- raw timestamp + QPC/TID/CPU 精确栈关联，以及 StackWalkRef/Def 解析。
- PE timestamp/checksum/RSDS PDB GUID/age 校验和模块 synthetic base 隔离。
- 非 ETL 与截断 ETL 回归。
- M7 和 offline 命令已从 `commands.cpp` 拆出。

仍待补强：压缩栈专用 fixture、WOW64、fuzz corpus、symbol failure 统计，以及 `stat/top` 进一步拆分。这些不阻塞 R1。
