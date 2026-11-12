# M0 结果记录

记录日期：2026-11-12（Asia/Shanghai）

## 当前环境

- Windows 11 Professional，10.0.26200，x64
- AMD Ryzen 7 8700G，8 cores / 16 logical processors
- 普通权限和管理员权限均已验证
- xperf：可用
- Visual Studio 2022 + Windows SDK 10.0.26100

## 能力探测结果

- ETW profile source 数量：18
- 可见 source 包括 Timer、TotalCycles、InstructionRetired、CacheMisses、BranchMispredictions 等
- Timer source：min interval 1221，max interval 1000000
- Max PMC counters：16
- 活动 ETW sessions：可枚举
- xperf 帮助中存在 `-LastBranch`，说明工具链暴露 LBR 配置入口；实际硬件可用性仍待采集验证

## 管理员验证

`xperf -on PROC_THREAD+LOADER+PROFILE -stackwalk Profile` 在当前 WPT/Windows build 上能启动，但其自身生成的 ETL 经 `xperf -a profile` 检查没有 sampled profile 数据。该问题不影响 wperf 的 ETW 采集路径。

wperf 自身的 CPU profile 已验证：

```powershell
wperf record -F 999 -o artifacts\m2\cpu.etl -- wperf_cpu_burn.exe 3000 4
```

该 ETL 同时被 `xperf -a profile` 和 `wperf report` 成功解析，证明：

- SystemTraceProvider sampled profile 正常。
- StackWalk 事件正常产生。
- xperf 可以解析 wperf 生成的 ETL。
- wperf 可以独立解码并生成 CPU report。

## 结论

- Windows 使用 ETW SystemTraceProvider 是可行路线。
- CPU sampling 和 stackwalk 已进入 M2/M3 实现。
- PMC source 数量可以通过 `TraceQueryInformation(TraceMaxPmcCounterQuery)` 获取。
- xperf 继续作为外部对照工具，不作为最终运行时依赖。
