# xsCHI TopoSys 多核模型配置与测试指南

> 适用仓库：`GEM5仓库分支：CHI-dev`
> 使用配置：`configs/example/xiangshanCHI.py`  

---

## 1. 背景与口径

在当前 xsCHI 实现中，“2核/4核”有两种常见口径：

| 口径 | 含义 | 典型参数 |
|---|---|---|
| 多 RN 流量源模型（常用） | `1` 个主 L2 + `N` 个 Shadow L2，形成 `1+N` 个 RN 注入点 | `--shadow-l2-enable --shadow-l2-count=N` |

### 1.1 本文采用的“2核/4核”定义

为贴合当前 `xsCHI/TopoSys` 常用测试方式，本文命令中的：

| 术语 | 定义 |
|---|---|
| 2核模型 | `1` 主 RN + `1` Shadow RN（`shadow-l2-count=1`） |
| 4核模型 | `1` 主 RN + `3` Shadow RN（`shadow-l2-count=3`） |

---

## 2. 通过TopoSys 创建不同拓扑

### 2.1 拓扑实例

| 拓扑 | 思路 | 端点默认放置 | 适用场景 |
|---|---|---|---|
| `L2ToDramSys` | 2x2 Mesh + `FakeL3`，最轻量 | `RN@Mesh0.local0`，`HN@Mesh1.local0`，`DRAM@Mesh2.local0` | 快速连通性/功能冒烟 |
| `L2L3DramSys_M1Local1Dram` | 2x2 Mesh + `CHI_L3`，HN/DRAM 同节点不同 local | `RN@Mesh0.local0`，`HN@Mesh1.local0`，`DRAM@Mesh1.local1` | 比较 HN/DRAM 同节点口竞争 |
| `L2L3DramSys_3x3` | 3x3 Mesh + 可扩展 shadow 挂点 | `RN@Mesh0.local0`，`HN@Mesh4.local0`，`DRAM@Mesh4.local1` | 多流量源压力、拓扑扩展实验 |
| `L2L3DramSys_5x3` | 5x3 Mesh + 多 HN/DRAM + shadow 挂点 | `RN@M0.local0`，默认 `HN@M6.local0`，默认 `DRAM@M6.local1` | 多目标分流、多 HN/多 DRAM 挂载实验 |

当前主要使用L2L3DramSys_3x3，如需要不同的拓扑可以参考L2L3DramSys_3x3生成新的拓扑
5x3 拓扑支持通过 `--chi-hn-count` / `--chi-hn-attach-points` 和 `--chi-dram-count` / `--chi-dram-attach-points` 显式配置多个 HN/DRAM；默认保持单 HN + 单 DRAM，分别挂在 `M6.local0` 与 `M6.local1`。当 `HN > 1` 时，`L3` 总容量与总 `MSHR` 预算会在 Python 配置层按 `HN` 数量严格等分，不再因为实例数增加而自动放大总量。

### 2.2 拓扑关键参数

| 参数 | 说明 | 备注 |
|---|---|---|
| `--chi-topology` | 选择 TopoSys 类型 | 候选见 `configs/common/xiangshan.py` |
| `--chi-voq-depth` | MeshNode VOQ 深度阈值 | 默认 `2` |
| `--chi-voq-depth-mode` | 回压口径：`per_ingress` / `aggregate` | 默认 `per_ingress` |
| `--chi-hn-count` / `--chi-hn-attach-points` | 5x3 多 HN 数量与挂点 | 默认 `1` / `mesh6.local0`；多 HN 时 `L3 size` 与 `MSHR` 自动均分 |
| `--chi-dram-count` / `--chi-dram-attach-points` | 5x3 多 DRAM 数量与挂点 | 默认 `1` / `mesh6.local1` |

### 2.3 三种拓扑配置示例

### 示例 A：`L2ToDramSys`（2x2 + FakeL3）

```bash
./build/RISCV/gem5.opt --outdir=out/l2todram \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --chi-topology=L2ToDramSys \
  --bp-type=DecoupledBPUWithBTB
```

### 示例 B：`L2L3DramSys_M1Local1Dram`（2x2 + CHI_L3）

```bash
./build/RISCV/gem5.opt --outdir=out/l2l3_m1l1 \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --chi-topology=L2L3DramSys_M1Local1Dram \
  --bp-type=DecoupledBPUWithBTB
```

### 示例 C：`L2L3DramSys_3x3`（3x3 + shadow）

```bash
./build/RISCV/gem5.opt --outdir=out/l2l3_3x3 \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --chi-topology=L2L3DramSys_3x3 \
  --shadow-l2-enable --shadow-l2-count=3 \
  --shadow-attach-points=mesh8.local0,mesh6.local0,mesh2.local0 \
  --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
  --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000
```

### 示例 D：`L2L3DramSys_5x3`（5x3 + 多 HN/DRAM）

```bash
./build/RISCV/gem5.opt --outdir=out/l2l3_5x3 \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --chi-topology=L2L3DramSys_5x3 \
  --chi-hn-count=2 --chi-hn-attach-points=mesh6.local0,mesh8.local0 \
  --chi-dram-count=2 --chi-dram-attach-points=mesh6.local1,mesh8.local1 \
  --shadow-l2-enable --shadow-l2-count=2 \
  --shadow-attach-points=mesh14.local0,mesh12.local0
```

---

## 3. 设备绑定到不同 node 的方法

### 3.1 设备与 node 映射规则

| 规则 | 说明 | 失败行为 |
|---|---|---|
| 挂点语法固定 | 每个 shadow 挂点必须是 `meshX.localY` | 启动 `panic` |
| local 口唯一占用 | 同一 `meshX.localY` 不能重复连接 | 启动 `panic` |
| shadow NodeID 自动推导 | `NodeID(meshX, meshY, localPort)` | 重复 NodeID 会 `panic` |
| 每个 shadow 目标统一路由至 HN | shadow 的 SAM 会加到 HN NodeID | 由 TopoSys 构造完成 |
| 地址窗口必须合法 | `window>0`，且无溢出、目标窗口不重叠 | 校验失败 `panic` |

### 3.2 Shadow 地址映射公式

| 项 | 公式 |
|---|---|
| 映射 | `A' = dst_base + (A - src_base)` |
| 生效条件 | `A ∈ [src_base, src_base + window_size)` |

### 3.3 配置片段示例 + 参数解释

```bash
--shadow-l2-enable \
--shadow-l2-count=3 \
--shadow-attach-points=mesh3.local0,mesh2.local0,mesh0.local1 \
--shadow-src-bases=0x80000000,0x80000000,0x80000000 \
--shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
--shadow-dst-bases=0x100000000,0x180000000,0x200000000
```

| 参数 | 含义 | 常见错误 |
|---|---|---|
| `--shadow-l2-enable` | 开启 shadow 流量注入 | 未开启却传 shadow 其他参数 |
| `--shadow-l2-count` | shadow 数量 | 与后续 CSV 列表长度不一致 |
| `--shadow-attach-points` | 每个 shadow 的 node/local 绑定点 | 写错格式，如 `mesh8local0` |
| `--shadow-src-bases` | 原地址窗口起点 | 没覆盖实际请求地址 |
| `--shadow-window-sizes` | 原地址窗口大小 | `0` 或过小导致 remap 失败 |
| `--shadow-dst-bases` | 重映射目标起点 | 多窗口重叠导致校验失败 |

---

## 4. 关键命令行清单（2核/4核三组示例）

> 当前拓扑基线：`L2L3DramSys_M1Local1Dram`  
> 说明：以下所有命令都给出了 2核/4核两套，满足“三组示例”要求。

### 4.1 组 A：单 workload（raw 二进制）冒烟

### A-2C（2核流量模型）

```bash
./build/RISCV/gem5.opt --outdir=out/m1l1_2c_smoke \
  ./configs/example/xiangshanCHI.py \
  --raw-cpt --generic-rv-cpt=$BIN \
  --chi-topology=L2L3DramSys_M1Local1Dram \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable --shadow-l2-count=1 \
  --shadow-attach-points=mesh3.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000
```

### A-4C（4核流量模型）

```bash
./build/RISCV/gem5.opt --outdir=out/m1l1_4c_smoke \
  ./configs/example/xiangshanCHI.py \
  --raw-cpt --generic-rv-cpt=$BIN \
  --chi-topology=L2L3DramSys_M1Local1Dram \
  --bp-type=DecoupledBPUWithBTB \
  --shadow-l2-enable --shadow-l2-count=3 \
  --shadow-attach-points=mesh3.local0,mesh2.local0,mesh0.local1 \
  --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
  --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000
```

### 4.2 组 B：单 checkpoint 跑分（固定指令数）

### B-2C

```bash
./build/RISCV/gem5.opt --outdir=out/m1l1_2c_score \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --maxinsts=50000000 \
  --chi-topology=L2L3DramSys_M1Local1Dram \
  --shadow-l2-enable --shadow-l2-count=1 \
  --shadow-attach-points=mesh3.local0 \
  --shadow-src-bases=0x80000000 \
  --shadow-window-sizes=0x80000000 \
  --shadow-dst-bases=0x100000000
```

### B-4C

```bash
./build/RISCV/gem5.opt --outdir=out/m1l1_4c_score \
  ./configs/example/xiangshanCHI.py \
  --generic-rv-cpt=$CKPT \
  --maxinsts=50000000 \
  --chi-topology=L2L3DramSys_M1Local1Dram \
  --shadow-l2-enable --shadow-l2-count=3 \
  --shadow-attach-points=mesh3.local0,mesh2.local0,mesh0.local1 \
  --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
  --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
  --shadow-dst-bases=0x100000000,0x180000000,0x200000000
```

### 4.3 组 C：批量 checkpoint 跑分（parallel_sim）

### C-2C

```bash
bash util/xs_scripts/parallel_sim.sh \
  configs/example/xiangshanCHI.py \
  $WORKLOADS_LST \
  $CKPT_ROOT \
  m1l1_2c_batch \
  "--chi-topology=L2L3DramSys_M1Local1Dram --maxinsts=50000000 \
   --shadow-l2-enable --shadow-l2-count=1 \
   --shadow-attach-points=mesh3.local0 \
   --shadow-src-bases=0x80000000 \
   --shadow-window-sizes=0x80000000 \
   --shadow-dst-bases=0x100000000"
```

### C-4C

```bash
bash util/xs_scripts/parallel_sim.sh \
  configs/example/xiangshanCHI.py \
  $WORKLOADS_LST \
  $CKPT_ROOT \
  m1l1_4c_batch \
  "--chi-topology=L2L3DramSys_M1Local1Dram --maxinsts=50000000 \
   --shadow-l2-enable --shadow-l2-count=3 \
   --shadow-attach-points=mesh3.local0,mesh2.local0,mesh0.local1 \
   --shadow-src-bases=0x80000000,0x80000000,0x80000000 \
   --shadow-window-sizes=0x80000000,0x80000000,0x80000000 \
   --shadow-dst-bases=0x100000000,0x180000000,0x200000000"
```

---

## 5. 每条命令参数逐项解释（统一清单）

### 5.1 gem5 通用参数

| 参数 | 说明 | 典型取值 |
|---|---|---|
| `./build/RISCV/gem5.opt` | gem5 可执行文件（优化版） | 固定 |
| `--outdir=...` | 本次仿真输出目录 | `out/m1l1_2c_score` |
| `./configs/example/xiangshanCHI.py` | xsCHI 入口脚本 | 固定 |
| `--generic-rv-cpt=...` | checkpoint/bin 路径 | `$CKPT` / `$BIN` |
| `--raw-cpt` | 输入为裸二进制（非压缩 checkpoint） | raw bin 场景启用 |
| `--maxinsts=...` | 最大指令条数，到达即退出 | `50000000` |
| `--bp-type=DecoupledBPUWithBTB` | BPU 类型 | 常用固定值 |

### 5.2 TopoSys/CHI 参数

| 参数 | 说明 | 典型取值 |
|---|---|---|
| `--chi-topology=...` | 选择拓扑类 | `L2ToDramSys` / `L2L3DramSys_M1Local1Dram` / `L2L3DramSys_3x3` |
| `--chi-voq-depth=...` | MeshNode VOQ 深度阈值 | `1` / `2` |
| `--chi-voq-depth-mode=...` | 回压聚合口径 | `per_ingress` |

### 5.3 Shadow 参数

| 参数 | 说明 | 2C 示例 | 4C 示例 |
|---|---|---|---|
| `--shadow-l2-enable` | 开启 shadow 机制 | 开 | 开 |
| `--shadow-l2-count` | shadow 数量 | `1` | `3` |
| `--shadow-attach-points` | shadow 绑定点列表 | `mesh3.local0` | `mesh3.local0,mesh2.local0,mesh0.local1` |
| `--shadow-src-bases` | 原地址窗口起点列表 | `0x80000000` | `0x80000000,0x80000000,0x80000000` |
| `--shadow-window-sizes` | 原地址窗口大小列表 | `0x80000000` | `0x80000000,0x80000000,0x80000000` |
| `--shadow-dst-bases` | 目标重映射起点列表 | `0x100000000` | `0x100000000,0x180000000,0x200000000` |

### 5.4 批量脚本参数（`parallel_sim.sh`）

| 位置 | 参数 | 说明 |
|---|---|---|
| `$1` | `config_file_or_script` | 可传 `.py` 或 `.sh`（自动识别模式） |
| `$2` | `workload_list.lst` | workload 列表 |
| `$3` | `checkpoint_top_dir` | checkpoint 根目录 |
| `$4` | `task_tag` | 批量任务名（输出目录名） |
| `$5` | `extra_gem5_args` | 仅 `.py` 模式有效，作为附加 gem5 参数串 |

---

## 6. 环境变量建议

### 6.1 单核/流量模型常用

| 变量 | 作用 |
|---|---|
| `GCBV_REF_SO` | difftest 参考模型 so |
| `GCB_RESTORER` | checkpoint restorer |

### 6.2 真实 CPU 多核必备（`--num-cpus>1`）

| 变量 | 作用 |
|---|---|
| `GCBV_MULTI_CORE_REF_SO` | 多核 difftest 参考 so |
| `GCB_MULTI_CORE_RESTORER` | 多核 restorer |

---

## 7. 验收与排错建议

### 7.1 建议检查项

| 检查项 | 命令/位置 | 期望 |
|---|---|---|
| 端点放置日志 | `log.txt` / stdout | 出现 `xsCHI endpoint placement` |
| mesh 连接摘要 | `log.txt` / stdout | 出现 `xsCHI mesh summary` |
| shadow 放置日志 | `log.txt` / stdout | 出现 `xsCHI shadow[i] placement` |
| 输出统计 | `<outdir>/stats.txt` | 正常生成 |

### 7.2 常见错误

| 现象 | 原因 | 处理建议 |
|---|---|---|
| `attach point ... already connected` | 多个设备绑定到同一 local 口 | 调整 `shadow-attach-points` |
| `shadow config length mismatch` | count 与 CSV 列表长度不一致 | 保持四组列表都与 `count` 对齐 |
| `address outside source window` | 请求地址不在 `src/window` | 扩大 `window` 或修正 `src` |
| `shadow dst windows overlap` | 目标窗口重叠 | 重新规划 `dst` 区间 |
| 缺少 multi-core 变量报错 | 使用了 `--num-cpus>1` 但没设环境 | 导出 `GCB_MULTI_CORE_*` 变量 |

---

## 8. 代码锚点（供同事快速对照）

| 主题 | 文件 |
|---|---|
| `--chi-topology` 入口 | `configs/common/xiangshan.py` |
| shadow 参数定义 | `configs/common/Options.py` |
| TopoSys 构造与 2x2/3x3 MeshNode 实例化 | `configs/common/CacheConfig.py` |
| 2x2 变体解析（`dram@M2.local0` / `dram@M1.local1`） | `src/mem/xsCHI/TopoSys/L2todram.cc` |
| `L2L3DramSys_M1Local1Dram` 端点放置 | `src/mem/xsCHI/TopoSys/L2L3DramSysM1Local1Dram.cc` |
| `L2L3DramSys_3x3` 端点放置与 shadow 接入 | `src/mem/xsCHI/TopoSys/L2L3DramSys3x3.cc` |
| shadow remap 公式与严格校验 | `src/mem/xsCHI/device/CHI_L2.hh` / `CHI_L2.cc` |
