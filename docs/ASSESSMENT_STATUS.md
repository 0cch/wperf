# 《wperf 项目评估》优化完成情况核对

> 本文是 `256eb66` 的历史核对，下面的“5 完成 / 9 部分完成”不是当前 main 的结论。
> `b050f07` 之后的实现、接口和验证边界见 [main 后续完善](MAIN_FOLLOWUPS.md)。
> 当前剩余任务的统一清单见 [后续待办与验收计划](TODO.md)。

核对日期：2026-11-13。原评估基线：`03ddd1f`。核对代码：`256eb66`，提交至 [PR #6](https://github.com/0cch/wperf/pull/6)。

**结论：优化点尚未全部完成。原评估的 14 项问题中，5 项的具体缺陷已修复并有本地回归依据，9 项只完成部分内容。测试体系和后续分析功能也仍有未完成项。**

这里的“已完成”指原问题的具体修复及已有回归，不表示 Windows/CPU/VBS 全部组合已经验证。“部分完成”表示已实现有效改进，但原评估提出的其他实现、边界或验证还没有落实。原报告位于本地 `artifacts/review/assessment.md`；本文保留全部问题编号，便于独立阅读和跟踪。

## 原 14 项问题

| 编号 | 原问题 | 状态 | 已落实 | 尚未完成 / 验证缺口 |
|---|---|---|---|---|
| 1 | PMC 采集与消费链路未闭环 | 部分完成 | 启用 `PERF_PMC_PROFILE`；解码 opcode 47、source、native period；`stat` 按目标 PID/source 筛选；删除错误的跨 CPU delta；record/stat 对缺少对应事件返回 unavailable | 没有实现 `TracePmcCounterListInfo` + `TracePmcEventListInfo` 的事件计数器快照配置；没有相应 CPU/source 差分统计；新的 PMC 采集尚未实机验证 |
| 2 | 实时 syscall 为无效 TID 编造耗时 | 已完成 | 实时/离线共用 `TraceCorrelationState`；拒绝 0/UINT32_MAX TID；校验已知线程生命周期；记录 syscall orphan；匿名 syscall 回归不再生成 duration | 实机吞吐和复杂丢事件场景属于整体验证待办 |
| 3 | 线程退出后丢失 PID、TID 重用错归属 | 已完成 | 样本到达时归属；延迟样本按时间查历史线程；新旧线程 start/end 不混用；回归覆盖退出、复用以及跨窗口 syscall/ready 关联 | 生命周期记录丢失、超过保留窗口的延迟数据仍只能报告未知；pending 通过身份检查和过期清理处理，未在每个 thread-end callback 立即移除 |
| 4 | diff 中的 Symbolizer 共用 DbgHelp 会话 | 已完成 | 独立 session key；DbgHelp 全局锁；diff 检查两个 Initialize 结果；回归验证第二会话关闭后第一会话仍可解析新地址 | 全仓库初始化错误处理仍有一处遗漏：`stat --pmu` 的 `Symbolizer::Initialize` 返回值未检查；不影响此次会话隔离修复的判断 |
| 5 | record 没等 READY 就启动目标 | 已完成 | worker 配置 session/guard 后发 READY；controller 等匹配 READY，再启动目标；处理失败、退出和超时；握手回归验证 PHASE 不能替代 READY | suspended-launch 选项及统一采集边界 metadata 尚未实现；短命目标的提权实机测试未做 |
| 6 | 全局配置恢复及并发策略不完整 | 部分完成 | wperf 全局 mutex；按选定 source 查询原 interval；查询配置后实际 interval；条件恢复；保留 STOP 失败的 session 身份；guard 携带恢复信息 | 不能恢复未知的完整原 source list，只清除自己设置的 PMC list；外部 profiler 不遵守 mutex；缺少共存/崩溃/恢复失败实测及配置阶段早期崩溃覆盖 |
| 7 | live LBR 未启用触发事件 | 部分完成 | live LBR 开启 profile；CPU/PMC 分别绑定 46/47；record 对零 LBR 数据返回 unavailable | live 路径没有零 LBR 数据失败检查，仍可能成功返回空结果；真实 trigger/payload 及平台矩阵未验证 |
| 8 | 实时丢失统计、内存上限不完整 | 部分完成 | ETW 运行期 loss/buffer 查询；ring、metadata、correlation eviction 计数；pending TTL/容量；历史 metadata 回收；符号地址 cache 容量、unused-module 清理；JSON quality 和 strict mode | 没有整体字节预算；live 停止时最终 ETW loss 尚未完整对外汇总；部分旧 payload decoder 仍未统一计入 malformed/decode errors；长期实测及可配置质量阈值未完成 |
| 9 | 内核地址空间和进程名查询不完整 | 已完成 | 高位内核地址可查 PID 0/4 模块；用户地址不跨 PID 借模块；镜像索引；进程显示名按采样时间查询；回归覆盖 kernel/user 分离和 PID 重用 | 当前是原生 x64/合成地址的验证，不代表完整 WOW64 coverage |
| 10 | diff 将采样间隙解释为函数耗时 | 部分完成 | 取消 next-sample gap 权重；解码 interval；已知 CPU sample 使用 period；PMC count 不转 CPU time | 缺少标准化/sample-share、采集时长、统计不确定性和运行区间分析；未知早期 sample 会借用 trace 的后期 interval，见下方复现 |
| 11 | annotate/archive 未校验正确版本 | 部分完成 | 共用 PE identity 检查；PDB GUID/age 校验；symbol-path 寻找 PDB；binary hash/PDB symbol-store 分目录；复制后再次验证；manifest 标记 mismatch；明确不复制 ETL | 缺少归档加载和原路径到归档路径的映射；原始路径失效后仍不能直接从 archive 解析；不是完整 portable archive |
| 12 | 表格和 Snapshot 存在多余开销 | 部分完成 | 普通表格只解 sampled IP；先过滤 PID/TID；按需构建 tree/folded；镜像/进程名索引；窗口 ring 交换；关联在锁外；该 fixture 报告约快 8.6 倍 | Snapshot 仍在 callback 共用 mutex 内复制/prune 全部 images/threads；未做到与 metadata 大小无关的短锁；部分路径保留线性 FindThread/FindImage；没有流式 replay/stack interning，也未将 live symbolization 移出提权 worker |
| 13 | 栈 provenance、聚合身份、机器输出 | 部分完成 | 保存 sampled IP/source/period/stack quality；近似匹配限 1ms 和相同线程实例；exact-only 输出；module-qualified frames；JSON/CSV 转义及 quality metadata | 聚合 key 仍以 PID/comm/module basename/symbol 为主，不是完整 process instance + module identity + RVA；不同路径/版本的同名 DLL 仍会合并，见下方复现；缺少专门的 Unicode/同名模块/版本测试 |
| 14 | 控制端消失和 STOP 失败处理不足 | 部分完成 | 识别 broken pipe；record/live 均有 guard；STOP 失败保留 session handle/name；CloseTrace 后回收 consumer；现有回归覆盖握手和断开的 pipe | `worker_.join()` 仍没有显式超时；未保存独立 controller process handle；controller/worker kill、STOP API failure、无匹配事件等完整故障矩阵没有验证 |

## 核对中额外确认的两处剩余行为

### 10：未知 sample period 仍会套用后期 interval

[`SampleWeightNs`](../src/trace_model.cpp#L142) 在 `sample.period == 0` 时使用 `trace.profile_interval_ns`。该 trace 字段会随 [`DecodeProfileInterval`](../src/etw_decode.cpp#L72) 更新。因此先采到 sample、后来才遇到 interval metadata 时，先前未知的 period 会被赋予后来的数值。

本地独立 probe 调用当前 core，先解码一个没有 interval 的 CPU sample，然后输入 source=0、interval=20000 的 CollectionStart：

```text
unknown_sample_period=0 later_trace_interval=2000000 weight_applied=2000000
```

这是实现行为的复现，不是一次新的实机采集。需决定未知 period 的明确策略，并加入“metadata 晚到/频率变化/同 trace 不同时段”的回归。当前文档中“unknown time weights stay zero”只对全 trace 都没有 interval 的情况成立，不能推广至每个未知 sample。

### 13：同名 DLL 的版本/路径身份仍会丢失

[`WriteCpuReport`](../src/cpu_report.cpp#L134) 的 row key 包含 PID、comm、module basename、symbol。tree/folded 在 [`module!symbol`](../src/cpu_report.cpp#L154) 层聚合，也没有完整模块身份。

独立 probe 使用两个不同路径、不同 timestamp 的 `plugin.dll`，分别位于不同基址、相同 RVA=0x100，且不初始化符号会话以模拟无法取得 PDB 的确定性 fallback。两个样本合并成一行：

```json
[{"overhead":100,"samples":2,"comm":"app.exe","dso":"plugin.dll","symbol":"plugin.dll+0x100"}]
```

需把计算身份与显示名称分开；保留 process instance、模块 path/identity、RVA，按 CLI 选择是否跨进程聚合。

## 原评估的测试缺口

| 项目 | 当前状态 |
|---|---|
| 只验证 help/exit-code，缺少行为回归 | 部分补齐：新增 raw decode、生命周期/关联、符号隔离、PE/PDB mismatch、READY/disconnect、JSON 回放 contract；仍缺工作负载校准 |
| StackWalkRef/Def 真实 ETL | 未完成：CPU golden 的 ref/def 仍为 0 |
| 含丢失事件的真实 ETL / parser fuzz | 未完成：现有 golden loss 均为 0；容量淘汰合成测试不等于 ETW loss 测试；截断文件测试不等于 fuzz |
| PMC query 恒真断言 | 已替换为 processor/owner type/session capacity 检查；真实硬件查询语义仍待验证 |
| live test 接受 access denied | 仍存在；普通 CI 不能因此证明完成了 live capture |
| 硬件失败也输出 PASS | M6 的 advertised PMC 失败已改为失败；CI 使用 live integration 和 `WPERF_REQUIRE_LIVE_TESTS=1`，但 LBR 的通用失败仍可能被记成 SKIP |
| soak 只监控 controller | 脚本已同时记录 worker/controller 的 memory/handles/CPU；本轮没有新的长时间 soak 结果 |
| Intel/AMD、OS、VBS/HVCI、WOW64/多 processor group 矩阵 | 未完成；当前只是本机普通权限和 synthetic/replay 验证 |

## 原推荐路线

| 路线 | 状态 | 剩余主要工作 |
|---|---|---|
| 1. 先建立可信结果 | 部分完成 | 完成 period/聚合边界、PMC 快照、live 零数据和有界停止；提权故障验证 |
| 2. 统一数据内核 | 部分完成 | 采样 decoder、correlator 已共享；其他原始 scheduler/syscall/pagefault/I/O decoder 仍重复；完整实例身份、采集边界、schema metadata 未完成 |
| 3. 提高分析效率 | 部分完成 | 修复 metadata 锁区；流式 replay、stack interning、字节预算、低权限 symbolization |
| 4. 完成常用 perf 工作流 | 大部分未开始 | 子进程范围、时间/CPU/user/kernel 过滤、sched runtime/blocked/ready 时长、off-CPU stacks、P50/P95/P99、I/O pathname、函数范围 annotate |
| 5. 建立实测发布门槛 | 部分完成 | 已有 replay/report benchmark 和 soak 采集脚本；缺已知 workload 对照、采样频率×栈深×核心数开销/丢失曲线及硬件 runner |

`sched` 当前的 Running/Ready/Blocked 仍是事件次数，不是累计时长。Linux perf 精确 event-group/multiplex scaling、syscall 参数语义、context-register payload 解码等能力边界也没有因为这次修复而改变。

## 已有验证及下一步

- 两种本地 Release 配置（有/无 Zydis）各 36/36 CTest 通过。
- GitHub push 与 PR CI 均成功；工作流已移除重复 smoke 脚本，改由 CTest 的 CLI、回放、archive、regression 和 live integration 测试覆盖。
- 原 benchmark 的三轮交替报告中位数：8.963s → 1.045s；仅代表该 ETL 的报告路径。
- 本次新建 probe 确认了上面的未知 period 和同名 DLL 合并行为。没有改动产品源码来隐藏这些剩余问题。

建议下一轮先完成 10、13、14 的结果/停止边界及回归，再补 1、6、7、8 的硬件与故障闭环；归档可移植性、metadata 锁区和分析功能按路线持续推进。
