# wperf 项目复查与优化报告方案

> 状态：评估方案 / 初步结论，尚未执行代码优化。  
> 原则：先验证、再评分、后优化；未经确认不修改实现代码和 Issue 状态。

## 1. 复查目标

本次复查回答五个问题：

1. 当前 wperf 对 Linux perf 的复刻程度有多高？哪些能力是真实支持，哪些只是近似或占位？
2. Issue #3、#4、#5 中哪些应该优先完善，哪些应该继续保持实验性？
3. 代码、测试、构建、发布和运行体验上，哪里是当前最值得优化的瓶颈？
4. 是否现在应该引入自有 `.wperf` 格式？收益是否大于成本？
5. 下一阶段如何排优先级，避免继续“功能增加但保真度不清晰”？

## 2. 复查范围

### 代码

- `src/commands.cpp`
- `src/etl_reader.*`
- `src/trace_model.*`
- `src/etw.*`
- `src/cpu_report.*`
- `src/symbolizer.*`
- 构建、脚本、CI 和发布流程

### 行为

- `record/report/top/stat/trace/sched/script`
- `annotate/diff/archive/buildid-list`
- PMU/LBR/context register 相关能力
- 普通权限、UAC、worker、guard 和异常清理

### 证据

- 本地 Release/Debug 构建和 CTest 结果
- 现有真实 ETL 样本（CPU、sched、vm、syscall、io、pmu）
- GitHub Actions 结果
- open/closed Issue 内容
- README、PLAN、ROADMAP 和阶段状态文档

## 3. 初步复刻程度结论

复刻度不能只用“命令是否存在”衡量。建议按三个口径分别评分：

| 口径 | 初步评价 | 说明 |
|---|---|---|
| 命令覆盖 | 约 70% | `record/report/top/trace/stat/sched/script/annotate/diff/archive/buildid-list` 已有入口 |
| 日常 CPU profiling 工作流 | 约 70%～80% | sample、stack、native PDB、flat/folded/calltree 已可用 |
| 完整 perf 能力面 | 约 40%～50% | live top/trace、精确 PMU、inline frame、JIT、uprobe/kprobe、mem/c2c 等缺失或近似 |
| Windows 原生系统能力利用 | 约 75% | ETW、DbgHelp、Win32 时间/cycle API 已接入 |

### 分项评分建议

评分采用 0～5 分，并同时标注“直接支持 / 近似 / 未支持”。

| 能力 | 当前分数 | 初步判断 |
|---|---:|---|
| CPU sampled profile | 4/5 | Timer sample 已真实工作 |
| native call stack | 3/5 | 可用，但 stack 关联仍有 heuristic fallback |
| PDB symbolization | 3.5/5 | 函数/行号可用，inline/symcache 待完善 |
| folded/calltree | 3.5/5 | 已可用，权重和栈准确性待提高 |
| `top` | 2/5 | snapshot capture，不是连续 real-time consumer |
| `stat` time/cycle/I/O | 3/5 | Win32 统计可用 |
| ETW PMC | 2.5/5 | 有 PMU profile source，但不是精确 perf_event 语义 |
| trace/sched | 3/5 | 基础 CSwitch/ReadyThread 可用，PID 归属部分缺失 |
| I/O tracing | 2.5/5 | 事件级基础输出，尚未完成 init/complete 延迟关联 |
| annotate | 2/5 | 当前是 address heat，不是指令级反汇编 |
| diff | 2.5/5 | 仅 leaf symbol 计数差异 |
| archive | 2.5/5 | 模块/PE/PDB 基础归档，缺少完整符号服务 |
| LBR/context register | 1/5 | LBR 配置入口存在，当前平台未验证；context 未接 CLI |

结论：**核心日常 profiling 已具备可用雏形，完整 perf 复刻仍处于中段。**

## 4. Issue 现状与完善建议

当前开放 Issue：

- #3：exact stack refs、inline frames、symcache
- #4：real-time top、live trace、exact sched/IO attribution
- #5：exact PMC semantics、LBR payload、context registers、disassembly、richer diff/archive

### 建议优先级

| 优先级 | Issue/工作 | 原因 |
|---|---|---|
| P0 | 真实 ETL golden fixture + 回归测试 | 当前很多测试只验证 help/合成数据，不能防止真实解码回归 |
| P0 | StackWalkRef/StackDef 精确关联 | 直接影响 report、folded、calltree 的可信度 |
| P1 | Issue #3：inline frame、PDB identity、symcache | 直接提高 native profiling 质量 |
| P1 | Issue #4：live top/live trace、I/O 延迟 | 决定它是否真正像 perf 的交互工具 |
| P1 | Issue #4：CSwitch/ReadyThread PID 归属 | 当前 sched 统计有明显 unknown 项 |
| P2 | Issue #5：PMC/LBR/context register | 硬件相关、平台差异大，应先保证实验边界 |
| P2 | Issue #5：annotate 反汇编、weighted diff | 高级分析，不影响基础 record/report |
| P2 | archive PDB/symbol-server 元数据 | 发布/诊断价值高，但不是核心采样路径 |

### 建议新建但暂不执行的 Issue

- `P0: Add real ETL replay fixture and golden trace tests`
- `P1: Refactor monolithic commands.cpp into feature modules`
- `P1: Add stable JSON schema versioning`
- `P2: Add archive size limits and PDB identity checks`
- `P2: Add trace quality regression dashboard`

这些 Issue 必须在报告确认后再创建或实施，避免把评估阶段变成代码变更阶段。

## 5. 可优化项目清单

### 5.1 架构和可维护性

1. **拆分 `commands.cpp`**
   - 现状：800+ 行，命令解析、采集、报告、stat、top、trace、archive 混在一起。
   - 影响：新增功能时回归风险高，单元测试难。
   - 目标：按 `commands/record`, `commands/report`, `commands/stat`, `commands/top`, `commands/trace`, `commands/m7` 拆分。

2. **统一 Event schema/version adapter**
   - 将 classic ETW provider、event opcode、payload 版本集中管理。
   - 避免每个 decoder 分支散落在 callback 中。

3. **统一 trace quality 模型**
   - lost events、unknown events、stack missing、PID unknown、symbol failure。
   - 当前已有部分质量字段，但报告展示和测试还不完整。

### 5.2 正确性

1. 解析 StackWalkRef / StackWalkDef，替换 leaf-IP + FIFO fallback。
2. 将模块 timestamp、size、checksum、PDB GUID 纳入符号缓存 key。
3. 对 CSwitch/ReadyThread 使用 Thread rundown/start time/tid 生命周期解析 PID。
4. File/Disk IO init/complete 关联，计算延迟和 orphan 计数。
5. Syscall 只输出 ETW 实际提供的地址/字段，不假装有完整参数。
6. 对 32-bit/WOW64 trace 和 ARM64 做布局回归。

### 5.3 测试和 CI

1. 保存一组小型 golden ETL：CPU、sched、vm、syscall、io、PMU。
2. 在非管理员 CI 中进行 ETL replay，不依赖 UAC。
3. 增加真实 capture 测试作为自托管管理员 runner。
4. 增加 DbgHelp/PDB fixture。
5. 增加 parser fuzz/截断 ETL 测试。
6. 对 M0-M7 smoke 做统一 failure reporting。

### 5.4 交互和用户价值

1. `top` 改为持续 real-time consumer + bounded ring。
2. `trace` 支持 live stdout，而不仅是 `-i`。
3. `top/trace` 支持更清晰的 Ctrl-C 和终端无 VT 降级。
4. `stat --pmu` 明确显示 estimated/sample-derived/total。
5. 所有 unsupported/approx 能力在 `--help` 和报告表头中标注。

### 5.5 发布与安全

1. archive 默认限制体积，避免复制整个系统的模块。
2. 符号源、PDB 路径和 source server 默认安全策略。
3. 诊断 bundle redaction 自动化测试。
4. release 包加入 PDB 选项、版本、SHA256 和兼容性矩阵。
5. 对 `mincore`/新 API 动态加载行为做跨 Windows 版本测试。

## 6. 自有格式收益评估

### 6.1 当前 ETL 的优缺点

优点：

- 系统原生格式，xperf/WPA/PerfView 可互操作。
- 不需要自建写入器和格式版本管理。
- 采集路径简单，风险低。
- 调试时可以交给现成工具。

缺点：

- 重复 report 需要重复解析和解码。
- 符号化和栈关联成本重复发生。
- 缺少适合快速随机访问的事件/栈/字符串索引。
- trace 体积较大。
- 不容易把采集质量、模块身份、符号来源和 filtered trace 元数据一起保存。
- 不同 Windows build 的 classic schema 兼容仍需自己做。

### 6.2 引入 `.wperf` 的潜在收益

- 字符串、栈、模块和符号 intern。
- 按 timestamp/PID/TID/event type/CPU 建索引。
- report、diff、annotate 更快。
- 可保存归一化事件模型和 trace quality。
- 可支持压缩、跨机器报告和渐进式符号缓存。
- 后续可以扩展自有检测结果、标记和 diff 基线。

### 6.3 引入 `.wperf` 的成本

- 需要定义稳定 schema、版本迁移和兼容规则。
- 需要 ETL -> `.wperf` 的 inject/convert 全流程。
- 需要同时维护 ETL 和自有格式两条读取路径。
- 增加测试矩阵、归档、损坏恢复和安全校验成本。
- 不会自动解决 ETW 解码、栈关联或 live consumer 的正确性问题。

### 6.4 初步建议

**当前阶段不建议把 `.wperf` 变成主格式。**

建议顺序：

1. 保留 ETL 作为原始采集格式。
2. 先修复真实 ETL 回归、StackWalkRef、live top/trace 和符号正确性。
3. 在 v1.0 后将 `.wperf` 设计为 **可选派生缓存/归一化格式**，而不是替换 ETL。
4. 通过 `wperf import/export` 保持互操作。
5. 只有出现明确的性能或功能痛点时，再决定是否默认写 `.wperf`。

建议的收益判断指标：

- 1 GB ETL 的重复 report 时间是否明显不可接受。
- 符号化是否在每次 report 中重复占用大量时间。
- annotate/diff/live trace 是否需要随机访问索引。
- 用户是否需要把 trace 和符号元数据一起交付。
- ETL 跨机器报告是否频繁失败或过慢。

## 7. 最终报告交付结构

确认后，我将输出正式报告，建议包含：

1. 项目现状和证据清单。
2. 命令/能力/保真度矩阵。
3. 复刻度评分和评分方法。
4. Issue #3/#4/#5 的逐项评估和新 Issue 建议。
5. 代码架构、正确性、测试、性能、安全、发布六个维度的优化建议。
6. 优化优先级和依赖关系。
7. `.wperf` 格式的收益/成本/决策建议。
8. 推荐下一步路线和明确不做的事项。

## 8. 决策门

在你确认报告方案前，不执行：

- 不修改实现代码。
- 不创建新 Issue。
- 不改变 ETL/`.wperf` 格式。
- 不调整 CI 或发布流程。

确认后再进入下一阶段：先修 P0/P1 正确性，再评估 real-time、自有格式和高级分析。
