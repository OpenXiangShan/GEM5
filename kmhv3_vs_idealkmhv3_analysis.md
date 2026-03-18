# kmhv3.py 与 idealkmhv3.py 差异分析

本文档基于当前 `xs-dev` 基线分支上的两个配置文件进行对比：

- `configs/example/kmhv3.py`
- `configs/example/idealkmhv3.py`

目标是梳理当前源码层面仍然存在的配置差异，不包含历史实验分支上的临时改动。

## 1. 总体结论

当前 `kmhv3.py` 与 `idealkmhv3.py` 的差异已经不只是单个 cache 参数差异，而是覆盖了以下几个层面：

1. 前端与调度结构仍保留较多 `kmhv3` 风格限制。
2. ROB、LSU、LSQ 资源规模仍与 ideal 有明显差距。
3. BPU 在 `kmhv3.py` 中仍保留大量显式 RTL 对齐配置，而 `idealkmhv3.py` 更接近放开限制的配置。
4. L1/L2/L3 cache 侧在端口数、bank 数、目录时序、dual-port、L3 MSHR 等方面仍有实际差异。
5. 入口参数中 `l2_size`、`kmh_align`、linux workload 处理逻辑也存在差异。

如果只看当前默认 wrapper 路径下真正影响行为的差异，最重要的项集中在：

- Dispatch / Scheduler
- ROB / LSU / LSQ
- BPU
- L1 DCache / L2 wrapper / tol2bus / L3
- `args.kmh_align` 与 `args.l2_size`

## 2. 前端与调度差异

### 2.1 Dispatch

- `kmhv3.py`
  - `cpu.enableDispatchStage = False`
- `idealkmhv3.py`
  - `cpu.enableDispatchStage = True`

这意味着当前 `kmhv3.py` 仍关闭了独立 dispatch stage，而 ideal 打开了这一级。

### 2.2 Scheduler / Reg Arb

`kmhv3.py` 仍保留了一组显式的调度与寄存器读口限制：

- `cpu.scheduler.disableAllRegArb()`
- `cpu.scheduler.enableMainRdpOpt = False`
- `cpu.scheduler.intRegfileBanks = 1`
- 三组 IQ 的 `rp` 映射显式写死

`idealkmhv3.py` 没有这些限制设置。也就是说，当前 `kmhv3.py` 在调度器资源仲裁和寄存器文件 bank 使用上仍更保守。

## 3. ROB、LSU、LSQ 与 Value Predictor 差异

### 3.1 ROB

`kmhv3.py`：

- `cpu.commitWidth = 8`
- `cpu.squashWidth = 8`
- `cpu.RobCompressPolicy = 'none'`
- `cpu.numROBEntries = 352`

`idealkmhv3.py`：

- `cpu.commitWidth = 12`
- `cpu.squashWidth = 12`
- `cpu.RobCompressPolicy = 'kmhv3'`
- `cpu.numROBEntries = 160`

这说明两者在提交带宽、恢复带宽、ROB 压缩策略和 ROB 深度上都不同。

### 3.2 LSU / LSQ

`kmhv3.py`：

- `cpu.sbufferBankWriteAccurately = False`
- 没有 `cpu.DcacheSetDivNum = 2`
- `cpu.LQEntries = 72`
- `cpu.SQEntries = 56`
- `cpu.RARQEntries = 72`
- `cpu.RAWQEntries = 32`
- `cpu.SbufferEntries = 16`
- `cpu.SbufferEvictThreshold = 7`

`idealkmhv3.py`：

- `cpu.sbufferBankWriteAccurately = True`
- `cpu.DcacheSetDivNum = 2`
- `cpu.LQEntries = 128`
- `cpu.SQEntries = 64`
- `cpu.RARQEntries = 96`
- `cpu.RAWQEntries = 56`
- `cpu.SbufferEntries = 24`
- `cpu.SbufferEvictThreshold = 16`

也就是说，ideal 在 load/store 相关结构上整体更大、更激进。

### 3.3 Value Predictor

`idealkmhv3.py` 额外启用了：

- `cpu.valuePred = IdealConstantLVP()`

`kmhv3.py` 没有这一行。这是一个明确的功能性差异。

### 3.4 StoreSet 训练

`kmhv3.py` 还显式关闭了：

- `cpu.enable_storeSet_train = False`

`idealkmhv3.py` 没有对应设置。若默认值不是 `False`，这也是一项额外差异。

## 4. BPU 差异

当前 `kmhv3.py` 在 BPU 侧保留了大量显式配置，主要包括：

- `if args.btb_tage_upper_bound:` 分支
- `mbtb/tage/ittage resolvedUpdate`
- `ubtb`、`abtb`、`microtage`、`mbtb`、`tage`、`ittage`、`mgsc`、`ras` 显式 `enabled = True`
- `mbtb.numWays = 8`
- `mgsc` 只开 `I/P/Bias`，关闭 `Bw/L/G`

而 `idealkmhv3.py` 在 BPU 侧只保留了少量显式配置，例如：

- `ftq_size`
- `fsq_size`

因此当前 `kmhv3.py` 不是简单的 “ideal 子集”，而是显式保留了一套更接近 RTL 对齐语义的 BPU 配置。

## 5. Cache 差异

### 5.1 L1 DCache

`kmhv3.py`：

- `cpu.dcache.tag_load_read_ports = 3`
- `cpu.dcache.do_fast_writeline = False`
- `cpu.dcache.prefetch_can_offload = False`

`idealkmhv3.py`：

- `cpu.dcache.tag_load_read_ports = 100`
- 没有后面两项显式限制

这里既有端口数差异，也有 cache 行为差异。

### 5.2 L2 classic 分支

`kmhv3.py` 在 `args.classic_l2` 分支下仍保留：

- `system.l2_caches[i].slice_num = 4`
- `system.l2_caches[i].wpu = NULL`
- `system.l2_caches[i].do_fast_writeline = False`
- `system.l2_caches[i].prefetch_can_offload = False`
- `system.l2_caches[i].replacement_policy = XSDRRIPRP(...)`

`idealkmhv3.py`：

- `system.l2_caches[i].slice_num = 0`
- 未显式设置其余这些限制项

不过这一段只在显式启用 `classic_l2` 时才会生效。

### 5.3 L2 wrapper 分支

当前默认更重要的是 wrapper 路径。

`kmhv3.py`：

- `l2_wrapper.data_sram_banks = 1`
- `l2_wrapper.dir_sram_banks = 1`
- `l2_wrapper.pipe_dir_write_stage = 3`
- `l2_wrapper.dir_read_bypass = False`

同时每个 `inner_cache` 还显式设置：

- `wpu = NULL`
- `do_fast_writeline = False`
- `prefetch_can_offload = False`
- `replacement_policy = XSDRRIPRP(...)`

`idealkmhv3.py`：

- `l2_wrapper.data_sram_banks = 2`
- `l2_wrapper.dir_sram_banks = 2`
- `l2_wrapper.pipe_dir_write_stage = 4`
- `l2_wrapper.dir_read_bypass = True`

同时 `inner_cache` 只保留：

- `replacement_policy = XSDRRIPRP(...)`

因此当前 wrapper 路径下最关键的结构差异，就是 L2 data/dir bank 数、目录写阶段和目录读 bypass。

### 5.4 tol2bus / dual-port

`kmhv3.py`：

- `forward_latency = 3`
- `response_latency = 3`
- `hint_wakeup_ahead_cycles = 1`
- `layer_bandwidth_configs` 仍注释掉，未开启 dual-port

`idealkmhv3.py`：

- `forward_latency = 0`
- `response_latency = 0`
- `hint_wakeup_ahead_cycles = 0`
- 开启了 `layer_bandwidth_configs`

也就是说，当前 `kmhv3.py` 在 `tol2bus` 路径上仍然没有对齐 ideal 的双端口和零延迟配置。

### 5.5 L3

`kmhv3.py`：

- `system.l3.mshrs = 64`
- `system.l3.do_fast_writeline = False`
- `system.l3.prefetch_can_offload = False`
- `system.l3.num_slices = 4`

`idealkmhv3.py`：

- `system.l3.mshrs = 128`
- 其余三项未显式设置

因此 L3 在 MSHR 规模和附加 cache 行为上都不同。

## 6. 入口参数与主流程差异

### 6.1 参数默认覆盖

`kmhv3.py`：

- `args.enable_pf_buffer = True`
- `args.bp_type = 'DecoupledBPUWithBTB'`
- `args.l2_size = '1MB'`
- `args.kmh_align = True`

`idealkmhv3.py`：

- `args.enable_pf_buffer = False`
- `args.bp_type = 'DecoupledBPUWithBTB'`
- `args.l2_size = '2MB'`
- 没有 `args.kmh_align = True`

这里主要差异是：

- `enable_pf_buffer`
- `l2_size`
- `kmh_align`

### 6.2 Linux workload 处理

`kmhv3.py` 末尾还保留：

- `if args.xiangshan_ecore and args.enable_riscv_vector: ...`
- `if len(args.os_type) > 0: ...`
- `if linux.bin != None: configure_xiangshan_linux_workload(args, system)`

`idealkmhv3.py` 没有最后这段 linux workload 配置逻辑。

## 7. 当前最值得关注的有效差异

如果目的是分析当前默认运行路径下，`kmhv3.py` 与 `idealkmhv3.py` 哪些差异最可能真正影响性能或行为，优先级可按以下顺序理解：

1. Dispatch / Scheduler / ROB
2. LSU / LSQ 规模与 `sbufferBankWriteAccurately`
3. BPU 显式对齐配置
4. L1 DCache 的 `tag_load_read_ports`
5. L2 wrapper 的 `data_sram_banks / dir_sram_banks / pipe_dir_write_stage / dir_read_bypass`
6. `tol2bus` dual-port 与延迟参数
7. L3 的 `mshrs`
8. `args.l2_size` 与 `args.kmh_align`

如果只讨论当前 wrapper 路径的 cache 行为，那么 classic L2 分支差异的重要性会下降，因为默认并不走 classic 分支。

## 8. 小结

当前 `kmhv3.py` 仍然不是简单的 “接近 ideal，只差少数 cache 参数”。它保留了一整套较强的 `kmhv3` 风格结构限制，尤其集中在：

- Dispatch / Scheduler / ROB
- LSU / LSQ
- BPU
- L1/L2/L3 结构资源与目录时序
- `kmh_align`、`l2_size` 等入口参数

因此，若后续要继续做“向 ideal 对齐”的实验，建议不要只盯 cache 层，而是把差异拆成以下几组分别验证：

1. 前后端结构组
2. LSU / LSQ 资源组
3. BPU 组
4. L1/L2/L3 cache 组
5. 入口参数组

这样更容易把每一组改动对 `mcf` / `omnetpp` 的影响拆解清楚。
