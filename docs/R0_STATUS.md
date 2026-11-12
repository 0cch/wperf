# R0 实施状态

> 更新：2026-11-12。范围仅限无驱动、无 MSR、无内核修改的系统能力。

## 已完成

### P0.1 golden replay

- 提交 `tests/data/cpu-stack.etl`，4 秒 x64 native CPU trace，使用 `xperf -merge -mergeonly -compress` 压缩。
- 文件可普通权限 replay，不依赖管理员权限。
- `tests/data/cpu-stack.expected.json` 固定事件数、sample 数、stack 覆盖质量与 lost 计数。
- 单元测试直接读取 fixture，避免只测试手写内存模型。

### P0.2 StackWalk 精确关联

- ETW replay 启用 `PROCESS_TRACE_MODE_RAW_TIMESTAMP`。
- sample 与 StackWalk 使用同一 QPC 时钟，并按 `QPC + TID + CPU` 关联。
- 同一 key 有多份栈时使用 sample leaf IP 消歧；仍不唯一时计入 `stack_ambiguous`，不静默猜测。
- 支持 direct stack，以及现代 `StackWalkDef/Create|Delete|Rundown`、`StackWalkRef/Kernel|User` payload 解析。
- user/kernel stack fragment 按 PerfView 语义拼接；未解析 key 计入 `stack_key_mismatch`。
- 质量输出包含 `stack_exact`、`stack_approx`、`stack_ambiguous`、`samples_without_stack`。

当前 CPU fixture 结果：

```text
samples=17323 stack_events=17971
stack_exact=16104 stack_approx=44
stack_ambiguous=496 stack_key_mismatch=0
samples_without_stack=1175
```

### P0.3 模块与 PDB 身份

- 模块 key 包含 PID、base、size、PE timestamp、checksum 和规范化路径。
- 解析 PE32/PE32+ 的 timestamp、checksum、ImageSize、machine 和 RSDS PDB GUID/age。
- DbgHelp 启用 `SYMOPT_EXACT_SYMBOLS`。
- 校验 DbgHelp `PdbUnmatched`/`DbgUnmatched`、PDB GUID/age；不匹配时回退为 `module+offset`。
- 每个模块使用独立 synthetic base，避免地址被卸载后重载时复用上一模块符号。
- image load/unload/DCStart/DCStop 更新生命周期，`FindImage` 不会命中已卸载模块。

### P0.4 decoder hardening

- 非 ETL 文件返回失败和明确错误。
- 0、64、4096 字节截断 ETL 不崩溃并被拒绝。
- 未知 provider/opcode 计入 `unknown_events`，不中止 replay。
- 所有 payload 读取使用带长度检查的 helper；32/64-bit pointer 分流保留。

### P0.5 命令模块化

- 公共 CLI helper 移入 `src/commands/util.h`。
- M7 命令移入 `src/commands/advanced.cpp`。
- `script/trace/sched/report` 移入 `src/commands/offline.cpp`。
- `commands.cpp` 从约 2185 行降至约 1300 行；采集、worker/guard 和 live stat/top 仍在主文件。

## 未完成但不阻塞 R1

- 压缩 stack 的专用 golden fixture：当前实现已覆盖代码路径，但现有 fixture 是 direct stack 格式。
- 32-bit/WOW64 replay fixture 和完整 symbolization 验收。
- decoder fuzz corpus、恶意长度和长时间随机 replay。
- `stat/top` 与 record/worker 的进一步拆分。
- 符号加载失败率、PDB missing/mismatch 的聚合统计与 JSON 输出。

## 验证

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\wperf.exe report -i tests\data\cpu-stack.etl --folded
```

## 进入下一阶段的判定

R0 的正确性基线已经足以约束 R1：golden replay 稳定、栈关联不再依赖 FIFO、过期模块身份被拒绝、截断输入不崩溃。剩余 R0 项属于扩展覆盖，可与 R1 的 inline/symcache/权重工作并行补强。