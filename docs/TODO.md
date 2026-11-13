# wperf 后续待办与验收计划

更新日期：2026-11-13。整理基线：`main` 的 `a8e9033`。

本文集中维护项目评估后仍未完成的工作，覆盖硬件验证、兼容性、长时间运行与采样扰动、off-CPU、JIT、低权限符号解析以及外部 profiler 共存边界。这里列出的是待实施或待验收计划，不表示对应能力已经完成。

## 1. 当前状态与使用方式

已有 CPU/sched live integration、故障清理、ETL 回放、inline/PDB/cache、archive 和分析回归。`10825e8` 的两种 CI 配置均为 37/37 通过，证据见 [CI run](https://github.com/0cch/wperf/actions/runs/43410931662)。这份历史结果不能代替后续代码或目标硬件的验收。

- [MAIN_FOLLOWUPS.md](MAIN_FOLLOWUPS.md)：已经实现的改进、现有接口和限制。
- [ISSUE_STATUS.md](ISSUE_STATUS.md)：Issue #3/#4/#5 的验收状态；#3/#4 已关闭，#5 仍开放。
- [ASSESSMENT_STATUS.md](ASSESSMENT_STATUS.md)：`256eb66` 的历史审计，不是当前 main 的完成率。
- 本文：后续任务的统一入口；任务完成时在本文登记实现提交和验收证据，并同步相关状态文档。

任务状态分为“待实施”“待实测”“进行中”“已验收”“平台限制”。任务只有同时具备实现、适用范围内的验收证据和限制说明，才能标为已验收。勾选框均代表尚未完成的后续事项；既有基础放在各任务的“当前基础”中说明。

优先级定义：P0 为当前 Issue #5 的验收缺口；P1 为稳定性、兼容性和权限架构；P2 为新增分析能力。建议从可在现有环境开发的测试工具开始，同时准备支持相应能力的硬件。优先级不意味着所有工作必须串行。

| 编号 | 工作 | 当前状态 | 优先级 | 主要依赖 |
|---|---|---|---|---|
| HW-01 | Intel LBR 真实采集 | 待实测 | P0 | 支持 ETW LBR 的 Intel 机器、管理员权限 |
| HW-02 | PMC 采样与快照校准 | 待实测 | P0 | 目标 CPU/OS 暴露所需 source、管理员权限 |
| HW-03 | context schema、解码与组合采集 | 待实施、待实测 | P0 | 真实 payload、可验证的 schema、支持硬件 |
| VAL-01 | OS/CPU/VBS/HVCI/WOW64/group 矩阵 | 待扩展验证 | P1 | 多种系统和设备 |
| VAL-02 | 真实现代缓存栈 ETL | 待扩展验证 | P1 | 支持 native stack cache 的采集环境 |
| RUN-01 | 长时间 soak 与故障压力 | 脚本已有基础，待完善、待实测 | P1 | 管理员权限、持续负载和指标采集 |
| RUN-02 | 采样扰动与容量曲线 | 待实施、待实测 | P1 | 可重复 workload、固定环境 |
| SEC-01 | live symbolization 移出提权 worker | 待实施 | P1 | 结构化 IPC、生命周期元数据传输 |
| ANA-01 | off-CPU 栈与等待火焰图 | 待实施 | P2 | 调度栈采集、区间关联器 |
| ANA-02 | JIT 方法映射与回放 | 待实施 | P2 | 选定运行时、方法生命周期事件 |
| CO-01 | 外部 profiler 冲突与恢复边界 | 部分机制已有，待实测 | P1 | 可控的第二采集器、故障注入 |
| REL-01 | 持续验证与发布证据 | 通用 CI 已有，待补专项门槛 | P1 | 上述任务的脚本和结果 |

## 2. 硬件验证

### HW-01：支持 Intel LBR 的真实采集

**当前基础：** LBR 配置、payload 解码、CPU/PMC trigger 和空采集失败检查已实现；仍缺受支持 Intel 平台的采集证据。当前记录的本机环境是 AMD Ryzen 7 8700G，不能完成此项验收。

实施与验证：

- [ ] 获取支持相应 Windows ETW LBR 能力的 Intel 机器，记录 OS build、CPU 型号、固件、VBS/HVCI、逻辑核心和权限状态；不能仅根据“Intel”品牌判定支持。
- [ ] 编写确定性分支 workload，保存匹配的二进制、PDB 和预期控制流；考虑优化、ASLR 和模块加载地址。
- [ ] 分别验证 CPU 定时采样触发与 PMC 中断触发，保存实际配置结果和非空 LBR ETL。
- [ ] 核对分支 from/to 地址、方向/排序、有限深度、PID/TID、CPU、时间及模块归属；只验证平台确实提供的字段。
- [ ] 比较 record 后离线解码与 live 输出的语义；因窗口或过滤不同，不能要求二者事件数量机械相等。
- [ ] 覆盖支持、明确不支持、配置成功但无有效记录三类结果；支持环境失败不得被笼统记为 SKIP。

**完成标准：** 目标 workload 实际产生可解释的分支记录，地址与控制流对照通过，实时/离线语义一致，失败路径明确；有原始 ETL、环境和自动核对结果。LBR 深度有限，不等同于 Intel PT 全路径追踪。

**相关代码：** `src/etw.cpp`、`src/lbr.*`、`src/etl_reader.cpp`、`src/commands/live_worker.cpp`。

### HW-02：PMC 中断采样和 CSwitch 快照校准

**当前基础：** 两种采集模式已分离；保存 source 名称和顺序；快照按 CPU/source 差分，并在身份已知时归属旧线程。输出保留 `exact:false` 和 `event-snapshot-derived`。

实施与验证：

- [ ] 保存 `list`、`doctor`、source ID/名称、支持区间和实际配置；source 数值不能跨机器直接套用。
- [ ] 建立计算、分支、缓存/内存访问的可重复 workload，并覆盖单线程固定 CPU、多线程、CPU 迁移及短运行。
- [ ] 中断采样：改变 `--period`，检查 native period、source、目标归属及采样数量趋势；重跑以评估波动，不能把中断数当成精确事件总数。
- [ ] CSwitch 快照：核对 source 列表顺序、同 CPU/source 差分、旧线程归属和未知身份处理，确认不跨 CPU 相减。
- [ ] 验证计数下降、reset、缺失事件和区间边界的处理；未验证的 wrap/reset 不补造总量，必要时扩展质量计数。
- [ ] 用独立的系统或厂商工具、可解释的 workload 趋势交叉校准；保存参考工具版本、配置和指标口径。存在资源冲突时采用分开的重复运行。
- [ ] 写出每种 source/平台的误差与适用范围，说明中断活动可能混入快照区间；不宣称 perf-event group、精确计数或 multiplex scaling。

**完成标准：** 两种模式各自具有真实采集结果；source/period/归属正确，趋势与参考结果在预先定义的容差内，异常区间可识别，所有近似语义保留。

**相关代码：** `src/etw.cpp`、`src/etw_decode.cpp`、`src/analysis.cpp`、`src/commands/offline.cpp`。

### HW-03：Context 寄存器 schema 与 LBR 组合验收

**当前基础：** context 配置入口已有，payload 解码仍禁用。配置 API 返回成功不是寄存器内容已经可用的证明。

实施与验证：

- [ ] 在支持的机器上保存真实事件及 extended data，登记 provider、event/version、长度、位宽和 OS build。
- [ ] 根据可获得的 SDK/TDH/schema 与真实记录确认寄存器集合、布局、标志和含义；无法确认的版本继续标为 unsupported。
- [ ] 创建可控的原生 workload，验证采样上下文中可核对的 IP/SP 等字段；结合反汇编或独立采集证据解释寄存器值。
- [ ] 实现按明确版本/架构识别的 decoder 和数据模型，增加长度、截断、未知版本与无效字段回归。
- [ ] 定义寄存器与 CPU/PMC sample、LBR 的关联键及歧义处理，禁止用时间邻近伪装精确关联。
- [ ] 验证 context 单独启用、与 LBR 组合启用、配置失败及有效数据为零；报告明确区分请求、配置和实际数据状态。

**完成标准：** 至少在声明支持的平台上，真实 schema、寄存器含义、sample/LBR 关联及异常处理均有证据；未验证组合仍明确列为不支持或待验证。

**依赖：** schema 证据先于正式 decoder 启用；与 HW-01/HW-02 组合验收。此项和 HW-01/HW-02 共同构成 Issue #5 当前的未完成验收项。

## 3. 兼容性和真实栈验证

### VAL-01：分层建立平台矩阵

- [ ] 登记实际测试的 Windows 10/11 版本和 build；先覆盖声明支持范围的代表组合，不能将一个 runner 外推到全部系统。
- [ ] 覆盖 Intel/AMD、VBS/HVCI 开关；记录虚拟化与安全配置及能力变化。
- [ ] 覆盖原生 x64 与 WOW64 目标，核对地址位宽、内核/用户栈、模块身份和符号；当前“不崩溃”不等于完整 WOW64 符号支持。
- [ ] 在具有多个 processor group 的设备上验证 CPU 索引、选择器、PMC 差分和栈关联，不能仅用合成 CPU index 代替实机验收。
- [ ] 在每个组合分别记录 CPU、sched/I/O、PMC、LBR、context、符号解析和清理结果，状态采用 PASS/FAIL/UNSUPPORTED/NOT-RUN。
- [ ] 更新 `docs/COMPATIBILITY.md`，区分有实测证据的能力、仅回归覆盖的能力和未知组合。

**完成标准：** 已声明验收的组合可复现；缺少的组合显式保留，不要求无差别覆盖全部排列后才能交付一个有限支持范围。

### VAL-02：真实现代缓存栈 ETL

- [ ] 使用 native `record --stack-cache` 取得含 StackWalkRef/Def 的真实 ETL，确认事件类型确实存在。
- [ ] 尽可能捕获 delete/rundown、key 重用、用户/内核栈及进程退出场景，核对 definition epoch 和地址归属。
- [ ] 对照系统工具或可控 workload 验证展开结果，记录 exact、approximate、missing、ambiguous 和 key mismatch。
- [ ] 保存可重复回放的 fixture、环境和预期结果；不能公开原始记录时保存访问位置、hash 和生成步骤。

**完成标准：** 原始 record 回归之外新增真实缓存栈采集证据。此项属于 #3 的扩展验证，不改变其既有关闭依据。

## 4. 持续运行与采样开销

### RUN-01：长时间 soak 和故障压力

**当前基础：** `scripts/r2_soak.ps1` 默认运行 60 分钟，每 30 秒记录 controller/worker 的 Working Set、句柄和累计 CPU，并检查退出码及残留 top session。它尚未自动判断内存增长、丢事件、输出延迟等指标，因此当前脚本的 `PASS` 不代表完整稳定性验收。

实施与验证：

- [ ] 扩展脚本以可靠登记 controller、worker、guard 进程身份和存活状态；不能只依赖一次父进程查询漏掉 worker。
- [ ] 增加 Private Bytes、线程数、CPU 增量、峰值、输出进度/延迟、loss、ring/metadata drops、correlation eviction 和缓存统计；指标缺失不得默认为零。
- [ ] 将预热期与稳定期分开，计算资源增长趋势，并保存原始指标和图表；清楚区分管理数据预算与进程总 RSS、DbgHelp 和内核消耗。
- [ ] 覆盖 top、live sched/I/O、长 record 和反复 start/stop；使用进程/线程 churn、DLL 加载卸载、大量不同符号与文件路径的负载。
- [ ] 注入慢输出/管道反压、符号不可用、下载超时、磁盘空间不足、controller/worker 异常退出等场景。
- [ ] 检查输出仍有进展、STOP 及时处理、worker/guard 最终退出、对应 session 消失和已知 interval 的条件恢复。
- [ ] 为脚本增加运行超时与失败判定，不能测试自身永久等待；清理只针对本次运行拥有的进程/session。
- [ ] 依次执行建议的 1 小时、8 小时、24 小时层级并归档结果；这些是计划层级，尚未有本轮对应验收报告。

**门槛制定：** 先采集基线，再为指定环境和 workload 固定资源趋势、CPU、输出延迟、loss/drop 与停止时间容差。运行前写入配置，禁止看到结果后调整门槛来判 PASS。预期过载场景可允许显式丢弃，但计数与降级必须正确。

**完成标准：** 稳定期资源增长符合预定门槛，无死锁或静默停止输出；正常结束和故障退出无本次 session/进程泄漏，所有质量指标可追溯。五秒 watchdog 是 worker teardown 机制，不等于端到端退出时间保证。

现有脚本可从管理员 pwsh 启动；以下命令只运行已有基础版本：

```powershell
pwsh -NoProfile -File .\scripts\r2_soak.ps1 -DurationMinutes 60 -IntervalMs 1000
```

### RUN-02：采样扰动与容量曲线

- [ ] 固定 workload、二进制/PDB、机器配置与运行方法，建立不采集的吞吐/延迟基线并重复运行。
- [ ] 比较 CPU 采样频率、PMC period、是否采栈/缓存栈、栈深、活跃核心数、事件类别和符号冷热缓存。
- [ ] 记录目标吞吐、P50/P95/P99 延迟、额外 CPU、内存、ETL/输出大小、loss 和关联覆盖率。
- [ ] 区分 capture、replay、symbolization 的开销；对不同线程数和运行时长绘图，报告重复次数及波动。
- [ ] 找到事件积压、管理预算或聚合上限触发的边界，验证有界失败/降级行为；既有预算不等于无限时长栈回放能力。
- [ ] 给出经测量支持的默认采样配置和适用范围，并保存脚本、原始数据与图表。

**完成标准：** 有可复现的频率/period × 栈成本 × 核心数曲线，能说明目标负载被扰动多少，以及何时结果质量下降。此前单个 fixture 的约 8.6 倍回放提速不能替代此项。

## 5. 低权限实时符号解析

### SEC-01：将 DbgHelp、PDB 与格式化搬出提权 worker

**当前基础：** live top 的 Symbolizer 仍在 elevated worker 中；虽已位于 ETW callback 之外，权限边界尚未调整。普通权限 controller 和 nonce/PID/ACL 认证管道已存在。

实施与验证：

- [ ] 定义职责：提权 worker 负责 ETW 配置、原始数据/必要元数据和恢复；普通权限 controller 或独立进程负责 DbgHelp、PDB 下载、符号缓存、聚合和格式化。
- [ ] 设计结构化、带版本的 IPC，传输 timestamp、CPU、PID/TID 生命周期、module load/unload、完整模块身份、IP/stack、period/source 和质量计数。
- [ ] 定义消息长度上限、批量/窗口语义、顺序、metadata reset、断连行为与兼容版本；优先保证同名 DLL、PID 重用和模块卸载后的历史样本正确。
- [ ] 给数据队列设置预算、背压与显式丢弃统计，保障 START/STOP/ERROR/FINISHED 控制消息不被大批数据堵塞。
- [ ] 保留 nonce、ACL、管道端 PID 和进程 handle 校验；worker 不接受任意文件读写/下载请求作为提权代理。
- [ ] 处理普通权限无法读取的模块/PDB：保留经验证可提供的身份元数据，输出 unknown/拒绝原因，并支持用户提供 archive；避免为解析符号扩大权限。
- [ ] 将 DbgHelp 初始化、全局串行约束、缓存和下载策略放在普通权限端；定义符号端卡住、崩溃、取消和重启行为。
- [ ] 运行现有 CPU 聚合、inline、identity/cache、archive 和 live/fault 回归；增加实际 token 权限、慢符号、IPC 截断/断连、旧版本、反压和 STOP 验证。
- [ ] 更新 `docs/ARCHITECTURE.md`、`docs/SECURITY.md` 和用户错误/降级说明。

**完成标准：** 实测确认 DbgHelp/PDB 下载处于普通权限进程；相同输入的统计和符号身份保持一致；符号故障不阻断 ETW 采集、STOP 和 session 清理；队列有界且质量损失可见。

**相关代码：** `src/ipc.*`、`src/commands/live_control.*`、`src/commands/live_worker.*`、`src/live_top.*`、`src/symbolizer.*`、`src/session_guard.*`。

## 6. off-CPU 分析

### ANA-01：等待区间关联调用栈

**当前基础：** 已有 observed running/ready/blocked 时间和 wakeup 分位数，但调度事件没有完整的等待栈关联与 off-CPU folded 输出。现有采栈配置面向 CPU/PMC sample，不能直接当作 CSwitch 等调度栈。

实施与验证：

- [ ] 先定义语义：blocked 为等待资源的区间，ready 为可运行但尚未获得 CPU 的区间；抢占、主动让出和采集边界单独处理，不将所有切出视为阻塞。
- [ ] 在平台支持时配置 CSwitch 等调度事件采栈，验证栈所属线程；ReadyThread 事件的唤醒方栈不能误标成被唤醒线程的阻塞栈。
- [ ] 扩展 event/interval 模型，关联事件与 StackWalkRef/Def，保留用户/内核栈、线程生命周期、exact/approximate/missing 等质量。
- [ ] 建立跨 CPU 的线程区间状态机，覆盖迁移、重复 ready、乱序、退出、TID 重用、丢事件和未闭合区间；状态及未匹配栈有预算和淘汰计数。
- [ ] 以已确认区间的等待时长为权重，按阻塞栈聚合；唤醒方信息单独表示，避免把同一区间重复累计。
- [ ] 增加表格/JSON 与 FlameGraph-compatible folded 输出，明确权重单位和覆盖率；SVG/HTML 渲染继续遵循项目当前的外部渲染方式。
- [ ] 用 Sleep、锁竞争、事件等待、受控 I/O 和 CPU 饱和负载验证 blocked/ready 区分、预期调用路径和等待时长。
- [ ] 先完成离线 record/report 闭环，再扩展 live；明确 `report --stream` 目前不保留栈，未完成专门设计前不承诺其支持 off-CPU。

**完成标准：** 等待时长、线程生命周期和调用栈对应正确；完整区间的聚合满足守恒核对，缺栈/边界/丢事件影响明确；可导出按等待时长加权的 folded 数据。多个线程等待时间之和可能超过 wall-clock，不能直接解释为总程序耗时。

**相关代码：** `src/etw.cpp`、`src/trace_model.h`、`src/etl_reader.cpp`、`src/trace_events.*`、`src/analysis.*`、`src/cpu_report.*`、`src/commands/offline.*`。

## 7. JIT 符号支持

### ANA-02：按运行时建立动态代码生命周期映射

**当前基础：** PE/PDB 符号路径主要服务静态模块，没有完整的 JIT 方法映射。不同运行时需要不同适配，不能用一个无生命周期的地址表覆盖全部 JIT。

实施与验证：

- [ ] 选择首期运行时和版本范围；建议先评估 .NET CLR 的 ETW 方法事件/rundown，确认所需字段、权限和采集结束流程后确定支持范围。
- [ ] 采集 method load/unload、rundown、module/runtime 身份等可用元数据，覆盖采集开始前已经生成的代码。
- [ ] 建立“进程生命周期 + runtime/method identity + 地址范围 + 生效时间”的查询模型，处理 tiered compilation、ReJIT、地址复用和缺失事件。
- [ ] 将 JIT 地址解析接入 CPU report、调用树、diff 和 top；区分 native/JIT 身份，不能只按函数名或地址合并。
- [ ] 仅在运行时提供并验证时支持源码行、IL/native 映射或 inline 信息；记录未解析覆盖率及原因。
- [ ] 为 archive 保存带版本和校验的 JIT 元数据，使脱离原机器后仍可离线解析；为大映射设置预算和质量计数。
- [ ] 编写采集前预热、采集中生成、重编译/卸载、同址复用、多进程和进程重启 workload，增加 synthetic 与真实运行时回归。
- [ ] 更新兼容性与使用说明；后续 V8/JVM 适配分别作为子任务评估，不把 .NET 完成标为全部 JIT 完成。

**完成标准：** 首期声明支持的运行时中，样本按采样时间解析到正确方法版本；同址复用和进程重启不串符号；archive 可离线重现。信息缺失时保留地址/unknown，不猜测方法名。

**相关代码：** `src/etw.*`、`src/etl_reader.*`、`src/trace_model.h`、`src/symbolizer.*`、`src/archive.*`、`src/commands/live_worker.*`、`src/commands/offline.*`。

## 8. 外部 profiler 共存与发布门槛

### CO-01：冲突检测和条件恢复的实测

**当前基础：** 已有 wperf mutex、counter ownership 检查、已知 interval 的条件恢复、早期 guard 和恢复 checkpoint。Windows 没有受支持的查询来读取原有完整全局 PMC source list，不能保证与任意外部 profiler 原子共存或完整恢复未知配置。

- [ ] 使用可控第二采集器，分别验证运行前占用、配置期间竞争、运行期间修改 interval/source 和并发停止。
- [ ] 注入配置中失败、worker/controller 异常退出、STOP 失败及恢复失败，核对 checkpoint、session 身份和诊断。
- [ ] 验证冲突时拒绝/报错明确；第三方修改后的配置不能被盲目覆盖，不主动终止不属于本次运行的 session。
- [ ] 记录能恢复的已知状态、无法查询的状态和竞态窗口，更新文档及错误消息。

**完成标准：** 可控场景下已实现策略有实测证据，失败可诊断；“任意外部 profiler 的全局 source list 原子恢复”继续列为平台限制，不作为可承诺的实现任务。

### REL-01：把验收证据接入持续验证和发布

- [ ] 通用 CI 保持现有两种构建和强制 elevated CPU/sched live integration；可移植回归与实机验收分别汇报。
- [ ] 建立带能力标签的专用硬件 runner；若只能手动运行，提供同一套可复现脚本与证据索引，防止无目标硬件时长期只报 SKIP。
- [ ] 对声称支持的 hardware 必须验收到有效数据；UNSUPPORTED/NOT-RUN 不计入通过率，失败保留原始诊断。
- [ ] 长 soak 放入计划运行或发布前运行，普通 CI 使用短压力回归；阈值和测试时长随报告登记。
- [ ] 每个发布版本列出本次验证的 commit、矩阵、未验证范围、性能曲线和已知限制，更新文档与打包清单。
- [ ] HW-01/HW-02/HW-03 的缺项验收通过后，逐项对照 Issue #5 原始要求，附证据再评估关闭；off-CPU/JIT 等独立路线不自动追加为 #5 的关闭前提。

## 9. 建议执行顺序与证据模板

1. 完善 RUN-01/RUN-02 的 workload、采集指标和自动判定；同时准备 HW-01/HW-02 的机器和校准工具。
2. 完成 LBR/PMC 实测，取得 HW-03 schema 证据后实现 decoder，推进 Issue #5 验收。
3. 扩展 VAL-01/VAL-02 和 CO-01，形成有明确范围的兼容性与恢复报告。
4. 推进 SEC-01 和离线 ANA-01；两者可独立开发，若扩展实时 off-CPU，再复用新的 IPC。
5. 为 ANA-02 选择首期运行时，完成离线映射与 archive，再接入实时流程。
6. 将可重复检查与周期性实测纳入 REL-01，并在新增能力后重跑相关 soak/扰动测试。

每项任务至少登记以下证据。建议原始产物放在 `artifacts/validation/<task-id>/<timestamp>/`；`artifacts` 是本地结果目录，不能作为其他人唯一可访问的证据来源。可共享报告放在版本化文档或 CI artifact 中，记录下载位置、hash 与保存期限。

| 字段 | 要求 |
|---|---|
| Task / 状态 | 对应编号、完成子项、剩余子项 |
| 代码 | commit、构建配置、工具链、运行时版本 |
| 环境 | OS build、CPU、核心/group、VBS/HVCI、权限、虚拟化状态 |
| 输入 | workload 源码/参数、binary/PDB 身份、参考工具与配置 |
| 操作 | 可复制的命令、采集时长、频率/period、过滤条件 |
| 门槛 | 运行前确定的预期值、容差、覆盖范围和失败规则 |
| 原始结果 | ETL、stdout/stderr、指标 CSV/JSON、hash 和访问位置 |
| 结论 | PASS/FAIL/UNSUPPORTED/NOT-RUN、统计波动、质量损失与限制 |
| 同步 | 本文、Issue 状态、兼容性/架构/使用文档的更新 |

原始 ETL/日志可能包含路径、命令行和用户信息；对外共享前按 `docs/SECURITY.md` 处理。尚无 evidence 的任务保持待完成，不以配置成功、合成回归或普通权限 SKIP 代替实机验收。
