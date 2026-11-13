# R3 实施状态

> 更新：2026-11-13。目标是提高调度、syscall、pagefault 和 I/O 的 ETW fidelity。

## 已完成

### 调度事件

- CSwitch 解析：
  - new/old TID
  - old wait reason
  - old thread state
  - wait reason/state 文本
  - new thread wakeup latency
- ReadyThread 解析 awakened TID、adjust flags，并关联目标 PID。
- PID/TID 通过线程生命周期 resolver 在事件时刻解析，避免 TID 复用误归属。
- `sched` 输出：
  - Running
  - Ready
  - Blocked
  - Wakeups
  - WakeupAvgUs / WakeupMaxUs
  - TopWaitReason
- 无法归属的系统线程保留 raw TID，显示 unknown，不伪造 PID。

### Syscall

- SysCallEnter 保留 syscall address。
- SysCallExit 解析 NTSTATUS。
- 同 TID 的 enter/exit 计算 duration。
- Windows kernel provider 不提供 PID/TID 时，不建立跨 TID 猜测关联，duration 保持未知。

### Pagefault

- 保留 virtual address 和 program counter。
- 不做 ETW 未提供的 fault resolution latency 推断。

### I/O

- FileIo：
  - 解析 IRP correlation id、offset、size、completion 和 NTSTATUS。
  - 通过 IRP 将 read/write/create/query/dir-enum/cleanup/close/flush 等 init 与 OperationEnd 关联。
  - 计算 completion duration；未匹配 init 时保留 completion，不伪造时长。
- DiskIo：
  - 解析 IRP、offset、transfer size、completion 状态。
  - 使用 ETW 提供的 HighResResponseTime 作为 duration。
- pending IRP map 设置 200000 上限，避免异常/缺失 completion 导致内存无界增长。

### Live/offline 一致性

- TraceEvent 统一保存 related TID/PID、status、duration、size、offset、
  correlation id、wait reason/state/flags。
- offline `trace` 与 live `trace` 输出相同的核心字段。
- JSON 同步输出新增字段。
- 新增 R3 golden fixtures：
  - `tests/data/sched.etl`
  - `tests/data/syscall.etl`
  - `tests/data/io.etl`
- 单元测试断言事件总数、调度关联数量、syscall enter/exit 数量和 I/O init/complete 数量。

## 边界

- 不实现 Linux syscall 参数级语义。
- 不用 CPU 或时间邻近性猜测缺失的 syscall TID。
- I/O 文件名仍需 ETW FileIo name/rundown 完整数据；当前重点是 IRP、大小、状态和时长。
- CSwitch 的 wait reason 是 Windows ETW 语义，不与 Linux `perf sched` 字段强行对齐。

## 验证

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
wperf trace -i tests/data/sched.etl -e sched --max 20
wperf sched -i tests/data/sched.etl
wperf trace -i tests/data/syscall.etl -e syscall --max 20
wperf trace -i tests/data/io.etl -e io --max 20
```
## Golden 指标

- `sched.etl`：CSwitch 127472，ReadyThread 71872，wakeup latency 71872，unknown PID 10623。
- `syscall.etl`：SysCallEnter 209018，SysCallExit 205152；该 trace 无可用 PID/TID，因此 duration 为 0。
- `io.etl`：FileIo 250028，DiskIo 1417；init 13964，complete 15374，orphan 1424。
- `report --kind io` 输出 init/complete/orphan、File/Disk 事件数、平均/最大延迟；`--json` 输出机器可读汇总。
