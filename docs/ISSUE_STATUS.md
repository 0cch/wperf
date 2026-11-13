# Issue #3 / #4 / #5 验收核对

核对基线：`b050f07`。实现提交：`8df156b`，CI 暴露的路径等价与停止反压修正：`10825e8`。

完整待办、实施步骤、依赖及证据要求见 [TODO.md](TODO.md)。本文保留 Issue 验收状态；建议的后续任务不等于已经完成。

## #3：现代栈、inline frame、symcache

| 验收项 | 实现 / 验证 |
|---|---|
| StackWalkRef/Def 精确关联 | 35/36 definition 与 37/38 ref，按 delete/rundown 时间区间处理 key 重用；原始 record 回归验证三个 epoch；提供 native `--stack-cache` 配置 |
| timestamp/checksum 端到端 | 记录身份、PE/PDB 验证、module/function 聚合 key 与独立 cache identity 都保留这些字段 |
| inline frame expansion | DbgHelp inline API；优化编译的 InlineInner/InlineOuter PDB fixture 确实展开，重新打开 cache 后仍可恢复 |
| 专用 .symcache | 有界 `identity/symbols.v1`，独立于 symsrv 下载缓存；包含 source/inline/RVA，拒绝不匹配 PE/PDB |
| PDB integration | 真实生成 PDB 的 matching/stale identity、会话隔离、inline/cache 重开验证 |

实现和验收已齐，[两种 CI 均通过](https://github.com/0cch/wperf/actions/runs/43410931662)，**#3 已关闭**。专门捕获的现代缓存栈 ETL、跨平台/WOW64 矩阵仍属于扩展验证，不能用原始 record 回归冒充实机采集。

## #4：实时 top/trace 与调度/I/O

| 验收项 | 实现 / 验证 |
|---|---|
| persistent real-time ETW + bounded ring | 持续 ProcessTrace、window budget、lifecycle delta、outside-lock metadata、loss/eviction 计数 |
| live trace | 共享 payload decoder 与增量关联器；CI 必须收到实际 sched 数字事件，不能只检查输出 header |
| CSwitch/ReadyThread attribution | 根据 payload new/old TID 与时间有效线程关联；不得用不相关 header context 冒充 scheduled-in PID |
| File/Disk I/O latency | init/RW/flush/complete 分开解码；正确 IRP 与 issuer、file key/name、completion pairing；synthetic 和真实 ETL 回归 |
| richer syscall where exposed | enter IP、exit status、已知身份的 duration；匿名线程不编造时间，不承诺 Windows 没公开的参数 |
| live/fault integration | CI 的 elevated runner 必须采到 CPU/sched；STOP 排空输出；worker kill 与零匹配控制端 kill 后 session 必须消失 |

强制真实采集的两种 CI 配置均为 **37/37 通过**，live integration 分别用时 10.85s / 10.38s，**#4 已关闭**。普通权限下的 SKIP 或 access denied 没有被用于满足这个条件。

## #5：PMC/LBR/context 与高级分析

| 验收项 | 状态 |
|---|---|
| LBR payload/config | decoder 与 CPU/PMC trigger、零数据检查已实现；尚无本轮 supported Intel 采集证据 |
| context-register CLI | 已配置；payload schema 未验证，仍不解码 |
| PMC period / exact-vs-estimated | interrupt sampling 与 CSwitch snapshots 分离；source/period/metadata 与 per-CPU/source 差分已实现；输出不宣称 exact/group/scaling；缺 target hardware calibration |
| Zydis instruction annotate | 已实现并验证；可对已知 function range 显示 instruction sample heat |
| weighted/calltree diff | 已实现 period、full identity、share/percentage-point/span/coverage 与采样估计界限 |
| archive PDB/symbol metadata | 校验 PDB GUID/age；identity/hash layout、symbol-path、modules.tsv、portable mapping 和 optional ETL |
| context/LBR integration | 仍缺 supported Intel + context schema 的实机组合验收 |

**#5 不能关闭。** 当前本机为 AMD Ryzen 7 8700G，且会话没有提权，不能据此证明 supported Intel LBR 或完整 context-register 验收。

## 原评估后续边界

本轮实现了原核对表中 period、full-identity、archive loading、streaming、interning、inline/cache、PMC snapshots、metadata lock、managed budgets、early recovery、zero-LBR、controller handle、teardown deadline 和分析 selectors/quantiles 等可直接改进的代码项。

仍未宣称完成：外部 profiler 的原 global source list 原子恢复（Windows API 边界）、全 OS/CPU/VBS/WOW64 矩阵、长 soak 与采样扰动曲线、context payload schema、off-CPU stack capture/JIT，以及把 live symbolization 搬出 elevated worker 的 IPC 架构改造。
