# 贡献与提交约定

## 提交信息

采用 [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)：

```text
fix(rtos): preserve task identity during static initialization

Explain the triggering condition, the resulting behavior and the reason
for the change. Include the validation performed and its limitations.
```

按目的使用 `fix`、`perf`、`test`、`docs`、`build` 或 `chore`。标题写具体行为，正文说明问题、方案及验证；实际破坏公共接口时明确 `BREAKING CHANGE`。一次提交应包含一个可审阅范围及必要配套代码。修改作者身份和签署信息须准确，不自动添加未实际完成的人工审核或认证声明。

## 源码与验证

生产内核在 `RTOS/`，权威 HIL 夹具在 `hil/fw/`，固件变体通过 `hil/build_fw.ps1` 选择。归档测试源不参与构建，构建产物和本地回滚副本不入库。

C 修改采用小写下划线标识符、定宽接口类型、4 空格、显式括号和清晰的头文件接口注释。遵守现有 ABI、Cortex-M 上下文保存、中断屏蔽及优先级契约。避免为了代码行数或指令数减少而删去生命周期及等待链保护。

内核、端口或 IPC 行为变化需要针对触发条件执行回归，并运行相关功能／边界及实板场景；记录真实执行的平台、配置和结果。O0／O3 编译成功不能表述为这两种配置已经上板。纯文档修改检查链接、数值和格式，不要求重复无关硬件测试。

HIL 保留失败尝试及缺失覆盖，按正常通过、预期负向失败和诊断排除分类。成功接纳、拒收、超时及溢出分别统计。软件替身或寄存器检查不能表述为物理 USB、传感器或 DMA 总线互操作测试。

## 证据与发布

Git 保存可阅读的日志、源码清单、JSON 汇总和方法说明。历史证据保持原始字节，不为了“全部通过”删除失败行或改写哈希。`.gitattributes` 禁止 HIL 文字证据的换行转换，避免克隆后日志内容未变却不再匹配记录的字节哈希。ELF 和 Debug 构建目录本地归档；没有原始二进制时须明确身份复核的限制。

发布前核对差异与暂存清单、`git diff --check`、提交内容关联的验证，以及远程分支是否有新提交。使用普通推送；已有提交不通过强制推送覆盖。参与协作时优先通过独立分支和 Pull Request 进行人工审阅。
