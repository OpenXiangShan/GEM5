# PDB 最初模型

## 目标

把预取数据的驻留位置从 L1 DCache 分出来，减少无用预取对 L1 的污染。第一版只改变预取返回后的存放位置：预取请求仍走 DCache 原有的 MSHR 和下行通路，不引入独立的请求队列或下行带宽。PDB 容量可配置，初始设为 1024 条 cacheline。

## 基本行为

- 纯预取返回完整数据后存入 PDB，不分配或替换 L1 cacheline。PDB 中的行按替换策略逐出；本级不再持有该行时，向下层报告副本已释放。
- 新预取命中 PDB 中已有的有效完整数据时，与命中 L1 一样丢弃该预取，不发下行请求，也不改动已有 PDB 行；需要独占权限的预取仍须申请写权限。
- Demand Load 查询 L1 和 PDB。L1 未命中而 PDB 有有效完整数据时，直接用 PDB 响应；数据继续留在 PDB，供后续 Load 使用，不因 Load 命中而移入 L1。
- Store 命中 PDB 时，将已有数据交给 DCache MSHR 申请写权限，并立即使 PDB entry 无效，但不在移交时报告释放；若在途数据被 snoop 失效，则丢弃旧数据，重新取数并取得写权限后再完成 Store。
- L2 或其他核的 snoop 命中有效 PDB 行时，按本级持有干净副本处理：查询是否缓存该行要报告命中；普通读 snoop 保留该行并报告有共享副本，由下层提供数据；写权限或其他失效 snoop 则立即使该行无效，并按 snoop 流程更新下层的副本记录。

## 实现细节

- `Cache` 增加 `pdb_entries` 参数。只在可写的 L1 Cache 中启用；`kmhv3.py` 的 DCache 设为 1024，其他配置默认 0。PDB 以物理 cacheline 地址及 secure 位索引，保存完整数据和只读状态，按 LRU 逐出；不另设 MSHR 或下行端口。
- 原有硬件预取仍通过 DCache MSHR 发出。仅当返回是干净、完整、只有预取目标且没有待处理失效的读响应时，将数据写入 PDB 并跳过 L1 tag 分配，同时保留预取来源元数据、按 L1 fill 的口径计算数据就绪时间并通知原有 Fill/refill 监听器。混合 demand/预取目标、失效中或来自其他 cache 的响应暂走原有 L1 fill 路径。预取入队和出队时同时查询 L1 和 PDB，命中则丢弃新预取，不更新 PDB 的替换位置。
- Demand ReadReq 先按原路径查询 L1；L1 未命中、PDB 命中且没有同地址 MSHR/write buffer 冲突时，直接用 PDB 的数据响应，并更新 PDB LRU。命中通过现有 Hit probe 按 cacheline 地址向预取器提供来源等元数据用于训练；首次命中报告有用预取，之后清除未使用标记。延迟响应在 LSQ replay bus 中保留到 Load 提交或被 squash，供重放使用；普通 L1 回填仍按原路径清理对应 bus 记录。数据继续留在 PDB，不安排移入 L1。
- 普通 WriteReq 命中 PDB 时，当前实现先在 L1 分配一条只读行并复制 PDB 数据，随即使 PDB entry 无效，不发送 CleanEvict；新 L1 行的就绪时间取当前 tick 与 PDB 数据就绪时间的较大值。成功移交计为一次有用预取，移交失败按未使用处理。原有 MSHR 随后发送 UpgradeReq 取得写权限；在途 snoop 若使 L1 行失效，原有 MSHR 逻辑将请求改为重新取数的 ReadExReq，不能使用旧数据完成 Store。这条只读 L1 行仅用于移交期间复用现有的权限和失效处理，并非 Load 命中后的异步迁移。
- Timing/atomic snoop 查询 PDB：`mustCheckAbove` 报告已有副本；普通读报告存在 sharer、保留干净 PDB 行且由下层提供数据；失效 snoop 删除 PDB 行，由 snoop filter 的失效流程清除 holder。功能访问也查询并更新 PDB 数据；完整功能读命中时不再向内存读取，功能写仍向下传播。内存系统全局失效时清空 PDB。
- PDB 因容量逐出、不可缓存请求或 Store 移交失败而丢失独有副本时，通过现有 write buffer 发送 CleanEvict；同地址 MSHR 在途时暂缓释放，待 MSHR 最终退役且 L1、PDB、write buffer 均未接手该行时再发送。失效 snoop 会取消尚未发送的 CleanEvict；若 MSHR 尚未下发，也取消暂缓释放，已下发时则等待响应后检查副本状态。普通软件预取命中 PDB 时直接响应，不重复取数，也不更新 PDB LRU 或将其计为有用预取；独占软件预取仍按原路径取得写权限。未被 demand 使用的 PDB 行在失效或逐出时通知预取器。统计位于 `dcache.pdb`：`fills`、`loadHits`、`storeHits`、`duplicatePrefetches`、`evictions`、`snoopInvalidations` 和 `occupancy`；`resetstats` 后按当前驻留行数恢复 `occupancy`。PDB 实际填入也计入纯预取 fill 总数 `pfOnlyFill`，重复预取不计入。
- `dcache.pdb.usefulLatency` 记录 PDB 中纯预取行从写入 PDB 到首次 demand Load 命中或成功 Store promotion 的周期分布；未被使用而逐出的行、重复预取和软件预取命中不产生样本，每个 PDB 行最多一个样本。该统计由 PDB 内部维护，不要求各预取器增加单独逻辑。
- `dcache.pdb.refillToReplaceLatency` 记录 PDB 行从写入到因容量替换而逐出的周期分布；`dcache.pdb.usedToReplaceLatency` 仅对已被 demand 首次使用、之后仍留在 PDB 并因容量替换而逐出的行，记录从首次使用到替换的周期分布。两者与 `usefulLatency` 使用相同的 0--65536 cycle 范围、64-cycle bucket 和 PDF 输出；snoop invalidation、Store promotion 及其他非容量删除不产生替换样本。
