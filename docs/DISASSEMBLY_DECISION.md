# 反汇编库选型

## 约束

- 单 EXE 优先。
- 可选、静态链接、不要求管理员。
- 只反汇编 ETL 中命中的模块 RVA，不执行目标代码。
- 不引入大型调试器运行时作为默认依赖。
- 构建失败不能影响不带反汇编功能的 wperf。

## 候选

| 方案 | 许可 | 优点 | 代价 | 适配度 |
|---|---|---|---|---|
| Zydis | MIT | x86/x64 专注、解码+格式化完整、易静态集成、Windows友好 | 仅 x86/x64 | 最高 |
| Capstone | BSD-3-Clause | 多架构、API成熟、社区广 | 二进制更大，x86细节与格式控制不如Zydis直接 | 高 |
| LLVM MC | Apache-2.0 with LLVM exceptions | 指令支持最全、格式强 | 构建和体积巨大，不适合轻量单 EXE | 低 |
| DbgEng | Windows SDK/系统组件 | 与调试器一致 | 依赖调试引擎，接口/部署更重 | 低 |
| udis86 | BSD | 小巧 | 维护老，新指令覆盖不足 | 不建议 |

## 推荐

**默认选择 Zydis。**

理由：

1. wperf 的主要目标架构是 x64，未来可自然支持 x86/WOW64。
2. MIT 许可适合单 EXE。
3. 解码与 formatter 分离，便于输出 instruction、operands、length。
4. CMake 静态链接集成简单，可做成 `WPERF_ENABLE_DISASM` 可选特性。

备选：需要 ARM64 或多架构同时反汇编时选择 Capstone。

## 建议接入方式

- CMake 选项 `WPERF_ENABLE_DISASM=OFF`。
- 固定版本并记录来源/SHA256。
- 读取模块 PE 的 RVA-to-file-offset，读取命中位置前后 16–64 bytes。
- `annotate --disasm` 输出：
  - samples
  - module!symbol+offset
  - source:line（可用时）
  - instruction bytes
  - 反汇编文本
- 反汇编失败时回退 address heat，不影响其他功能。
- 增加固定机器码单元测试，禁止联网下载依赖作为默认构建步骤。

## 待确认

请选择：

1. Zydis（推荐）
2. Capstone
3. LLVM MC
4. 暂不做反汇编