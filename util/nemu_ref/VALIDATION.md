# NEMU 统一版本验证记录

日期：2026-09-10。当前 GEM5 分支：`codex/spec17-gcc16-profile`。
主线 `89e4b9e06f` 已通过 merge commit `c7b4907015` 合并；
下面测试包含本 PR 的 FCSR、memdedup 和向量异常标志修复，不能把 merge SHA
单独当作被测源码。测试构建以以下二进制 SHA256 标识。

## 最终构建身份

NEMU 源码：`d30fff1ece9e0480146caf504660a0239b994eef`，无源码补丁。
依赖、公共配置、普通扩展增量和变体配置均由本目录锁定。

| 变体 | SHA256 |
| --- | --- |
| normal | `e84c59788ac609bc1a5fd91217a516155a594ed1a19c9fcdda07a05616832615` |
| normal-dedup | `e5e58a35ae758b46de9c17cf8d30ef92e0a168d197939926a7185ee94a9315f3` |
| h | `53467b58eab8d63dae6161c168101b305bf13365e9b1e3cc28f21fbb384b7bcb` |
| multi | `bef59826d1be2b8d4c37f27ca78b7475670d03b9140eb3dad2c0c4c85a591760` |

最终 RISCV GEM5 二进制 SHA256：
`b3567330b0a424760b2fd083040784a4fb9271639de57b870d1b0ef08e3facff`。
冻结副本：`/tmp/nemu-unified-fixed-gem5.opt`。
四个 REF 的导出寄存器长度均为 1376 字节。
Ruby/CHI GEM5 二进制 SHA256：
`54aa6630c548e4333326b411d7e21cc5767cc108c9f0794ab5d8bc5eef31ce2a`。

## 已完成的定点验证

最终二进制和最终 REF 的定点结果见 `/tmp/nemu-unified-fixed-tests/*/run.json`，
每条记录含命令、REF SHA、GEM5 SHA、返回码、时长、内存采样和模拟统计。

- SPEC06 GCC12 mcf/10688，普通/memdedup 各 1M 指令，通过。
- SPEC06 GCC16 GemsFDTD/10734，普通/memdedup 各 1M 指令，通过。
- SPEC17 GCC16 gcc AMOCAS.D 点 670，普通/memdedup 各 1M 指令，通过。
- SPEC26 stockfish/145943，普通/memdedup 各 1M 指令，通过。
- H wrf/6348、astar_biglakes/563，各 1M 指令，通过。
- H GemsFDTD/14687，采用性能 CI 的外部 GCPT restorer，各 1M 指令，通过。
- H `gcbh_test.zstd` 外部 restorer 路径，100k 指令，通过。
- SMT CoreMark：两线程合计提交 1,996,703 指令，通过。
- SMT mcf/10688 checkpoint：两线程分别提交 991,931 和 1,000,001 指令，通过。
- Ruby/CHI 真双核 `multi_core_test.gz`：两核分别提交 995,268 和 1,000,000
  指令，通过；使用 CI 的 2-core restorer 和 DDR4 配置。
- `vfwredosum`、`vfwredusum` 在 normal 和 normal-dedup 下完整结束，通过。
- 修复版完整 vector 小测试集：878/878 通过，配置为 CI 使用的 `kmhv2.py`；
  逐例命令与日志见 `/tmp/nemu-unified-vector-fixed/`。
- 六项边界检查通过：旧 1368 字节布局拒绝、4096 字节伪布局拒绝、注入错误
  FCSR 拒绝、两个方向的 memdedup 错配拒绝、正确配对正常结束。
- 7 项 REF manifest/路径单测、现有 solver 单测通过。
- 实际执行性能 workflow 的配置步骤，8 种普通、RVV、H、SMT、自定义 REF 路由通过；
  所有 workflow YAML 可解析。

六项边界测试记录：`/tmp/nemu-unified-fixed-negative/`。
路由测试记录：`/tmp/nemu-workflow-tests/`。

## 内存效果

以下是最终构建 1M 指令 A/B 的峰值估算，占用包含 PSS 以及未计入映射 PSS 的
memfd 已分配页面，避免只看 RSS 而漏掉 COW 后仍保留的 backing pages。
这是采样估算，不是隔离 cgroup 的精确峰值；各测试进程采样到的 Swap 为 0。

| 切片 | normal GiB | memdedup GiB | 降低 |
| --- | ---: | ---: | ---: |
| SPEC06 GCC12 mcf | 3.679 | 1.972 | 46.39% |
| SPEC06 GCC16 GemsFDTD | 1.469 | 0.900 | 38.73% |
| SPEC17 gcc AMOCAS.D | 2.044 | 1.144 | 44.05% |
| SPEC26 stockfish | 4.039 | 2.143 | 46.94% |

上述四组 `simInsts`、`simTicks`、`numCycles`、IPC 完全一致。
小型 raw CoreMark 的节省很小；不能将大 checkpoint 的收益推广到所有运行方式。

补充的 10M 指令实验也通过：mcf 从 3.758 降至 2.061 GiB（45.16%），
stockfish 从 4.302 降至 2.405 GiB（44.08%），四项模拟统计仍完全一致。
该组固定使用 `/tmp/nemu-unified-final-gem5.opt`（SHA `e9b842857f58...`）
和 `r3d` 中间产物，记录在 `/tmp/nemu-unified-final-tests/`。
它包含正式 FCSR 比较，但在 widening reduction 单行修复之前；normal 两个变体的
实际 `.config` 与最终构建相同。不要将该组冒充最终二进制的 10M 回归。

## 回归发现与修复

1. 首轮完整 vector 为 876/878。两个 widening reduction 的结果寄存器一致，
   但 REF 的 FCSR.NX=1、GEM5=0。对应 format 缺少已有的 `fflags_wrapper`，
   已补上并在两个内存变体下复现修复；没有关闭 FCSR 比较。
2. 最初把普通单核扩展增量也应用到 H，wrf 在 `stimecmp` 写入处分叉：
   GEM5 触发非法指令，NEMU 因 SSTC 开启而接受。公共 ABI 配置与普通扩展增量
   已分离，H 不继承 SSTC 等增量，wrf 复测通过。
3. H 功能 autotest 的旧 checkpoint 根目录在本机不存在。已改为共享 CI
   `spec06_cpts/h_spec06` 下相同 workload/point，不改测试指令或 restorer。

## 发布与验证边界

- 四个最终产物已新增到
  `/nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3/`。
  本机 node037 和远端 node020 均通过四份 manifest/SHA 校验及动态库加载检查。
- 所有本轮启动的测试和构建已结束。RISCV、RISCV_CHI 构建、增量 style 和
  `git diff --check` 通过。
- SPEC/H/SMT 是定点短测，不是完整性能覆盖率 CI，也没有生成新的 SPEC 总分。
- raw image 大于等于模拟 RAM 的 memdedup 路径仍未支持，不做全局默认开启。
- 本机构建依赖 glibc 2.38 或更高，与现有 scalar REF 的最高 GLIBC 符号版本一致；
  node020 使用 glibc 2.39。两台节点的成功不代表已经检查所有 runner。
- 共享旧 REF 未覆盖。线上默认 workflow 的切换以本 PR 合入为准；
  合入前仍建议运行完整 SPEC/H/SMT CI，并保留历史产物用于旧 GEM5 复现。
- 本文的 `/tmp` 路径是本次本地验证的日志索引，不是持久 CI 归档；
  后续复现应使用锁定配置重新构建，以新运行的日志和构建身份为准。
