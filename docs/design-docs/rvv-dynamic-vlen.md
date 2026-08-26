# Configurable / Dynamic VLEN in XS-GEM5

文档站点相关：本说明记录路线图 [#181](https://github.com/OpenXiangShan/GEM5/issues/181)
中 “Dynamic Vector Length Support” 的 XS-GEM5 落地方式。

## 目标

同一份 gem5 二进制可通过配置选择架构 VLEN（128 / 256 / 512），
而不再把语义宽度写死为 Kunminghu 的 128。

## 设计取舍

| 层 | 选择 | 原因 |
|----|------|------|
| 物理寄存器容器 | 固定 `MaxVecLenInBits = 512` | 避免真正动态分配穿透 O3 PRF / operand ABI |
| 架构 VLEN | `RiscvISA.vlen` 运行时参数（默认 128） | 对齐上游 gem5，且保持 Kunminghu 默认行为 |
| 指令语义 | StaticInst 在 decode 时捕获 `vlen` | 不必改遍 ISA parser 的每个 `new` 调用点 |
| Difftest | NEMU ABI 仍固定 128 | 现有 `riscv64-nemu-interpreter-so` 布局依赖 128；非 128 时禁止 `--enable-difftest` |

## 使用方法

```bash
# 默认 Kunminghu：VLEN=128
./build/RISCV/gem5.opt configs/example/kmhv3.py ...

# 探索更大 VLEN（不要开 stock NEMU difftest）
./build/RISCV/gem5.opt configs/example/kmhv3.py --rvv-vlen=256 ...
./build/RISCV/gem5.opt configs/example/kmhv3.py --rvv-vlen=512 --rvv-elen=64 ...
```

`vlenb` CSR 返回 `vlen/8`。`vsetvl*` 的 VLMAX 通过 `getVlmax(vtype, vlen)` 计算。

## 关键代码锚点

- `src/arch/riscv/types.hh`：`MaxVecLenInBits` / `DefaultVecLenInBits`
- `src/arch/riscv/RiscvISA.py`：`vlen` / `elen` 参数与合法性检查
- `src/arch/riscv/isa.{hh,cc}`：CSR `vlenb`、序列化
- `src/arch/riscv/vec_len.{hh,cc}`：decode 期 `thread_local` 发布 / ExecContext 查询
- `src/arch/riscv/decoder.{hh,cc}`：从 ISA 读取 VLEN 并在 decode 前发布
- `src/arch/riscv/insts/vector.hh`：`Vector*Inst::vlen` 成员
- `configs/common/Options.py`：`--rvv-vlen` / `--rvv-elen`
- `configs/common/xiangshan.py`：`_configure_riscv_vector_isa()`

## 测试

```bash
# 1) VLMAX 公式 / 配置契约（无需链接 gem5）
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen.py
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen_config.py
python3 configs/example/xiangshan_rvv_vlen_smoke.py --standalone --rvv-vlen=256

# 2) 上游 RVV intrinsic 矩阵的 XS 适配清单（VLEN 限制为 128/256/512）
python3 util/xs_scripts/rvv_vlen/test_matrix.py

# 3) C++ GTest（按 gem5 常规方式构建 unit tests）
#    src/arch/riscv/vlen.test.cc 已注册为 GTest('vlen.test')
```

上游对应来源：

- `configs/example/gem5_library/riscv-rvv-example.py`
- `tests/gem5/se_mode/rvv_intrinsic_tests/test.py`

上游在 SE 模式下用 `obtain_resource(rvv-*)` 扫描 VLEN=128..16384。
XS-GEM5 无 SE，因此移植了资源列表与断言正则，并把 VLEN 收敛到容器上限 512；
真正跑二进制需要先把 `rvv-*` 做成香山 FS/AM workload。

## 构建说明

推荐使用 **xs-env / Ubuntu 22.04 + GCC ≥ 11**（与 CI / 现有 `gem5.opt` 一致）：

```bash
scons build/RISCV/gem5.opt --linker=gold -j$(nproc)
```

## 测试状态（近期验证）

| 套件 | 结果 |
|------|------|
| Python VLMAX / 配置契约 / 矩阵元数据 | PASS |
| `xiangshan_rvv_vlen_smoke`（RiscvISA 128/256/512） | PASS |
| GTest `vlen.test.opt` | PASS（container / VLMAX / `elem_gen_idx` / mask 公式） |
| kmhv3 短仿真 x VLEN∈{128,256,512}（microbench cpt） | PASS，`config.ini` 中 `vlen=` 正确 |
| 上游 SE `rvv-*` x 3 VLEN（36 cases，参考二进制） | PASS |
| AM `rvv-vlen-check` x VLEN∈{128,256,512} | PASS：CSR + `vlseg/vsseg/vlse/vluxei/vle_m2/vl1re/vslide/vmseq_m2` |
| Python 负向证明 `test_elem_gen_idx_negative.py` | PASS（缺 arch vlen 时 256/512 必挂） |
| Difftest 守卫 | PASS：`--rvv-vlen=256 --enable-difftest` fatal |
| XS FS 上直接跑上游 Linux `rvv-*` ELF | 不做（需 AM/FS 封装；SE ELF ≠ raw-cpt） |

详见 `util/xs_scripts/rvv_vlen/README.md`、`run_all_tests.sh`、`run_am_vlen_check.sh`。

## 后续工作

1. 为 VLEN=256/512 准备匹配的 NEMU / checkpoint / SPEC-RVV 参考。
2. 将上游 `rvv-*` 资源编入 workload-builder / AM，接入 `test_matrix.py --bin-dir`。
3. 将更多仍依赖容器宽度假设的调试打印改为按 active VLEN 截断。
4. 若需单进程混合不同 VLEN，必须取消共享 decode cache 或把 VLEN 编入 cache key。
