# R4 实施状态

> 更新：2026-11-13。目标是在不写驱动、不访问 MSR 的前提下，提高 PMC、LBR 和 context register 的语义可信度。

## PMC

### 已完成

- `doctor` 查询：
  - `TraceMaxPmcCounterQuery`
  - `TracePmcCounterOwners`
  - `TracePmcSessionInformation`
- PMC profile session 自动附带 CSwitch、Dispatcher、SystemCall 事件；这些事件可携带 PMC extended counters。
- 解析 `EVENT_HEADER_EXT_TYPE_PMC_COUNTERS`：
  - sample 的 `pmc_counters`
  - 普通 TraceEvent 的 `pmc_counters`
  - `pmc_counter_events` 质量计数
- `stat --pmu` 现在明确输出：
  - sample-derived 语义
  - samples_with_pmc
  - counters/sample
  - first/last raw counter
  - counter resets
  - sample-delta estimate
  - “不是精确 perf stat 总量”的警示
- 找不到 extended PMC counters 时返回 `unavailable`，不把 sample 数冒充 counter total。

### 平台限制

- ETW PMC 是 sample/event 附带数据，不是 `perf_event_open` 的精确组计数。
- PMC owners 查询在普通权限可能返回 `ERROR_PRIVILEGE_NOT_HELD`，doctor 会显示该状态。
- 当前 AMD 环境已有 PMU profile 基线，但 extended counter payload 需要管理员重新采集验证。

## LBR

- 新增公开数据布局解码器 `DecodeLbrPayload`：
  - header：QPC、PID、TID、filters
  - branch entry：from/to/reserved
  - 32/64-bit pointer size
  - 最多保留 128 个 branch
- `trace -e lbr` 输出 last-branch 事件：
  - from/to address
  - filter flags
  - PID/TID
- live `trace -e lbr` 会调用 `TraceConfigureLastBranchRecord`；不支持平台明确失败。
- 当前 AMD Ryzen 7 8700G 环境仍应返回 unsupported，这是合法结果。
- LBR 解码已有单元测试；真实硬件 payload 仍需 Intel 支持平台验证。

## Context register

- `record --context-registers` 已接入：
  - `TraceContextRegisterInfo`
  - `EtwContextRegisterTypeControl | EtwContextRegisterTypeInteger`
  - classic PerfInfo sample event
- API 不支持时不阻塞普通采集，worker 通过 `STATUS context-registers=...` 明确报告降级原因。
- 由于 Windows 公开 SDK 没有稳定的 context-register payload 布局契约，当前不猜测 IP/SP/control/integer 的字节偏移，也不伪造寄存器值。
- 只有后续在支持平台上取得可验证 schema 后，才启用寄存器值解码。

## 验证

普通权限回归：

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
wperf doctor
wperf trace -i tests/data/io.etl -e io --max 5
```

硬件/提权验证：

```powershell
wperf record --elevate --profile pmu --pmc-source 19 --duration 3000 -o pmu-r4.etl
wperf stat --pmu -e TotalCycles --duration 3000 -- <workload>
wperf record --elevate --lbr --duration 1000 -o lbr.etl
wperf record --elevate --context-registers --duration 1000 -o context.etl
wperf trace -i lbr.etl -e lbr --max 20
```