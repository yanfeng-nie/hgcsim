# 我的配置

答题时创建的配置都放在这里（前端的“保存到我的配置”按钮写入的也是这里）：

| 目录 | 内容 | 前端位置 |
|---|---|---|
| `systems/` | 系统（硬件）配置，例如协同设计赛道的方案 | “系统配置”页 |
| `strategies/` | 调度策略（JSON 策略，或指向 C++ 插件的策略） | “任务与策略”页 |

规则：

- 配置名就是文件名（不含 `.json`），以字母或数字开头，只含字母、数字、下划线、短横线，长度 1~40。
- 不能与内置配置（`configs/systems/`、`configs/strategies/`）重名。
- 可以直接在本地新建或编辑这里的 JSON 文件，然后在前端点“刷新列表”。
- 命令行套件同样能读取这里的配置：`bash scripts/run_suite.sh --systems my_sys --strategies my_st`。
- 可用环境变量 `HGCSIM_USER_CONFIG_DIR` 把本目录换到别处。
