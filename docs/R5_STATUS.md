# R5 实施状态（wperf 0.5.0-r5）

> 更新：2026-11-13。目标：高级分析、manifest 可复现性和 provider 扩展；反汇编能力等待库选型。

## Diff

- 新增加权比较：按每个 CPU 的相邻 sample 时间差计算权重，避免只用样本数。
- 新增 `--pid`、`--dso` 过滤。
- 新增 `--calltree`，按完整 leaf-to-root 栈进行差异比较。
- 支持文本、CSV、JSON。
- JSON/CSV 同时输出 sample delta 和 weight delta。

## Annotate

- 已保留 module、symbol、displacement。
- 新增 source file、line。
- 支持文本、CSV、JSON。
- 反汇编暂不引入依赖，等待库选型；当前不使用错误的“伪反汇编”。

## Archive

- 仍会收集实际 sample/stack 命中的模块。
- `--max-size-mb` 限制单文件归档体积。
- `--symbol-path` 写入 manifest，便于复现实验。
- 同时生成：
  - `manifest.json`
  - `manifest.csv`
  - `manifest.txt`
- manifest 包含：
  - source/archived path
  - copy 状态与原因
  - 文件大小
  - SHA256
  - PE timestamp/checksum/ImageSize/machine
  - PDB GUID/age
  - PDB 路径、复制状态与 SHA256
- 修复 archive SHA256 读缓冲导致的栈溢出，并加入 `archive_manifest_smoke` 回归测试。

## Providers

- `wperf list providers [--max N]` 使用 `TdhEnumerateProviders`。
- `record --provider GUID` 使用 private ETW session + `EnableTraceEx2`。
- 自定义 provider 产生 ETL，保留与 WPA/PerfView 的互操作；TDH 字段级解码后续再扩展。
- 无效 provider GUID 有参数回归测试。

## Release

- `build_release.ps1` 现在包含：
  - EXE + PDB
  - README/LICENSE（存在时）
  - 关键 docs
  - `manifest.json`（文件名、大小、SHA256）
  - `COMPATIBILITY.md`
  - `RELEASE.txt`
- 保留单 EXE 运行，不依赖 WPA/WPR 运行时。

## 反汇编待决策

反汇编将作为可选构建特性，候选为 Zydis、Capstone、LLVM MC 或 DbgEng。当前推荐 Zydis，详见 `docs/DISASSEMBLY_DECISION.md`。

## 验证

- CTest 21 项通过。
- archive manifest smoke 通过。
- provider list / invalid GUID 测试通过。
- 反汇编尚未加入，避免在选型前破坏单 EXE 或引入不必要依赖。