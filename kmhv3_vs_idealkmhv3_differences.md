# kmhv3 与 idealkmhv3 配置差异清单

对比文件：
- `configs/example/kmhv3.py`
- `configs/example/idealkmhv3.py`

1. [x] `Dispatch` 阶段：`kmhv3` 关闭（`enableDispatchStage=False`），`idealkmhv3` 开启（`enableDispatchStage=True`）。
2. [x] `ROB` 配置：`idealkmhv3` 的 `commitWidth/squashWidth=12`，`kmhv3` 为 `8`。
3. [x] `ROB` 配置：`idealkmhv3` 的 `RobCompressPolicy='kmhv3'`，`kmhv3` 为 `'none'`。
4. [x] `ROB` 配置：`idealkmhv3` 的 `numROBEntries=160`，`kmhv3` 为 `352`。
5. [x] `LSQ/SBuffer` 容量：`idealkmhv3` 的 `LQEntries/SQEntries/RARQEntries/RAWQEntries/SbufferEntries` 均大于 `kmhv3`。
6. [x] `LSQ/SBuffer` 阈值：`idealkmhv3` 的 `SbufferEvictThreshold=16`，`kmhv3` 为 `7`。
7. [x] `LSU` 行为：`idealkmhv3` 的 `sbufferBankWriteAccurately=True`，`kmhv3` 为 `False`。
8. [x] `LSU` 额外参数：`idealkmhv3` 新增 `DcacheSetDivNum=2`，`kmhv3` 未设置该项。
9. [x] `Scheduler` 约束：`kmhv3` 额外设置了 `disableAllRegArb`、`enableMainRdpOpt=False`、`intRegfileBanks=1` 以及 IQ 读端口映射；`idealkmhv3` 未设置这些限制。
10. [x] 分支预测器细项：`kmhv3` 额外显式配置了 `mbtb/tage/ittage/ubtb/abtb/ras` 的启用与 `resolvedUpdate`；`idealkmhv3` 仅设置 `ftq_size/fsq_size`。
11. [x] `L1 DCache` 端口：`idealkmhv3` 的 `tag_load_read_ports=100`，`kmhv3` 为 `3`。
12. [x] `L1 DCache` 行为：`kmhv3` 额外显式设置 `do_fast_writeline=False` 与 `prefetch_can_offload=False`；`idealkmhv3` 未设置这两项。
13. [x] `L2` 总线时延：`idealkmhv3` 的 `tol2bus` 为 `forward/response/hint = 0/0/0`，`kmhv3` 为 `3/3/1`。
14. [x] `L2` 带宽层配置：`idealkmhv3` 启用了 `layer_bandwidth_configs` 双端口配置；`kmhv3` 中该配置被注释。
15. [x] `classic_l2` 模式：`idealkmhv3` 设置 `slice_num=0`；`kmhv3` 为 `slice_num=4`，并额外设置了 `wpu/do_fast_writeline/prefetch_can_offload/replacement_policy`。
16. [x] `non-classic l2 wrapper`：`idealkmhv3` 设置 `data/dir_sram_banks=2`、`pipe_dir_write_stage=4`、`dir_read_bypass=True`；`kmhv3` 为 `1/1/3/False`，且 `kmhv3` 额外对 `inner_cache` 设置了 `wpu/do_fast_writeline/prefetch_can_offload`。
17. [x] `L3` 配置：`idealkmhv3` 设置 `mshrs=128`；`kmhv3` 为 `mshrs=64`，并额外设置 `do_fast_writeline=False`、`prefetch_can_offload=False`、`num_slices=4`。
18. [x] 启动默认参数：`idealkmhv3` 的 `args.l2_size='2MB'`，`kmhv3` 为 `'1MB'`。
19. [ ] 启动默认参数：`kmhv3` 额外设置 `args.kmh_align=True`，`idealkmhv3` 未设置。

## 跑分记录

- 场景：未开启任何差异项（即 `kmhv3.py` 原配置）
- `mcf`：`23.647`
- `omnetpp`：`15.448`

- 场景：开启差异项 `1, 2, 3, 4, 5, 6, 18`
- `mcf`：`25.49`
- `omnetpp`：`16.993`

- 场景：开启差异项 `1, 2, 3, 4, 5, 6, 10, 18`
- `mcf`：`25.726`
- `omnetpp`：`17.212`

- 场景：开启差异项 `1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 18`
- `mcf`：`26.283`
- `omnetpp`：`17.454`

- 场景：开启差异项 `1-12, 18`
- `mcf`：`26.23`
- `omnetpp`：`17.456`

- 场景：开启差异项 `1-14, 18`
- `mcf`：`27.278`
- `omnetpp`：`17.832`

- 场景：开启差异项 `1-17, 18`
- `mcf`：`待补充`
- `omnetpp`：`待补充`

- 场景：`Turbo_align` 版本的 `kmhv3` 配置
- `mcf`：`25.625`
- `omnetpp`：`17.47`

- 场景：`Turbo_align + dualPort` 版本的 `kmhv3` 配置
- `mcf`：`26.627`
- `omnetpp`：`17.570`

- 场景：当前 `kmhv3` + `L2 4SLICE + WPU + prefetch_can_offload + L3 MSHR增大` 4SLICE 未应用，在Turbo版本已经是默认4SCLICE了
- `mcf`：`26.664`
- `omnetpp`：`17.576`

- 场景：上述配置基础上增加第 `16` 项（`data_sram_banks=2`、`dir_sram_banks=2`、`pipe_dir_write_stage=4`、`dir_read_bypass=True`）
- `mcf`：`27.844`
- `omnetpp`：`17.611`

- 场景：上述配置基础上开启第 `18` 项（`args.l2_size='2MB'`）
- `mcf`：`28.48`
- `omnetpp`：`18.221`

- 场景：`Turbo_align + no_pf` 版本的 `kmhv3` 配置
- `mcf`：`18.363`
- `omnetpp`：`15.809`
