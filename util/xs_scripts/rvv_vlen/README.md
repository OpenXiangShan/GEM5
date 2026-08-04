# XS-GEM5 RVV VLEN tests (ported from upstream gem5)

Upstream sources:

- `gem5/configs/example/gem5_library/riscv-rvv-example.py`
- `gem5/tests/gem5/se_mode/rvv_intrinsic_tests/test.py`
- `gem5/util/cpt_upgraders/riscv-dyn-vlen.py`

## What was ported

| Upstream | XS adaptation |
|----------|---------------|
| SE `riscv-rvv-example.py` with `--vlen/--elen` | `configs/example/xiangshan_rvv_vlen_smoke.py` (+ `--rvv-vlen` on kmhv3) |
| SE matrix VLEN=128..16384 × 12 `rvv-*` resources | `util/xs_scripts/rvv_vlen/test_matrix.py` (VLEN ∈ {128,256,512}) |
| VLMAX semantics | `util/xs_scripts/rvv_vlen/test_rvv_vlen.py` + `src/arch/riscv/vlen.test.cc` |
| Checkpoint upgrader (8192 B regs) | `util/cpt_upgraders/riscv-dyn-vlen-xs.py` (64 B / MaxVLEN=512) |

# Run (no rebuild required for Python; gem5/GTest need build/RISCV/*)

```bash
# Inside xs-env / xs-gem5-build (GCC >= 11):
./util/xs_scripts/rvv_vlen/run_all_tests.sh

# Or step-by-step:
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen.py
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen_config.py
python3 configs/example/xiangshan_rvv_vlen_smoke.py --standalone --rvv-vlen=256
python3 util/xs_scripts/rvv_vlen/test_matrix.py \
  --bin-dir util/xs_scripts/rvv_vlen/bins
./build/RISCV/gem5.opt configs/example/xiangshan_rvv_vlen_smoke.py --rvv-vlen=256
scons -j$(nproc) --ignore-style build/RISCV/arch/riscv/vlen.test.opt
./build/RISCV/arch/riscv/vlen.test.opt
```

### Download upstream `rvv-*` binaries

```bash
# Needs an upstream stdlib gem5 (e.g. /ssdhome/.../gem5/build/RISCV/gem5.opt)
GEM5_RESOURCE_DIR=util/xs_scripts/rvv_vlen/gem5-resource-cache \
  /path/to/upstream/gem5.opt -d /tmp/m5out-dl \
  util/xs_scripts/rvv_vlen/download_upstream_bins.py \
  --out util/xs_scripts/rvv_vlen/bins
```

### XS AM functional check（推荐）

上游 `rvv-*` 是 Linux SE ELF，**不能**直接给 `--raw-cpt`。用 AM 自检程序验证
真实 `vlenb` / `vsetvli` VLMAX：

```bash
export AM_HOME=/path/to/nexus-am
# Ubuntu 交叉工具链：
make -C util/xs_scripts/rvv_vlen/am/rvv-vlen-check \
  ARCH=riscv64-xs LINUX_GNU_TOOLCHAIN=1

./util/xs_scripts/rvv_vlen/run_am_vlen_check.sh
# Expect for each VLEN:
#   rvv-vlen-check: vlenb=VLEN/8 ... PASS
```

### Upstream SE reference matrix (12 × {128,256,512} = 36)

XS-GEM5 has no SE; run the same binaries on upstream gem5 to validate the
resource binaries and VLEN pass strings:

```bash
# Example (container with upstream gem5 mounted):
for r in rvv-branch rvv-index rvv-matmul rvv-memcpy rvv-reduce rvv-saxpy \
         rvv-sgemm rvv-strcmp rvv-strcpy rvv-strlen rvv-strlen-fault rvv-strncpy; do
  for v in 128 256 512; do
    ./build/RISCV/gem5.opt configs/example/gem5_library/riscv-rvv-example.py \
      "$r" --vlen=$v --elen=64
  done
done
```

Last full run: **36/36 PASS**.

### XS kmhv3 wiring smoke

```bash
./build/RISCV/gem5.opt -d m5out-vlen-$VLEN configs/example/kmhv3.py \
  --generic-rv-cpt=$CPT --maxinsts=50000 \
  --rvv-vlen=$VLEN --disable-difftest
# Expect config.ini: vlen=$VLEN
```

## Remaining gap for full XS FS parity

Upstream SE ELFs are now downloaded under `util/xs_scripts/rvv_vlen/bins/`, but
XS-GEM5 cannot execute them directly. To finish the matrix on XS:

1. Wrap each `rvv-*` as XiangShan AM or Linux FS workloads / checkpoints.
2. Run with `./build/RISCV/gem5.opt configs/example/kmhv3.py --rvv-vlen=$VLEN ...`
3. Match stdout against the upstream pass regex: `^.*<resource>-x.y.z: pass$`.

Stock NEMU difftest remains valid only for `--rvv-vlen=128`.
