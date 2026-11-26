# RISC-V Matrix 快速开始

KMHV2 的全系统配置支持单核、单硬件线程的 Matrix 程序。下面的命令用于包含 Matrix ISA、CUTE 后端和 O3 访存实现的完整分支，在 GEM5 仓库根目录执行。

## 编译 GEM5

```bash
scons build/RISCV_MATRIX/gem5.opt --default=RISCV --enable-matrix --linker=gold -j64
```

普通 RISC-V 构建默认关闭 Matrix。增量编译 Matrix 版本时也要带上 `--enable-matrix`。构建依赖见[快速开始](../quick_start.md)。

## 运行镜像

```bash
image=/absolute/path/to/matrix-workload.bin
./build/RISCV_MATRIX/gem5.opt -r -d m5out/matrix \
    configs/example/kmhv2.py --matrix --classic-l2 \
    --raw-cpt --generic-rv-cpt="$image" --warmup-insts-no-switch=0 \
    --mem-type=SimpleMemory --mem-size=512MB --disable-difftest
```

Matrix 的私有 Cache 当前需要接入 classic L2。`--raw-cpt` 用于平坦的 `.bin` 镜像，运行 GCPT 切片时去掉该参数。示例使用 512 MiB SimpleMemory。使用 DRAMsim3 时，先构建 DRAMsim3 并重编 GEM5，再选择 `--mem-type=DRAMsim3` 和镜像所需的内存容量。比较性能时应使用相同的镜像、CPU 和 DDR 配置。

## 用 NEMU 对拍

Matrix 版本的 GEM5 使用 1544 字节寄存器布局的 NEMU REF，即使运行时关闭 Matrix 后端也是如此。共享 CI 服务器可直接使用下面的 REF，其他环境可换成本地编译的共享库。

```bash
export GCBV_REF_SO=/nfs/share/gem5_ci/ref/matrix/af9d4ce51fc2/riscv64-nemu-interpreter-so
./build/RISCV_MATRIX/gem5.opt -r -d m5out/matrix-difftest \
    configs/example/kmhv2.py --matrix --classic-l2 \
    --raw-cpt --generic-rv-cpt="$image" --warmup-insts-no-switch=0 \
    --mem-type=SimpleMemory --mem-size=512MB \
    --enable-difftest --difftest-ref-so="$GCBV_REF_SO"
```

在 `m5out/matrix-difftest/simout` 确认启动时打印了 `NEMU register state: 1544 bytes`，并查看程序自检结果和 `m5_exit instruction encountered`。`MatrixDiff` 汇总中，`submitted`、`completed`、`checked` 应相等，`memorySent` 应等于 `memoryCompleted`，`pendingChecks`、`pendingOwners`、`pendingMemory` 应为零。

### 自行编译 Matrix REF

共享 REF 使用 NEMU `af9d4ce51fc2`。从 GEM5 根目录执行下面的命令，在新目录 `../NEMU-matrix` 中按版本清单检出 NEMU 和四个依赖。构建需要 `jq`、`flex` 和 `bison`。[NEMU 提交](https://github.com/OpenXiangShan/NEMU/commit/af9d4ce51fc2087156b09f26d64a8e1cfb83332f) · [版本清单](../../util/nemu_ref/lock.json)

```bash
set -e
lock="$PWD/util/nemu_ref/lock.json"
git clone --no-checkout "$(jq -r .repository "$lock")" ../NEMU-matrix
cd ../NEMU-matrix
git checkout --detach "$(jq -r .commit "$lock")"
export NEMU_HOME="$PWD"

clone_dep() {
    git clone --no-checkout "https://github.com/$2.git" "$1"
    git -C "$1" checkout --detach "$(jq -r --arg dep "$1" '.dependencies[$dep]' "$lock")"
}
clone_dep resource/softfloat/repo ucb-bar/berkeley-softfloat-3
clone_dep resource/gcpt_restore OpenXiangShan/LibCheckpointAlpha
clone_dep resource/LibCheckpoint OpenXiangShan/LibCheckpoint
clone_dep resource/nanopb nanopb/nanopb

cat > matrix.config <<'EOF'
CONFIG_RV_AME=y
CONFIG_RV_AME_TLEN=65536
CONFIG_RV_AME_TRLEN=512
CONFIG_RV_AME_MLEN=32
CONFIG_RV_AME_MSYNC_32=y
CONFIG_DIFFTEST_AMU_CTRL=y
CONFIG_TVAL_EX_II=y
EOF
cat "configs/$(jq -r .base_defconfig "$lock")" matrix.config > configs/riscv64-gem5-matrix-ref_defconfig
make riscv64-gem5-matrix-ref_defconfig
diff -u <(sort matrix.config) <(grep -Fxf matrix.config .config | sort)
make -j32
export GCBV_REF_SO="$NEMU_HOME/build/riscv64-nemu-interpreter-so"
```

## 访存与同步

O3 在提交前翻译 Matrix 访存地址，提交后按序把命令交给 CUTE。A、B 矩阵和 C 累加器保存在后端寄存器文件中。`mrelease` 完成后更新 token，`macquire` 等待 token 达到指定值。

Matrix 端口经 1 KiB、2 路、16 个 MSHR 的私有 `matrix_cache` 访问共享 L2。标量和 RVV 经 L1 DCache 接入同一条 L2 总线。Matrix 按 64 字节缓存行发请求，部分写使用字节掩码。CPU 和 Matrix 交换数据时仍需相应的内存屏障和 Matrix 同步指令。[端口与发射队列配置](../../configs/common/MatrixConfig.py)

## Tile CSR 地址

| Guest 地址 | 读回值 |
| --- | --- |
| `0xCC1`、`0xCC2`、`0xCC3` | `tlenb`、`trlenb`、`alenb` |
| `0xCC4`、`0xCC5`、`0xCC6` | `mtilem`、`mtilen`、`mtilek` |
| `0xCC7` | `msync`（token 数量） |

用 `msettile*` 更新 M/N/K，通过只读 CSR 读回。旧 AME LLVM 将 `mtok`、`xmtilem`、`xmtilen`、`xmtilek` 依次编码为 `0xCC4–0xCC7`，将 `xmisa` 编码为 `0xCC0`。XSAI 访问 `0xCC0` 会触发非法指令。使用旧名字的程序需更新汇编器地址表，或按表中的数字地址重新编译。
