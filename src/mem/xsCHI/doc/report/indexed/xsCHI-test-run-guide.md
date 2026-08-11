# xsCHI 测试运行指南

本文档给出使用 xsCHI 跑 checkpoint、编写包装脚本和批量任务的最短路径。更细的拓扑挂载、ShadowRN 地址映射和多 HN/DRAM 规则见 [TopoSys Config Guide](TopoSys_Multicore_Config_Test_Guide.md)。

## 1. 运行入口

| 场景 | 推荐入口 | 说明 |
|---|---|---|
| 单个 checkpoint 冒烟 | `configs/example/xiangshanCHI.py` | 该入口会设置 `args.CHI = True`，通常不需要再手动传 `--CHI` |
| 固定拓扑快速运行 | `util/xs_scripts/kmh_v3_CHI_*.sh` | 包装了常用 3x3、5x3、6x6 配置 |
| 批量 checkpoint | `util/xs_scripts/parallel_sim.sh` | 每个 workload 进入独立目录，保留 `log.txt`、`stats.txt`、`config.ini` |

> 说明：脚本名中的 `1core/2core/4core/8core/16core` 在当前 xsCHI 测试口径中通常表示 `1` 个主 RN 加若干 ShadowRN 流量源，不等价于完整多 CPU 一致性系统。

## 2. 环境准备

| 项 | 命令或取值 | 说明 |
|---|---|---|
| 进入仓库 | `cd /nfs/home/wuchengkai/GEM5_reps/GEM5` | 后续命令默认在仓库根目录执行 |
| NEMU 参考模型 | `export GCBV_REF_SO=/nfs/home/yanyue/tools/gem5-tools/ref-h/riscv64-nemu-interpreter-so` | difftest 运行需要 |
| restorer | `export GCB_RESTORER=` | 当前脚本会检查该变量是否已导出 |
| checkpoint | `export CKPT=/path/to/checkpoint.gz` | 传给 `--generic-rv-cpt` 或包装脚本第一个参数 |
| 构建产物选择 | `export GEM5_BUILD_TYPE=opt` | 包装脚本默认使用 `build/RISCV/gem5.opt` |

构建优化版：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
```

构建调试版：

```bash
scons build/RISCV/gem5.debug --gold-linker -j64 --debug-cycle
```

## 3. 最小冒烟运行

3x3 拓扑适合快速验证 xsCHI 是否能从 checkpoint 跑通。

```bash
OUT=m5out/xschi_smoke_3x3

./build/RISCV/gem5.opt --outdir="$OUT" \
  configs/example/xiangshanCHI.py \
  --generic-rv-cpt="$CKPT" \
  --bp-type=DecoupledBPUWithBTB \
  --chi-topology=L2L3DramSys_3x3
```

运行后优先检查：

| 文件 | 看什么 |
|---|---|
| `$OUT/simout` | 是否出现正常退出原因，是否有 `[xsCHI][Build]` 拓扑打印 |
| `$OUT/config.ini` | 是否存在 `system.CHIsys`、`MeshNode*`、`HNs`、`dramsim3s` 等对象 |
| `$OUT/stats.txt` | 是否有 `system.CHIsys.MeshNode*.network.*` 统计 |

常用检查命令：

```bash
rg -n "Exiting|panic|fatal|\\[xsCHI\\]\\[Build\\]" "$OUT/simout"
rg -n "system\\.CHIsys\\.MeshNode[0-9]+\\.network\\.(ingress_flits_by_channel|hop_count_hist_req|e2e_latency_hist_req)" "$OUT/stats.txt"
```

## 4. 常用包装脚本

包装脚本适合直接复用既有拓扑。脚本内部会从 `util/xs_scripts/common.sh` 推导 `gem5_home` 和 gem5 二进制路径。

| 脚本 | 拓扑 | 流量源口径 | 适用场景 |
|---|---|---:|---|
| `util/xs_scripts/kmh_v3_CHI_1core.sh` | `L2L3DramSys_3x3` | 1 RN | 最轻量功能冒烟 |
| `util/xs_scripts/kmh_v3_CHI_4core.sh` | `L2L3DramSys_3x3` | 1 RN + 3 ShadowRN | 3x3 多流量源压力 |
| `util/xs_scripts/kmh_v3_CHI_1core_5x3_cmn700-rtl.sh` | `L2L3DramSys_5x3` | 1 RN | 5x3 cmn700_rtl 单源基线 |
| `util/xs_scripts/kmh_v3_CHI_4core_5x3_cmn700-rtl.sh` | `L2L3DramSys_5x3` | 1 RN + 3 ShadowRN | 5x3 多源、多 HN/DRAM 压力 |
| `util/xs_scripts/kmh_v3_CHI_1core_6x6_cmn700-rtl.sh` | `L2L3DramSys_6x6` | 1 RN | CMN700-like 6x6 单源基线 |
| `util/xs_scripts/kmh_v3_CHI_2core_6x6_cmn700-rtl.sh` | `L2L3DramSys_6x6` | 1 RN + 1 ShadowRN | CMN700-like 6x6 小压力 |
| `util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh` | `L2L3DramSys_6x6` | 1 RN + 3 ShadowRN | CMN700-like 6x6 中等压力 |
| `util/xs_scripts/kmh_v3_CHI_8core_6x6_cmn700-rtl.sh` | `L2L3DramSys_6x6` | 1 RN + 7 ShadowRN | CMN700-like 6x6 大压力 |
| `util/xs_scripts/kmh_v3_CHI_16core_6x6_cmn700-rtl.sh` | `L2L3DramSys_6x6` | 1 RN + 15 ShadowRN | CMN700-like 6x6 满 HN 区域压力 |

单个 checkpoint 运行示例：

```bash
bash util/xs_scripts/kmh_v3_CHI_4core_5x3_cmn700-rtl.sh "$CKPT"
```

如果不通过 `parallel_sim.sh`，包装脚本默认写当前工作目录下的 `m5out/`。建议在独立目录中运行，避免覆盖已有结果。

## 5. 直接配置 5x3 cmn700_rtl

需要临时改拓扑、credit 或 HN/DRAM 参数时，可以直接调用 `xiangshanCHI.py`：

```bash
OUT=m5out/xschi_5x3_4src_cmn700_rtl

./build/RISCV/gem5.opt --outdir="$OUT" \
  configs/example/xiangshanCHI.py \
  --generic-rv-cpt="$CKPT" \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=3 \
  --shadow-attach-points=mesh14.local0,mesh10.local0,mesh4.local0 \
  --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
  --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000 \
  --chi-topology=L2L3DramSys_5x3 \
  --chi-hn-count=8 \
  --chi-hn-attach-points=mesh1.local0,mesh2.local0,mesh3.local0,mesh6.local0,mesh7.local0,mesh8.local0,mesh11.local0,mesh12.local0 \
  --chi-dram-count=2 \
  --chi-dram-attach-points=mesh5.local0,mesh9.local0 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-voq-depth=2
```

关键约束：

| 参数组 | 规则 |
|---|---|
| `--chi-credit-model=cmn700_rtl` | 需要满足 `rxbuf_num == skid_depth + ib_depth`，常用值是 `3 == 1 + 2` |
| `--chi-credit-model=cmn700_rtl` 下的 VOQ 参数 | `--chi-voq-depth` 和 `--chi-voq-depth-mode` 不作为有效 backpressure 控制项；MeshNode 入口判满使用 `--chi-ib-depth` |
| `--shadow-l2-count` | 必须和 `shadow-attach/src/window/dst` 列表长度一致；单个 src/window 值可由配置逻辑扩展到多个 shadow |
| `--chi-hn-count` | 多 HN 时 `--l3_size` 和 `--l3_mshrs` 必须能按 HN 数量整分 |
| `--chi-dram-count` | 多 DRAM 地址交织依赖 xsCHI SAM/XOR 规则，建议保持 1/2/4 等 2 的幂数量 |
| attach point | 格式固定为 `meshN.local0` 或 `meshN.local1`，同一 local 口不能重复占用 |

### 5.1 常用 option 汇总

下表按运行命令中常见参数分组。`configs/example/xiangshanCHI.py` 会在入口中设置 `args.CHI = True`，因此使用该配置文件时通常不需要额外传 `--CHI`。

#### 运行输入与输出

| 参数 | 典型值 | 含义 | 使用建议 |
|---|---|---|---|
| `--outdir` | `m5out/xschi_smoke_3x3` | gem5 输出目录，保存 `simout`、`stats.txt`、`config.ini` 等结果 | 直接跑单个任务时建议显式指定，避免覆盖默认 `m5out/` |
| `--generic-rv-cpt` | `$CKPT` | RISC-V checkpoint 或裸二进制输入路径 | 包装脚本中通常写成 `--generic-rv-cpt=$1` |
| `--raw-cpt` | 无值 flag | 表示 `--generic-rv-cpt` 指向 raw binary，而不是常规 checkpoint | 只有跑裸二进制时打开 |
| `--maxinsts` / `-I` | `50000000` | 每个 CPU 线程最多执行的指令数，到达后退出 | 批量跑分时建议固定，用于保证不同配置采样窗口一致 |
| `--bp-type` | `DecoupledBPUWithBTB` | 分支预测器类型 | 当前 xsCHI/KMHv3 脚本基本固定使用该值 |
| `--dramsim3-ini` | `ext/dramsim3/xiangshan_configs/...ini` | DRAMSim3 配置文件 | 多 DRAM 或特定通道配置时应显式指定 |
| `--mem-size` | `8GB` / `16GB` / `32GB` | gem5 物理内存范围大小 | 需要覆盖 shadow remap 后的目标地址窗口；8core/16core 示例会扩大内存 |

#### xsCHI 拓扑与挂点

| 参数 | 典型值 | 含义 | 使用建议 |
|---|---|---|---|
| `--chi-topology` | `L2L3DramSys_3x3` / `L2L3DramSys_5x3` / `L2L3DramSys_6x6` | 选择 xsCHI TopoSys 拓扑对象 | 3x3 用于冒烟，5x3/6x6 用于多 HN/DRAM 和 CMN700-like 实验 |
| `--chi-rn-attach-point` | `mesh7.local0` | 主 RN/L2 注入到 mesh 的 local 端口 | 6x6 cmn700_rtl 脚本固定为 `mesh7.local0` |
| `--chi-hn-count` | `8` / `16` | CHI_L3/HN endpoint 数量 | 多 HN 时会按数量平分总 L3 size 和 MSHR |
| `--chi-hn-attach-points` | `mesh7.local1,...` | HN endpoint 的 mesh 挂点列表 | 列表长度必须等于 `--chi-hn-count` |
| `--chi-dram-count` | `2` / `4` | DDRWrapper/DRAM endpoint 数量 | 多 DRAM 依赖 SAM/XOR 交织，建议保持 2 的幂 |
| `--chi-dram-attach-points` | `mesh1.local0,mesh4.local0,...` | DRAM/SN endpoint 的 mesh 挂点列表 | 列表长度必须等于 `--chi-dram-count` |

#### ShadowRN 流量源

| 参数 | 典型值 | 含义 | 使用建议 |
|---|---|---|---|
| `--shadow-l2-enable` | 无值 flag | 开启 ShadowRN replay，把主请求复制成额外 RN 流量 | 2core/4core/8core/16core 口径需要开启 |
| `--shadow-l2-count` | `1` / `3` / `7` / `15` | ShadowRN 数量 | `Ncore` 口径通常为 `N - 1` |
| `--shadow-attach-points` | `mesh10.local0,mesh25.local0,mesh28.local0` | 每个 ShadowRN 的 mesh 注入点 | 列表长度必须等于 `--shadow-l2-count` |
| `--shadow-src-bases` | `0x80000000` | 被复制请求的源地址窗口起点 | 单个值可扩展到多个 shadow |
| `--shadow-window-sizes` | `0x80000000` | 源地址窗口大小 | 单个值可扩展到多个 shadow，必须大于 0 |
| `--shadow-dst-bases` | `0x100000000,0x180000000,...` | 每个 ShadowRN 重映射后的目标地址起点 | 列表长度必须等于 `--shadow-l2-count`，目标窗口不能重叠 |

Shadow 地址重映射公式：

```text
A' = dst_base + (A - src_base)
```

只有当原地址 `A` 落在 `[src_base, src_base + window_size)` 内时，该 shadow remap 才生效。

#### cmn700_rtl credit 与 queue

| 参数 | 典型值 | 含义 | 使用建议 |
|---|---|---|---|
| `--chi-credit-model` | `cmn700_rtl` | 选择 credit timing 模型；`cmn700_rtl` 使用 RXBUF + skid + IB + downstream release 口径 | 需要分析 CMN700-like backpressure 时使用 |
| `--chi-rxbuf-num` | `3` | 每个 channel 的 receive flit window 总 entry 数 | cmn700_rtl 常用 3 |
| `--chi-skid-depth` | `1` | CHIPort staging/skid entry 数 | cmn700_rtl 常用 1 |
| `--chi-ib-depth` | `2` | MeshNode ingress/pending admission budget | cmn700_rtl 常用 2 |
| `--chi-initial-credit-count` | `3` | sender 初始可见 credit 数 | 通常等于 `--chi-rxbuf-num` |
| `--chi-up-crd-lat-int` | `1` | endpoint 上传方向内部 credit latency 分量 | 6x6 脚本保持默认/显式 1 |
| `--chi-up-crd-lat-ext` | `2` | endpoint 上传方向外部 credit latency 分量 | 与上项合成 Up credit return latency |
| `--chi-dn-crd-lat-int` | `2` | CMN 下传 endpoint 方向内部 credit latency 分量 | 6x6 脚本保持默认/显式 2 |
| `--chi-dn-crd-lat-ext` | `1` | CMN 下传 endpoint 方向外部 credit latency 分量 | 与上项合成 Dn credit return latency |
| `--chi-internal-crd-lat` | `1` | mesh 内部方向端口 credit return latency | east/west/north/south 内部链路常用 1 |
| `--chi-voq-depth` | `2` | legacy/cmn700 VOQ backpressure 深度阈值；`cmn700_rtl` 下不启用该阈值作为主要判满控制 | `cmn700_rtl` 下请看 `--chi-ib-depth`，脚本中保留该参数主要用于兼容和显式记录 |
| `--chi-voq-depth-mode` | `per_ingress` / `aggregate` | legacy/cmn700 的 VOQ 判满口径；`cmn700_rtl` 下不会改变入口判满语义 | `cmn700_rtl` 下 MeshNode 使用 IB admission 口径，因此该选项不生效 |
| `--chi-ddr-read-response-padding-cycles` | `0` / 正整数 | DRAM read 完成后，DDRWrapper 额外延迟多少 cycle 再注入 DAT response | 只在做延迟对齐实验时使用 |

cmn700_rtl 的关键约束：

```text
chi_rxbuf_num == chi_skid_depth + chi_ib_depth
```

当前常用配置为：

```text
3 == 1 + 2
```

cmn700_rtl 下还有一个容易误解的点：

| 参数 | cmn700_rtl 下的实际口径 |
|---|---|
| `--chi-voq-depth` | 不作为 MeshNode 入口 backpressure 的有效深度；入口判满使用 `--chi-ib-depth` |
| `--chi-voq-depth-mode` | 不改变入口判满模式；`per_ingress` / `aggregate` 选择不会影响 cmn700_rtl 的 IB admission |

#### L3 与多 HN 资源切分

| 参数 | 典型值 | 含义 | 使用建议 |
|---|---|---|---|
| `--l3_size` | `64MB` | 所有 HN 共享的总 L3 容量预算 | 多 HN 时会按 `--chi-hn-count` 严格平分 |
| `--l3_mshrs` | `256` | 所有 HN 共享的总 L3 MSHR 预算 | 多 HN 时会按 `--chi-hn-count` 严格平分 |

例如 6x6 中 `--chi-hn-count=16 --l3_size=64MB --l3_mshrs=256`，每个 HN 获得：

| 项 | 每 HN 结果 |
|---|---:|
| L3 容量 | `4MB` |
| L3 MSHR | `16` |

## 6. 批量运行

`parallel_sim.sh` 的 workload list 每行通常包含：

```text
workload_name checkpoint_relative_path skip_inst functional_warmup detailed_warmup sample_inst
```

使用包装脚本批量运行：

```bash
export xsgem5_para_jobs=16

bash util/xs_scripts/parallel_sim.sh \
  util/xs_scripts/kmh_v3_CHI_4core_5x3_cmn700-rtl.sh \
  "$WORKLOAD_LIST" \
  "$CKPT_ROOT" \
  xschi_5x3_4src_batch
```

检查批量状态：

```bash
bash util/xs_scripts/check_parallel_runs.sh xschi_5x3_4src_batch --details
```

每个 workload 目录中重点看：

| 文件 | 用途 |
|---|---|
| `log.txt` | gem5 stdout/stderr，定位 panic、fatal、difftest 报错 |
| `stats.txt` | 性能统计和 MeshNode/CHI 诊断输入 |
| `config.ini` | 确认拓扑、HN/DRAM/ShadowRN 挂点是否按预期实例化 |
| `completed` / `abort` / `running` | `parallel_sim.sh` 的任务状态标记 |

## 附录 A：编写 .sh 包装脚本

包装脚本的目标是把一组稳定参数固化下来，只把 checkpoint 作为第一个位置参数传入。仓内脚本统一放在 `util/xs_scripts/`，并复用 `common.sh` 推导 `gem5_home` 和 `$gem5`。

### A.1 基本模板

```bash
#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshanCHI.py --generic-rv-cpt=$1 \
  --bp-type=DecoupledBPUWithBTB \
  --chi-topology=L2L3DramSys_3x3
```

### A.2 编写规则

| 规则 | 建议 |
|---|---|
| 脚本位置 | 放在 `util/xs_scripts/`，文件名体现核数、拓扑和 credit 模型，例如 `kmh_v3_CHI_4core_6x6_cmn700-rtl.sh` |
| checkpoint 参数 | 使用 `$1`，即 `--generic-rv-cpt=$1`；这样可直接被 `parallel_sim.sh` 调用 |
| 仓库路径 | 不写死仓库根目录，统一 `source $script_dir/common.sh` 后使用 `$gem5_home` |
| gem5 二进制 | 使用 `$gem5`，由 `GEM5_BUILD_TYPE` 控制 `opt/debug` |
| 环境检查 | 保留 `GCBV_REF_SO`、`GCB_RESTORER`、`gem5_home` 检查 |
| 长命令换行 | 每行末尾使用 `\`，最后一行不要加 `\` |
| ShadowRN | `Ncore` 通常对应 `shadow-l2-count=N-1`；1core 不打开 shadow |
| 输出目录 | 单脚本运行默认写当前目录 `m5out/`；批量运行由 `parallel_sim.sh` 在每个 workload 目录中执行 |

### A.3 4core 6x6 示例脚本

对应文件：[util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh:1)。

```bash
#!/usr/bin/env bash

script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
source $script_dir/common.sh

for var in GCBV_REF_SO GCB_RESTORER gem5_home; do
    checkForVariable $var
done

$gem5 $gem5_home/configs/example/xiangshanCHI.py --generic-rv-cpt=$1 \
  --mem-size=8GB \
  --dramsim3-ini="$gem5_home/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini" \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=3 \
  --shadow-attach-points=mesh10.local0,mesh25.local0,mesh28.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000 \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```

## 附录 B：6x6 运行示例

6x6 cmn700_rtl 脚本统一使用 `L2L3DramSys_6x6`、`RN@mesh7.local0`、16 个 HN、4 个 DRAM/SN，以及 `rxbuf/skid/IB = 3/1/2` 的 credit 配置。

| 核数口径 | ShadowRN 数 | 现有脚本 | 直接运行脚本 |
|---|---:|---|---|
| 1core | 0 | `util/xs_scripts/kmh_v3_CHI_1core_6x6_cmn700-rtl.sh` | `bash util/xs_scripts/kmh_v3_CHI_1core_6x6_cmn700-rtl.sh "$CKPT"` |
| 2core | 1 | `util/xs_scripts/kmh_v3_CHI_2core_6x6_cmn700-rtl.sh` | `bash util/xs_scripts/kmh_v3_CHI_2core_6x6_cmn700-rtl.sh "$CKPT"` |
| 4core | 3 | `util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh` | `bash util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh "$CKPT"` |
| 8core | 7 | `util/xs_scripts/kmh_v3_CHI_8core_6x6_cmn700-rtl.sh` | `bash util/xs_scripts/kmh_v3_CHI_8core_6x6_cmn700-rtl.sh "$CKPT"` |
| 16core | 15 | `util/xs_scripts/kmh_v3_CHI_16core_6x6_cmn700-rtl.sh` | `bash util/xs_scripts/kmh_v3_CHI_16core_6x6_cmn700-rtl.sh "$CKPT"` |

### B.1 1core 6x6

脚本：[util/xs_scripts/kmh_v3_CHI_1core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_1core_6x6_cmn700-rtl.sh:1)。

```bash
./build/RISCV/gem5.opt configs/example/xiangshanCHI.py --generic-rv-cpt="$CKPT" \
  --mem-size=8GB \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```

### B.2 2core 6x6

脚本：[util/xs_scripts/kmh_v3_CHI_2core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_2core_6x6_cmn700-rtl.sh:1)。

```bash
./build/RISCV/gem5.opt configs/example/xiangshanCHI.py --generic-rv-cpt="$CKPT" \
  --mem-size=8GB \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=1 \
  --shadow-attach-points=mesh28.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000 \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```

### B.3 4core 6x6

脚本：[util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_4core_6x6_cmn700-rtl.sh:1)。

```bash
./build/RISCV/gem5.opt configs/example/xiangshanCHI.py --generic-rv-cpt="$CKPT" \
  --mem-size=8GB \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=3 \
  --shadow-attach-points=mesh10.local0,mesh25.local0,mesh28.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000 \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```

### B.4 8core 6x6

脚本：[util/xs_scripts/kmh_v3_CHI_8core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_8core_6x6_cmn700-rtl.sh:1)。

```bash
./build/RISCV/gem5.opt configs/example/xiangshanCHI.py --generic-rv-cpt="$CKPT" \
  --mem-size=16GB \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=7 \
  --shadow-attach-points=mesh8.local0,mesh9.local0,mesh10.local0,mesh25.local0,mesh26.local0,mesh27.local0,mesh28.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000,0x280000000,0x300000000,0x380000000,0x400000000 \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```

### B.5 16core 6x6

脚本：[util/xs_scripts/kmh_v3_CHI_16core_6x6_cmn700-rtl.sh](/nfs/home/wuchengkai/GEM5_reps/GEM5/util/xs_scripts/kmh_v3_CHI_16core_6x6_cmn700-rtl.sh:1)。

```bash
./build/RISCV/gem5.opt configs/example/xiangshanCHI.py --generic-rv-cpt="$CKPT" \
  --mem-size=32GB \
  --dramsim3-ini=ext/dramsim3/xiangshan_configs/xiangshan_DDR4_32Gb_x8_3200_8ch.ini \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable \
  --shadow-l2-count=15 \
  --shadow-attach-points=mesh8.local0,mesh9.local0,mesh10.local0,mesh13.local0,mesh14.local0,mesh15.local0,mesh16.local0,mesh19.local0,mesh20.local0,mesh21.local0,mesh22.local0,mesh25.local0,mesh26.local0,mesh27.local0,mesh28.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000,0x280000000,0x300000000,0x380000000,0x400000000,0x480000000,0x500000000,0x580000000,0x600000000,0x680000000,0x700000000,0x780000000,0x800000000 \
  --chi-topology=L2L3DramSys_6x6 \
  --chi-rn-attach-point=mesh7.local0 \
  --chi-hn-count=16 \
  --chi-hn-attach-points=mesh7.local1,mesh8.local1,mesh9.local1,mesh10.local1,mesh13.local1,mesh14.local1,mesh15.local1,mesh16.local1,mesh19.local1,mesh20.local1,mesh21.local1,mesh22.local1,mesh25.local1,mesh26.local1,mesh27.local1,mesh28.local1 \
  --chi-dram-count=4 \
  --chi-dram-attach-points=mesh1.local0,mesh4.local0,mesh31.local0,mesh34.local0 \
  --l3_size=64MB \
  --l3_mshrs=256 \
  --chi-credit-model=cmn700_rtl \
  --chi-rxbuf-num=3 \
  --chi-skid-depth=1 \
  --chi-ib-depth=2 \
  --chi-initial-credit-count=3 \
  --chi-up-crd-lat-int=1 \
  --chi-up-crd-lat-ext=2 \
  --chi-dn-crd-lat-int=2 \
  --chi-dn-crd-lat-ext=1 \
  --chi-internal-crd-lat=1 \
  --chi-voq-depth=2
```
