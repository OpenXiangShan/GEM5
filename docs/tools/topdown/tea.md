# TEA 使用说明

TEA（Timing Event Attribution）用于香山 O3 CPU 的性能归因：按周期采样流水线
状态，把样本权重分配给相关指令的 PC 和事件签名 PSV（Performance Signature
Vector）。它与已有的 Topdown 统计配合使用，默认关闭。

## 开启与关闭

先按仓库 [README](../../../README.md) 编译 `build/RISCV/gem5.opt`，准备可运行的
镜像或 checkpoint。下面从仓库根目录运行一个普通 bin 镜像：

```sh
./build/RISCV/gem5.opt --outdir=m5out/tea \
    ./configs/example/kmhv3.py \
    --raw-cpt --generic-rv-cpt=/path/to/image.bin \
    --enable-tea --tea-sample-period=1
```

运行 GCPT 切片时使用 checkpoint 路径，省略 `--raw-cpt`：

```sh
./build/RISCV/gem5.opt --outdir=m5out/tea \
    ./configs/example/kmhv3.py \
    --generic-rv-cpt=/path/to/checkpoint.zstd \
    --enable-tea --tea-sample-period=16
```

上述命令沿用正常运行的 difftest 配置，例如已设置的 `GCBV_REF_SO`；TEA 无需
额外的 REF 或 restorer。普通单核 GCPT 切片使用自带的 restorer 即可。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--enable-tea` | 关闭 | 创建 TEA 统计器并开启归因 |
| `--tea-sample-period=N` | `1` | 每 N 个 CPU 周期采样一次；`0` 与 `1` 都是逐周期采样 |

关闭 TEA 时省略 `--enable-tea`。只设置采样周期不会开启 TEA，把周期设为 `0`
也不会关闭 TEA。关闭时不创建统计器，不执行归因回调和事件标记，不生成 TEA
统计组或 CSV；正常路径仍有开关判断和少量字段初始化。

需要降低采样成本时可使用 `--enable-tea --tea-sample-period=16`。增大周期会减少
采样和目标查询，但已开启的事件跟踪仍会执行；固定周期采样也可能遗漏短事件。
比较配置时应保持采样周期和测量窗口一致。

自定义 Python 配置可直接设置 O3 CPU 的 `enableTea`、`teaSamplePeriod` 和
`teaLastLevelCache`。最后一项用于判定 ST_LLC，`0` 表示不记录该事件；
`kmhv3.py` 的常规缓存配置会根据是否启用 L3 将其设置为 `3` 或 `2`。

## 输出文件

TEA 在 gem5 dump stats 时生成两类输出。以默认单核对象名 `system.cpu` 为例：

- `m5out/tea/stats.txt` 中的 `system.cpu.tea.*`：状态、事件及归因质量统计。
- `m5out/tea/system.cpu.tea.csv`：按线程、PC 和 PSV 聚合的样本权重。

实际文件名以前缀 CPU 对象名为准，多 CPU 各自输出。CSV 表头为：

```text
thread,pc,psv,sample_weight
```

| 列 | 含义 |
| --- | --- |
| `thread` | CPU 内的硬件线程编号 |
| `pc` | 指令地址，十六进制；没有已知目标地址时为空 |
| `psv` | 事件位掩码，十进制；负值表示特殊归因状态 |
| `sample_weight` | 样本权重，可为小数；同周期多个架构提交之间会均分权重 |

CSV 在每次 dump 时覆盖为当前统计窗口的快照，stats reset 会清空累计权重和
待归因样本。若运行包含 warmup/reset，分析结束时的 CSV 应配对 `stats.txt` 的
最后一个统计区段，避免与预热统计相加。CSV 没有单独的逐 PC 流水线状态列。

## 状态与事件

`samplesByState::*` 中四类状态互斥，按 Computing、Flushed、Drained、Stalled
的优先级判定，每个活跃线程在采样点记一个样本：

| 状态 | 含义与归因方式 |
| --- | --- |
| Computing | 本周期有计入 TEA 的架构提交，在这些提交间均分；不表示提交带宽已用满 |
| Flushed | 无上述提交，正在恢复、ROB walk，或处于带有效恢复原因的空 ROB 阶段；归给恢复原因 |
| Drained | 无上述提交和恢复且 ROB 为空，样本等待归给后续首个架构提交 |
| Stalled | 其余 ROB 非空且无提交的周期，选择提交阻塞目标，等待其退休或被 squash |

PSV 的事件位定义如下。多个事件可同时出现在同一指令上：

| 位 | 掩码 | 事件 | 含义 |
| --- | ---: | --- | --- |
| 0 | 1 | DR_L1 | 取指响应记录了 L1 之后的缓存访问 |
| 1 | 2 | DR_TLB | 取指地址翻译发生延迟 |
| 2 | 4 | DR_SQ | 无更老 ROB 工作时，store/atomic 因 SQ 压力无法继续 |
| 3 | 8 | FL_MB | 分支预测错误引起恢复 |
| 4 | 16 | FL_EX | 指令异常引起恢复 |
| 5 | 32 | FL_MO | 内存顺序冲突或提交时 MDP 违例引起恢复 |
| 6 | 64 | ST_L1 | 访存路径记录的 L1 数据访问未命中 |
| 7 | 128 | ST_TLB | 数据地址翻译发生延迟 |
| 8 | 256 | ST_LLC | cacheable load 响应达到配置的末级缓存未命中深度 |

例如 `psv=320` 是 `ST_L1|ST_LLC`，`psv=448` 是
`ST_L1|ST_TLB|ST_LLC`。`psv=0` 表示这些事件位均未设置，仍属于成功归因，
不能据此认为没有瓶颈；普通标量执行等待等情况没有独立事件位。

| 特殊 PSV | 含义 | 对应统计 |
| --- | --- | --- |
| `-1` | Stalled 的目标被 squash，样本取消 | `cancelledTargets` |
| `-2` | 目标尚未退休，或 Drained 样本尚未等到后继提交 | `pendingSamples` |
| `-3` | 没有可选择的目标或已知恢复原因 | `unresolvedTargets` |

## 读取与分析

先查看统计组：

```sh
rg 'system.cpu.tea\.' m5out/tea/stats.txt
```

在同一统计窗口内，检查以下守恒关系；归因权重应允许浮点舍入误差：

```text
samples = sum(samplesByState::*)
samples = attributedSamples + cancelledTargets + unresolvedTargets + pendingSamples
samples = sum(CSV sample_weight)
```

状态比例使用 `samplesByState::<state> / samples`，事件比例使用
`samplesByEvent::<event> / samples`。事件统计是带对应 PSV 位的成功归因样本
权重，多个事件的比例不能相加；状态和事件也不是一一对应，例如 Computing
样本可以归给带 FL_MB 的指令。`committedByThread::*` 统计观察到的架构提交数，
不受采样周期稀疏化，不应用作这些比例的分母。

可用下面的命令汇总成功归因的前 10 个 PC。分母包含取消、未决和未知样本，
分子只计成功归因，按（线程，PC）分别累计：

```sh
python3 - m5out/tea/system.cpu.tea.csv <<'PY'
import csv
import sys
from collections import defaultdict

with open(sys.argv[1]) as f:
    rows = list(csv.DictReader(f))
total = sum(float(row['sample_weight']) for row in rows)
pcs = defaultdict(float)
for row in rows:
    if int(row['psv']) >= 0 and row['pc']:
        pcs[(row['thread'], row['pc'])] += float(row['sample_weight'])
for (thread, pc), weight in sorted(pcs.items(), key=lambda x: -x[1])[:10]:
    print(f'thread={thread} pc={pc} weight={weight:.3f} share={weight / total:.2%}')
PY
```

PC 权重包含所有状态的成功归因样本，不能全部当作停顿周期或可消除的性能损失。
采样周期 N 大于 1 时，权重仍以样本为单位；单线程估算周期量可乘以 N，比例无需
缩放。SMT 下每个活跃线程各自采样，总样本数不是物理 CPU 周期数。

跨 SimPoint 汇总时，应先用测量窗口指令数归一化，再乘 SimPoint 权重及对应
workload 的 profile 指令总量，最后计算比例；不要直接平均切片百分比。
保留相同窗口的 `stats.txt`、CSV、`config.json` 和切片权重以便复核。

## 当前限制

当前实现仍有已知归因边界：中间 RVV 微操作退休周期未计入 Computing，部分
split 访存可能漏记 ST_L1，部分退休的 RVV 指令异常后 macroPSV 可能带入异常
处理路径。涉及这些场景时，应结合已有 Topdown 计数器及指令 trace 交叉分析。
