# PerfCCT Top10 诊断与候选建议

本工具在核心采集与原始查询之上整理调查入口，不执行模拟、不修改输入 DB。
数据约定与 OFF/ON 方法见 [PerfCCT_causal.md](PerfCCT_causal.md)。

## Top10 诊断

所有数据库查询使用 SQLite 只读连接；默认 Top5；`--top 10` 扩大调查预算。

```sh
python3 util/perfcct_query.py trace.db diagnose --cpu system.cpu --tid 0 \
  --stats stats.txt --top 10 --limit 128 > diagnose.json
```

`diagnose` 要求明确 CPU/TID；`--stats` 用最后一个完整统计段计算
`[finalTick-simTicks,finalTick)`，尾部未完成段报错。也可显式 `--start/--end`。
工具报告 ROI 与已知 trace 范围的关系，不把数据存在等同于运行成功。

诊断分别输出：

- 症状 PC：ROI 内已提交操作最终 `iq_to_fu`、`execution_or_memory`、
  `rob_drain` 片段累计。缺失端点不补造，阶段逆序实例排除。
  分数是 instruction-ticks，重叠受害者不能相加为运行时间。
- 零提交 blocker PC：仅实际提交失败，按 PC 合并区间；partial 使用独立榜。
  所有 PC 的整体区间并集另列，不把不同 PC 之和当成独占损失。
- 可选 Fetch 部分传送锚点：定位传送批次，不等于造成空槽的分支或前端根因。
- IEW dispatch 槽位分布与调查入口：传播标签只作观察背景，单位不同于 span ticks。

SQL 先聚合全部合格对象，再截 TopN。`selection_audits` 给出合格 PC、
保留/遗漏数量与各榜独立分母；Top10 不保证主要瓶颈已覆盖。
零提交 Top1 与 partial Top1 查询 p50/p95/max 的不同实例，其他 blocker
取 p95；Top1 另按时间分散筛查至多64段，明确未抽数量。这不是随机抽样，
不能估计总体机制比例；最长实例也可能只是少见尾部。

配对等待要求同 attempt 和兼容 wake source；STLF 还须相同已知 store。
重复 begin 保留最新锚点，未知来源保持 unmatched。等待与 span 的交集
不证明可恢复时间。窗口内时间戳计数也不等于等待区间长度。
`--limit` 限制实例局部证据，默认128；事件优先窗口，少量前后/依赖上下文
共享预算，遗漏与续页 SQL 显式输出。聚合排名仍可能扫描较多数据。
沿精确身份/请求深入 `inst`、`chain`、`resources`，缺失或截断不能证明不存在等待。

## 从诊断到候选建议

`util/perfcct_candidates.py` 读取以下 case 目录，不执行模拟器：

```text
case/
  diagnose.json
  validation.json
  off/{command.json,config.ini,stats.txt,topMisPredicts.csv(optional)}
  on/command.json
```

```sh
python3 util/perfcct_candidates.py --case-dir case --output-dir reports/case
python3 util/perfcct_candidates.py --case-dir case --no-interventions
```

脚本生成 `candidates.json` / `CANDIDATES.md`。从有效 dispatch 分布选择
主调查入口，再优先保留直接局部证据、重复动态实例，至多三个候选。
`candidate_selection_audit` 保留排序和遗漏；标签、关系边与实际 blocker
交叠的证据层级分开。每项列事实、局部机制、未知环节和下一验证，
`root_cause_status` 始终为 `not_established`。

建议门槛需要调用方提供可复核的运行记录，不负责生成或认证这些记录：

- `validation.json` 的 off/on：`status=passed`、host exit0、difftest/ref
  初始化、两段 stats、无错误、maxinst marker；ON trace end 必须匹配 marker。
- `timing_invariance.status=passed`，两段 `difference_count` 均为0
  （`difference_counts=[0,0]` 或 `segments` 格式）。
- command JSON：`command`（或 `argv`）、匹配的 `binary_sha256` 和
  `reference_sha256`；当前建议策略要求5M+5M，并核对 OFF 未启用 lifetime/causal。
- diagnosis ROI 与最终 OFF stats/trace 覆盖匹配；实际配置容量与测量段计数存在。

没有这些条件仍可列候选和补证请求，但不提出容量干预。
目前只在多个实际 blocker 的 translation 配对与 DTLB miss 相互支撑，
或多个真实 NoMSHR 拒绝与测量计数相互支撑时，建议独立容量敏感性对照，
至多两项；缺失证据不自动关闭预取或扩大其他资源。
`StoreL1Bound` 可来自已 issued、无 pendingCacheReq 的 ROB-head store，
`IQFull` 可混合容量/输入失败并传播到所有 lanes；两者不证明 SB/L1 根因。
branch CSV 覆盖全运行，未与 measurement 对齐时仅作热点地址线索。

## 验证与结论边界

```sh
python3 -m unittest discover -s util -p 'test_perfcct*.py'
```

合成测试覆盖统计段完整性、ROI、阶段缺口、身份隔离、等待配对、TopN
审计、证据预算与候选建议门槛。开发期完整单核切片的 OFF/ON 配对已验证
非 host stats 一致；候选输出仍只是该窗口的调查假设，不表示优化已成立。
采集成功、排名覆盖和根因可信度是不同问题。等待/状态/关系不能替代实际
资源解除与独立性能对照；缺少前端恢复、下层服务或端到端链时保留 unknown。
