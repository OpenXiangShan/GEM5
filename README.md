# About

This is the gem5 simulator for Xiangshan (XS-GEM5), which
currently scores similar with Nanhu on SPEC CPU 2006.
XS-GEM5 is enhanced with

- Xiangshan RVGCpt: a cross-platform full-system checkpoint for RISC-V.
- Xiangshan online Difftest: an API to check execution results online.
- Frontend microarchitecture calibrated with Xiangshan V2 (Nanhu): Decoupled frontend, TAGESC, and ITTAGE,
which performance better than LTAGE and TAGE-SCL shipped in official version on SPECCPU.
- Instruction latency calibrated with Nanhu
- Cache hierarchy, latency, and prefetchers calibrated with Nanhu.
- A fixed Multi-Prefetcher framework with VA-PA translation support
- A fixed BOP prefetcher
- Parallel RV PTW (Page Table Walker) and walking state coalescing
- Cascaded FMA
- Move elimination
- L2 TLB and TLB prefetching (coming soon).
- Other functional or performance bug fixes.

# A Short Doc

### GCC & libboost

- Use GCC > 9.4.0.
- Install libboost and valgrind

## Clone and build DRAMSim3

Refer to [The readme for DRAMSim3](ext/dramsim3/README) to install DRAMSim3.

Notes:
- If you have already built GEM5, you should rebuild gem5 after install DRAMSim3
- If simulating Xiangshan system, use DRAMSim3 with our costumized config

Usage:

`$gem5_home/build/gem5.opt ...fs.py ... --mem-type=DRAMsim3 --dramsim3-ini=$gem5_home/xiangshan_DDR4_8Gb_x8_2400.ini ...`

## Build GEM5

```shell
cd gem5
scons build/RISCV/gem5.opt --gold-linker
export gem5_home=`pwd`
```

## Run Gem5

See [The example running script](util/warmup_scripts/simple_gem5.sh).

This script runs GEM5 with single thread (`function single_run`) or multiple threads (`function parallel_run`).
Both `single_run` and `parallel_run` calls `function run`.
`function run` provides the default parameters for XS-GEM5.

For debugging or performance tuning, we usually call `single_run` and modify parameters for `function run`.
`run` takes 5 parameters:
- `debug_gz`: the path to the debug binary (usually checkpoint) of the program to run.
- `warmup_inst`: the number of instructions to warmup the cache, usually 20M.
- `max_inst`: the number of instructions to run, usually 40M. The first half is used for warmup, and the second half is used for statistics collection.
- `work_dir`: the directory to store the output files.
- the last parameter: whether enable Arch DB. Arch DB is a database to store the micro-architectural trace of the program. It is used for debugging and performance tuning.

More details can be found in comments and code of [the example running script](util/warmup_scripts/simple_gem5.sh).

## Play with Arch DB

Arch DB is a database to store the micro-architectural trace of the program with SQLite.
You can access it with Python or other languages.
A Python example is given [here](util/arch_db/mem_trace.py).

## Produce RVGCpt checkpoints with NEMU

Please refer to [the checkpoint tutorial for Xiangshan](https://xiangshan-doc.readthedocs.io/zh_CN/latest/tools/simpoint/)
and [Build Linux kernel for Xiangshan](https://github.com/OpenXiangShan/XiangShan-doc/blob/main/tutorial/others/Linux%20Kernel%20%E7%9A%84%E6%9E%84%E5%BB%BA.md)

The process of SimPoint checkpointing includes ***3 individual steps***
1. SimPoint Profiling to get BBVs. (To save space, they often output in compressed formats such as **bbv.gz**.)
1. SimPoint clustering. You can also opt to Python and sk-learn to do k-means clustering. (In this step, what is typically obtained are the **positions** selected by SimPoint and their **weights**.)
1. Taking checkpoints according to clustering results. (In the RVGCpt process, this step generates the **checkpoints** that will be used for simulation.)

If you have problem generating SPECCPU checkpoints, following links might help you.
- [The video to build SPECCPU, put it in Linux, and run it in NEMU to get SimPoint BBVs](https://drive.google.com/file/d/1msr_YijlYN4rxpn71bod1LAoRWs5VtAL/view?usp=sharing) (step 1)
- [The document to do SimPoint clustering based on BBVs and take simpoint checkpoints](https://zhuanlan.zhihu.com/p/604396330) (step 2 & 3)

## Difftest with NEMU

The Difftest framework used in XS-GEM5 is similar to the one used in Xiangshan.
Please use the [gem5-ref-main branch of NEMU](https://github.com/OpenXiangShan/NEMU/tree/gem5-ref-main) for difftest with XS-GEM5.

``` shell
git clone https://github.com/OpenXiangShan/NEMU.git -b gem5-ref-main
cd NEMU
export NEMU_HOME=`pwd`
make riscv64-nohype-ref_defconfig
make menuconfig  # then save configs
make -j 10
```

Then the contents of `build` directory should be 
```
build
|-- obj-riscv64-nemu-interpreter-so
|   `-- src
`-- riscv64-nemu-interpreter-so
```

then use `riscv64-nemu-interpreter-so` as reference for GEM5,
``` shell
export ref_so=`realpath build/riscv64-nemu-interpreter-so`

# This is not full command, but a piece of example.
$gem5_home/build/gem5.opt ... --enable-difftest --difftest-ref-so $ref_so ...
```

# Original README

The README for official GEM5 is here: [Original README](./official-README.md)
