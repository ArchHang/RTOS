# docs/images — 文档插图

本目录存放 `docs/*.md` 中插图的 **PNG 渲染结果**。

## 这些文件是构建产物

**请勿手工编辑 PNG。** 每张图的**唯一真源**是文档内嵌的 ` ```mermaid ` 代码块：

```
<图注: images/rtos_design_03.png>       ← 供不支持 mermaid 的查看器显示

```mermaid
flowchart LR
    ...                                  ← 唯一真源: 改图改这里
```
```

> 图中占位符 `<图注: …>` 对应 Markdown 的 `![图注](…)` 写法。
> 该路径是相对**引用它的文档**（如 `docs/rtos_design.md`）解析的，不是相对本文件 ——
> 本文件位于 `docs/images/`，若要在此处真正插图，路径应为 `rtos_design_03.png`。

同时保留 PNG 与 mermaid 源码的原因是：本项目文档可能用 STM32CubeIDE、
Word 或纯文本编辑器查看，这些环境不渲染 mermaid；而 PNG 在任何 Markdown
查看器中都能显示。GitHub / VS Code(装插件) 则直接渲染源码块，便于阅读与修改。

## 如何重新生成

改完文档里的 mermaid 代码块后，在 `docs/` 目录执行：

```powershell
powershell -ExecutionPolicy Bypass -Command "& .\gen_diagrams.ps1 -Docs rtos_design,rtos_performance_report"
```

脚本会按**文档内出现顺序**把每个 mermaid 块渲染为
`<文档名>_<两位序号>.png`，并校验每个块都有对应的图片链接
（缺失或编号错位会打印 `WARN`，便于发现图文漂移）。

依赖：Node.js + `npx`（首次运行会联网拉取 `@mermaid-js/mermaid-cli`，之后走 npm 缓存）。

## 命名约定

| 文件 | 说明 |
|---|---|
| `rtos_design_NN.png` | 来自 `rtos_design.md`，NN 为块在文档中的序号 |
| `rtos_vs_freertos_perf_report_NN.png` | 历史文件名, 现由 `rtos_performance_report.md` 引用(合并报告沿用了原图, 不重命名以免重新渲染) |

序号与文档顺序绑定，因此**在文档中间插入新图会使后续图片整体重编号** ——
重新运行脚本即可自动对齐，无需手工改名。
