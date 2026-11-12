# R1 实施状态

> 更新：2026-11-12。R1 目标是 native profiling fidelity，不改变“不写驱动”的边界。

## 已完成

### In-memory symbol cache

- 缓存键使用模块 synthetic base + RVA，避免同一路径不同模块实例互相污染。
- 正结果和失败后的 `module+offset` 都会缓存。
- `SymbolizerStats` 暴露 `cache_hits/cache_misses`，单元测试验证重复地址命中缓存。
- 缓存生命周期绑定 `Symbolizer` 实例；`Close()` 会清空。

### 线程和进程生命周期

- Process/Thread 的 Start、Stop、DCStart、DCStop 分别更新开始/结束时间。
- sample 的 PID 归属按 `TID + sample timestamp` 查询，不再使用“最后一次出现”的 TID→PID 映射。
- 同一 TID/PID 在 trace 中复用时会匹配各自生命周期。
- 单元测试覆盖 TID/PID 复用空档和第二阶段。

### 行号

- DbgHelp 已启用 `SYMOPT_LOAD_LINES`。
- `ResolvedSymbol` 已携带 source file 和 line；现有 JSON/CSV 尚未把二者作为独立字段输出。

## 尚未完成

- inline frame：DbgHelp inline context API 已存在，但需要专门的 inline fixture 与 frame 展开规则后再接入，避免输出错误父子关系。
- 磁盘 symcache：当前依赖 DbgHelp/`_NT_SYMBOL_PATH` 的标准符号缓存；独立 `.symcache` 暂不引入。
- 报告权重：当前 timer sample 仍按样本数统计；period 权重和 calltree diff 留待后续。
- source file/line 的结构化 JSON 字段。

## 验证

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```