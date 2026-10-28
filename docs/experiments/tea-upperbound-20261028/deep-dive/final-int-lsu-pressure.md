# 最终 12 宽组合：89 个 INT 点 LSU 压力扫描

扫描 run1676 / 40648104599 的 89 个所选 INT 点，读取各点最后 20M ROI stats 和 INI；cluster 内原始权重保存在 JSON，排名是单点压力而非 workload 加权收益。89 份 INI 相同：LQ256、物理 SQ128（StoreQueueMultiple=2）、RAR192、RAW128；RAR/RAW 出队上限各4，load/store completion index 推进各8/4；Sbuffer16、evict threshold8、release-on-miss=true。没有修改配置或运行新模拟。

| 点（cluster 权重） | 有区分力的原始计数 | 含义 / 边界 |
|---|---|---|
| bzip2_source_3760 (.127347) | 10,904,865 cycles；SbufferFullCycles 323,182（2.96%）；平均有效条目 7.872；Dcache mainpipe 拒绝 228,947 次；受理 180,601 次 | FullCycles 是对应周期无法接收条目，平均占用与偶发满可同时出现；拒绝是尝试次数。阈值驱动的 `sbufferEvictDuetoFull`=175,205 是发起驱逐原因，不是物理满周期。 |
| omnetpp_18492 (.313070) | 8,222,150 cycles；RAW 实际满 104,105 cycles（1.27%）、RAWQueueFull 3,936 次、RAW replay 3,866 次、RAW 平均 8.725/128；RAR 满 0 | 平均低不排除 burst 满；满周期、触发次数和 replay 不同且可重叠。可分别检查 RAW 容量与完成/出队速率。 |
| h264ref_sss_157672 (.0716096) | 3,197,359 cycles；RAR 实际满 19,918（.62%）、RAR replay 5,588、RAR 平均 25.750/192；RAW 满 14,374、LQ 不足完整入队包 7,915 | LQFullCycles 的判定是剩余条目不足 rename 宽度的完整包，不等于物理恰好 0；这些事件可能同时发生，不能相加。 |

bzip2_combined_5764 权重 .2439 也有 SbufferFullCycles 234,921/9,234,426（2.54%），是容量试验的第二个对照；mcf_6753 的 SbufferFullCycles 仅 1,389/15,869,153，而 cache-miss replay、bank-conflict replay 分别为 4,452,894 / 2,615,541 次，不能把 replay 次数读成队列满或停顿周期。mcf 的显著 `scheduler.ld*.issueOccupy` 按 `issue_queue.cc:890-923,966-970` 实际累加当周期从 replayQ 发射数，虽然统计描述写 replayQ blocked，也不是 replayQ 容量满周期。

`lsq_unit.cc:3380-3438` 的 LoadCompletionWidth/StoreCompletionWidth 控制按顺序推进已执行 load / 地址就绪 store 的完成索引，之后 RAR/RAW 清除受各自出队宽度影响；它们不是独立 completion queue 条目数。`processReplayQueues` 还受同一出队参数限制；现有结果没有显示完成索引宽度饱和或 backlog 的直接计数，队列满不证明出队宽度是根因。load IQ replayQsize 是 `issue_queue.hh:117` 固定32，`ready()` 在 `replayQ.size()>32` 阻挡 IQ 入队（issue_queue.cc:1329-1334），目前没有对应 CLI 参数；它与 RAW/RAR replay list 不同。

建议可证伪的单轴顺序（保持其余最终组合和同 slice/warmup/ROI）：

1. 对两个 bzip2 点使用 `--param='system.cpu[0].SbufferEntries=32'`，仍保持 threshold8；检查物理满周期、驱逐请求、拒绝、平均占用与 IPC。若容量增大没有用，再以单独或交互控制将 `SbufferEvictThreshold=16`；`lsq.cc:1689-1691` 在 unsentSize 超过 threshold 时启动驱逐，阈值与容量不是同一轴。
2. 对 omnetpp 单独 `--param='system.cpu[0].RAWQEntries=256'`；另做 `--param='system.cpu[0].RAWDequeuePerCycle=8'`，必要时 `--param='system.cpu[0].StoreCompletionWidth=8'`，分别验证空间与回收链，不能只用一个组合推断根因。
3. 对 h264ref 点单独 `--param='system.cpu[0].RARQEntries=384'`；再分别测 `RARDequeuePerCycle=8`、`LoadCompletionWidth=16`。如果复现效应微小，优先降级这个方向。

以上 FullCycles 可能与别的等待重叠，不是可直接折算的可省 CPI。保留 L2/L3 总容量，MSHR/bank 可另做单轴；出现明确容量/污染证据时可把扩 cache 作诊断。此表不是 suite 性能归因，详细全部点原始计数及权重见同名 JSON。
