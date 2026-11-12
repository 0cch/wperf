# R2 实施状态

> 更新：2026-11-12。目标是持续 ETW consumer、bounded ring、live top/trace，以及可控制的提权 worker。

## 已完成

### 提权控制面

- 新增 `--internal-live-worker`。
- controller 创建命名管道并发起一次 `runas` worker；UAC 只出现一次。
- worker 通过 `HELLO nonce pid` 握手，controller 发送 `START/STOP`。
- worker 输出 `TOP_BEGIN/TOP_LINE/TOP_END`、`OUT`、`WARN`、`ERROR`、`FINISHED` 消息。
- controller 断开或 Ctrl-C 时发送 `STOP`；worker 未及时退出时 controller 停止同名 ETW session 并终止 worker。
- 非提权环境下的 worker handshake/失败路径已加入单元测试。

### live top

- `EtwSession` 支持 `EVENT_TRACE_REAL_TIME_MODE`，不需要 ETL 输出路径。
- `LiveTopCapture` 用专用线程执行 `ProcessTrace`。
- producer callback 只解析固定字段并写入 bounded ring；DbgHelp、报告排序和 UI 不在 callback 内。
- sample ring 上限 200000，event ring 上限 200000；溢出计数为 `ring_dropped`。
- 默认 1 秒窗口；支持 `-p PID`、`-e cpu|<profile-source>`、`--sort`、`--percent-limit`、`--iterations`。
- 符号缓存跨 interval 复用；未命中时显示 `module+offset`。
- VT 可用时清屏刷新；不可用时降级为滚动输出。

### live trace

- `wperf trace --live -e sched|syscall|pagefault|io -p PID`。
- 支持 `--duration MS`、`--max N`、文本或 JSON。
- 离线 `trace -i FILE` 保持兼容。
- 实时事件通过同一 bounded ring 到 consumer，再由 controller 输出。
- `trace -e sched` 作为 live scheduler 观察入口；离线 `sched -i FILE` 继续保留。

### 稳定性工具

- `scripts/r2_soak.ps1`：管理员环境下运行 1 小时 top soak，记录 working set/handle count、退出码和残留 `wperf-top-*` session。
- controller/worker 断链清理路径已实现。

## 验证状态

普通权限下已通过：

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
wperf top --help
wperf trace --help
```

提权相关验收需要交互确认 UAC，尚未在本会话自动执行：

```powershell
wperf top --elevate --iterations 1
wperf trace --live --elevate --duration 2000 -e sched
pwsh -File .\scripts\r2_soak.ps1 -DurationMinutes 60
```

## 边界

- live top 只使用 leaf IP，不采集 stack；这是 top 的轻量路径。
- live trace 只输出 ETW 暴露的字段，不补造 Linux syscall 参数语义。
- 1 小时 soak、内存曲线和残留 session 需要管理员终端执行后才能签署最终验收。