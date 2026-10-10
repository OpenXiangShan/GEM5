# PerfCCT 受控微程序与成对验证

这些单线程标量 RV64IM 程序不依赖 libc、GCPT 或 NEMU，使用 Linux exit
检查计算结果。它们验证事件与局部机制，不代表完整系统、RTL 或应用收益。
默认工具链 `riscv64-linux-gnu-gcc`，可用 `CROSS_COMPILE` 覆盖：

```sh
make -C util/perfcct_workload
make -C util/perfcct_workload disasm
```

ELF 为本地构建产物，不提交。需要已构建的 gem5 O3 binary 和支持 ArchDB
的 SE 配置；README 不要求重建模拟器。`qemu-riscv64` 可校验计算结果，
不能校验 gem5 时序或事件覆盖。

## 程序及配对条件

| 程序 | 配对条件 | 观察目标 |
|---|---|---|
| `causal` | STLF、冷 pointer chase、group 候选段 | 基本事件入口与 guest 结果 |
| `pointer-cold` / `pointer-warm` | 同 binary，SimpleMemory 30ns / 90ns | 冷 load 正对照及预热负对照 |
| `stlf-short` / `stlf-long` | store 数据依赖链深度2 / 12 | STLF wait/wake、store 和生产者 |
| `group` | 同 binary，ROB none / kmhv3 | head 与组内失败成员分离 |
| `resource-lines` | 同 binary，MSHR 2/16/64/256 | admission 拒绝及 owner/credit |
| `resource-targets` | 同 binary，target limit2 / 20 | target 占用与实际拒绝 |
| `scheduling-serial` / `scheduling-independent` | 同运算数量，源依赖不同 | IQ 选择、值来源和执行进度 |

pointer 的预热/冷链各64 cache lines，关闭预取；代码预先运行以缩小
无关冷启动影响。STLF 两程序指令数不同，因此不能把 IPC 差直接称为收益。
group 是否覆盖非head失败取决于 dispatch/FTQ/ROB 分组，必须查事件。
MDP 训练可能阻止 load 提前执行；缺少 STLF replay 不自动表示插桩失败。
scheduling 配对均执行128次 DIV 与128次恢复 XOR，先预热共享 kernel；
它用于检查依赖/端口观察，不预设某一 variant 的精确周期。

## 运行与结果

```sh
python3 util/perfcct_workload/run_suite.py --suite paired --verify-untraced
python3 util/perfcct_workload/run_suite.py --suite resources --verify-untraced
python3 util/perfcct_workload/run_suite.py --suite scheduling --verify-untraced
python3 util/perfcct_workload/run_suite.py --case pointer-cold-30
```

`--binary` 指定 gem5，`--out` 指定输出目录；默认分别位于
`m5out/perfcct-paired/suite`、`m5out/perfcct-resource-probes/suite` 和
`m5out/perfcct-scheduling/suite`。可先查看 `--help`。
paired 共8个配置，resources 共6个，scheduling 共2个。

每组保存 `command.json`、`config.ini`、`stats.txt`、`run.log`、`trace.db`、
`analysis.json`；启用 `--verify-untraced` 后比较 tracing OFF/ON 的非 host stats 与原表兼容性。
总目录记录 binary/源码指纹、`summary.json` 与 `comparisons.json`。
`integrity.json` 检查身份和事件自洽；resources/scheduling 还要求对应表或
metadata 与实际事件存在，避免旧 binary 缺插桩却被当成覆盖成功。

```sh
python3 util/perfcct_workload/analyze.py \
  m5out/perfcct-paired/suite/stlf-long/trace.db \
  util/perfcct_workload/stlf-long
python3 util/perfcct_workload/check_trace.py trace.db
python3 util/perfcct_query.py trace.db resources --reject 100
python3 util/perfcct_query.py trace.db resources --request-id 401
```

ROI 使用 marker 提交区间和反汇编身份。`marker_commit_to_commit_cycles`
不同于全运行 stats，也不是 marker 指令的 decode→commit 时长。
全程事件计数与 ROI tick 窗口计数分开；资源 owner 可能更老或来自错误路径，
不能用 ROI 指令 SeqNum 排除所有窗外创建对象。样例数量有上限，遗漏显式记录。

## 核验与边界

```sh
python3 -m unittest discover -s util/perfcct_workload -p 'test_*.py'
python3 -m unittest discover -s util -p 'test_perfcct_query.py'
make -C util/perfcct_workload check-request-identity
```

最后一项是 host C++ 测试，需要已有 `build/RISCV` 生成头文件；检查共享
Request ID 稳定、复制对象和复用地址不会复用身份，不运行 gem5。
guest exit0 只证明计算检查通过；进一步确认事件覆盖、正常退出、统计
一致性、schema、未闭合对象及 trace end。自洽测试不能证明所有事件均已采集。

blocked/full 状态不等于请求真的被拒绝；reject 后 owner 也只是候选持有者。
长 replay/wait 不等于同等长提交停顿，局部时间交叠不能直接称为因果代价。
依赖关系、被选择与推测唤醒不等于值已完成；缺失路径保留 unknown。
开发期配对探针观察到 OFF/ON 非 host stats 一致；性能方向仍须结合实际
事件与匹配工作量验证，不设固定周期或收益门槛。

完整 schema、原始查询与证据边界见
[PerfCCT_causal.md](../../docs/tools/alignToRTL/PerfCCT_causal.md)。
