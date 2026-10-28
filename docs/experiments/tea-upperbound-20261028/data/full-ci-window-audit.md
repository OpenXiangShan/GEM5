# 完整 CI 窗口口径复核

八组各 145/145 点通过，共 **1160/1160 点，异常 0**。每点的 CPU INI 均为 warmupInstCount=20M、max_insts_any_thread=40M；日志均出现 warmup dump/reset；stats 均有恰好两个完整块，首末块 `simInsts` 与 `system.cpu.committedInsts` 均在 20M ±16 内。首块实际范围 20,000,000–20,000,012，末块 19,999,989–20,000,011；各组每个实际值的数量保存在 [JSON](full-ci-window-audit.json)。

| 组 | run | 窗口通过 / 应有 |
|---|---:|---:|
| baseline24 | 40638589458 | 145 / 145 |
| R4-fourload | 40646873569 | 145 / 145 |
| R5-fourload-window | 40648078898 | 145 / 145 |
| R1-frontend | 40641369657 | 145 / 145 |
| R4-fourload-frontend | 40646888439 | 145 / 145 |
| R5-fourload-window-frontend | 40648104599 | 145 / 145 |
| R7-single-supply-override | 40651698971 | 145 / 145 |
| R7-double-supply-override | 40651748060 | 145 / 145 |

这次只检查参数、日志和指令窗口，不重复其它配置、正确性或计分审计。结果支持这八组完整 CI 的窗口没有受到本地 maxinsts 口径错误影响。
