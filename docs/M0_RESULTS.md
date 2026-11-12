# M0 结果记录

记录日期：2026-11-12（Asia/Shanghai）

## 当前环境

- Windows 11 Professional，10.0.26200，x64
- AMD Ryzen 7 8700G，8 cores / 16 logical processors
- 当前执行上下文：普通权限
- xperf：可用
- Visual Studio 2022 + Windows SDK 10.0.26100

## 能力探测结果

- ETW profile source 数量：18
- 可见 source 包括 Timer、TotalCycles、InstructionRetired、CacheMisses、BranchMispredictions 等
- Timer source：min interval 1221，max interval 1000000
- Max PMC counters：16
- `TraceMaxLoggersQuery` 在当前普通权限上下文返回 ERROR_NOT_SUPPORTED，`doctor` 已降级显示
- 活动 ETW sessions：可枚举
- xperf 帮助中存在 `-LastBranch`，说明工具链暴露 LBR 配置入口；实际硬件可用性仍待采集验证

## M0 产物

`scripts/m0_probe.ps1` 已生成：

- `wperf-list.txt`
- `wperf-doctor.txt`
- `environment.json`
- `summary.json`
- `xperf-help-start.txt`

## 未完成项

当前 shell 不是管理员，因此尚未生成真实 xperf CPU sample + stack ETL。管理员终端执行：

```powershell
pwsh -File .\scripts\m0_probe.ps1
```

应生成：

- `m0-cpu.etl`
- `xperf-profile.txt`
- `cpu-burn.txt`

该项作为发布 M0 基线前必须完成的管理员验收，不通过普通权限猜测结果。
