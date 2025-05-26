# About

This is the gem5 simulator for Xiangshan (XS-GEM5), which currently scores similar with Kunminghu on SPEC CPU 2006.

## Features

XS-GEM5 is not as easy to use as official GEM5, because it only supports full-system simulation
with Xiangshan's specific formats, refer to [Workflows](#workflows-how-to-run-workloads) for more details.

XS-GEM5 is enhanced with
- Xiangshan RVGCpt: a cross-platform full-system checkpoint for RISC-V.
- Xiangshan online Difftest: an API to check execution results online.
- Topdown performance counters
- Frontend microarchitecture calibrated with Xiangshan V3 (Kunminghu)
  * Decoupled frontend
  * TAGESC, ITTAGE, and optinal Loop predictor (performs better than LTAGE and TAGE-SCL shipped in official version on SPECCPU)
  * Instruction latency calibrated with Kunminghu
- Backend microarchitecture calibrated with Xiangshan V3 (Kunminghu)
  * Distributed scheduler
  * Scheduling/execution latency calibrated with Kunminghu
  * RVV mostly calibrated
- Cache hierarchy, latency, and prefetchers calibrated with Kunminghu.
  * Algorithm: Stream + Berti/Stride + BOP + SMS + Temporal + CDP
  * Framework: Active/Passive offloading; Multi-Prefetcher coordination
  * VA-PA translation support for all prefetchers
- Parallel RV PTW (Page Table Walker)
  * Walking state coalescing
  * PTW and TLBs for RV-H
- Cascaded FMA
- Move elimination
- L2 TLB and TLB prefetching.
- CSR fixes
- Other functional or performance bug fixes.

## Branches

Because XS-GEM5 is currently under internal development, we have several branches for different purposes:
- xs-dev branch is periodically synced with our internal development branch.
- backport branch is used to backport patches that affects functional correctness and basic usage.

## What is NOT supported

- Cannot run Boom's baremetal app
  * We only support [Abstract Machine](https://github.com/OpenXiangShan/nexus-am) baremetal environment or Linux for Xiangshan.
- Cannot directly run an ELF
  * GEM5's System call emulation is not supported.([What is system call emulation](https://stackoverflow.com/questions/48986597/when-to-use-full-system-fs-vs-syscall-emulation-se-with-userland-programs-in-gem))
  * QEMU's User space emulation is not supported.([What is user space emulation](https://www.qemu.org/docs/master/user/main.html))
- Checkpoint is not compatible with GEM5's SE checkpoints or m5 checkpoints.
  * Cannot produce GEM5's SE checkpoints or m5 checkpoints
  * Cannot run GEM5's SE checkpoints or m5 checkpoints
- Recommend NOT to produce a checkpoint in M-mode

## Please DO NOT

- Please don't make a new issue without reading the doc
- Please don't make a new issue without searching in issue list
- Please don't running boom's baremetal app with XS-GEM5
- Please don't running SimPoint bbv.gz with NEMU, XS-GEM5, or Xiangshan processor, because it is not bootable
- Please don't make a new issue about building Linux in NEMU's issue list,
plz head to [Xiangshan doc](https://github.com/OpenXiangShan/XiangShan-doc/issues?q=is%3Aissue)

## Maintainers will BLOCK you from this repo if

- Try to run boom's baremetal app with XS-GEM5, and make a related issue
- Try to run SimPoint bbv.gz with XS-GEM5, and make a related issue

# A Short Doc

## Workflows: How to run workloads

### Run without checkpoint

The typical flow for running workloads is similar for [NEMU](https://github.com/OpenXiangShan/NEMU/),
[XS-GEM5](https://github.com/OpenXiangShan/GEM5),
and [Xiangshan processor](https://github.com/OpenXiangShan/XiangShan).
All of them only support full-system simulation.
To prepare workloads for full-system simulation, users need to either build a baremetal app or
running user programs in an operating system.

```mermaid
graph TD;
am["Build a baremetal app with AM"]
linux["Build a Linux image containing user app"]
baremetal[/"Image of baremetal app or OS"/]
run["Run image with NEMU, XS-GEM5, or Xiangshan processor"]

am-->baremetal
linux-->baremetal
baremetal-->run
```

### Run in with checkpoints

Because most of the enterprise users and researchers are more interested in running larger workloads,
like SPECCPU, on XS-GEM5.
To reduce the simulation time of detailed simulation, NEMU serves as a checkpoint producer.
The flow for producing and running checkpoints is as follows.

```mermaid
graph TD;
linux["Build a Linux image containing NEMU trap app and user app"]
bin[/"Image containing Linux and app"/]
profiling["Boot image with NEMU with SimPoint profiling"]
bbv[/"SimPoint BBV, a .gz file"/]
cluster["Cluster BBV with SimPoint"]
points[/"SimPoint sampled points and weights"/]
take_cpt["Boot image with NEMU to produce checkpoints"]
checkpoints[/"Checkpoints, several .gz files of memory image"/]
run["Run checkpoints with XS-GEM5"]

linux-->bin
bin-->profiling
profiling-->bbv
bbv-->cluster
cluster-->points
points-->take_cpt
take_cpt-->checkpoints
checkpoints-->run

```

### How to prepare workloads

As described above, XS-GEM5 either takes a baremetal app or a checkpoint as input.

To build baremetal app compatible with XS-GEM5,
we use [Abstract Machine](https://github.com/OpenXiangShan/nexus-am) as a light-weight baremetal library.
Common simple apps like coremark and dhrystone can be built with Abstract Machine.

To obtain checkpoints of large applications,
please follow [the doc to build Linux](https://xiangshan-doc.readthedocs.io/zh-cn/latest/tools/linux-kernel-for-xs/)
to pack a image,
and follow [the checkpoint tutorial for Xiangshan](https://xiangshan-doc.readthedocs.io/zh_CN/latest/tools/simpoint/)
to produce checkpoints.

The process to produce SimPoint checkpoints includes ***3 individual steps***
1. SimPoint Profiling to get BBVs. (To save space, they often output in compressed formats such as **bbv.gz**.)
1. SimPoint clustering. You can also opt to Python and sk-learn to do k-means clustering. (In this step, what is typically obtained are the **positions** selected by SimPoint and their **weights**.)
1. Taking checkpoints according to clustering results. (In the RVGCpt process, this step generates the **checkpoints** that will be used for simulation.)

If you have problem generating SPECCPU checkpoints, following links might help you.
- [The video to produce SimPoint checkpoints from SPECCPU source code](https://www.bilibili.com/video/BV1Wr421h7XN?p=2)

## Basic build environment

Install dependencies as [official GEM5 tutorial](https://www.gem5.org/documentation/general_docs/building) says:

### Setup on Ubuntu 22.04
If compiling gem5 on Ubuntu 22.04, or related Linux distributions, you may install all these dependencies using APT:

``` shell
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev libboost-all-dev pkg-config libsqlite3-dev zstd libzstd-dev
```

### Setup on Ubuntu 20.04
If compiling gem5 on Ubuntu 20.04, or related Linux distributions, you may install all these dependencies using APT:

``` shell
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev python-is-python3 libboost-all-dev pkg-config libsqlite3-dev zstd libzstd-dev
```

### Setup using [Nix](https://github.com/NixOS/nix)

If you are a Nix or NixOS user, you can setup a gem5 development environment using your familiar tools.
This project already included a [flake.nix](nix/flake.nix) for development shell and a [.envrc](.envrc) for [direnv](https://github.com/direnv/direnv)

If you don't want to use direnv or for some reason you want to manully run the nix development shell, the following command can be used.

```shell
nix develop path:nix
```

## Clone and build DRAMSim3

Refer to [The readme for DRAMSim3](ext/dramsim3/README) to install DRAMSim3.

Notes:
- If you have already built GEM5, you should rebuild gem5 after install DRAMSim3
- If simulating Xiangshan system, use DRAMSim3 with our costumized config

Use init.sh to clone and build DRAMSim3.

```shell
bash ./init.sh
```

## Build GEM5

```shell
cd GEM5
scons build/RISCV/gem5.opt --gold-linker -j8
export gem5_home=`pwd`
```

Press enter if you saw
```
You're missing the gem5 style or commit message hook. These hooks help
to ensure that your code follows gem5's style rules on git commit.
This script will now install the hook in your .git/hooks/ directory.
Press enter to continue, or ctrl-c to abort:
```

## Run Gem5

Users must properly prepare workloads before running GEM5, plz read [Workflows](#workflows-how-to-run-workloads) first.

[The example running script](util/xs_scripts/kmh_6wide.sh) contains the default command for simulate XS-GEM5.
[The example batch running script](util/xs_scripts/parallel_sim.sh) shows an example to simulate multiple workloads in parallel.

### Environment variables

Users should set the following environment variables before running GEM5:

- $GCBV_REF_SO: The reference design used in Difftest, which is the path to the `.so` file of NEMU or spike.
- $GCBV_MULTI_CORE_REF_SO: The reference design for multi-core.
- $GCB_RESTORER: A piece of RISC-V code to restore the checkpoint of RV64GCB.
- $GCBV_RESTORER: Restorer of RV64GCBV.
- $GCB_MULTI_CORE_RESTORER: Restorer of RV64GCB + multi-core.

These files can be found in the release page.
Users can also opt to build them from source ([Difftest with NEMU](#difftest-with-nemu) and
[Build GCPT restorer](#build-gcpt-restorer)).
A tested working matrix of repos & revisions is here:

|  Checkpoint Type  | reference design | GCPT restorer
| ---------- | --------- | --------- |
| RV64GCB       | NEMU master + riscv64-gem5-ref_defconfig  | NEMU master |
| RV64GCBV      | NEMU master + riscv64-gem5-ref_defconfig | NEMU gcpt_new_mem_layout |
| RV64GCB multi-core | NEMU master + riscv64-gem5-multicore-ref_defconfig | Download Binary from release; Code release soon |
| RV64GCBV multi-core | NEMU master + riscv64-gem5-multicore-ref_defconfig | ~~NOT available yet~~ |

If above branches are not working, you can try the following commits:

| Checkpoint Type | reference design | GCPT restorer
| ---------- | --------- | --------- |
| RV64GCB | NEMU 4332a525 + riscv64-gem5-ref_defconfig | NEMU 732e4ccd |
| RV64GCBV      | NEMU 4332a525 + riscv64-gem5-ref_defconfig | NEMU b966d274 |
| RV64GCB multi-core | NEMU 4332a525 + riscv64-gem5-multicore-ref_defconfig | Download Binary from release; Code release soon |
| RV64GCBV multi-core | NEMU 4332a525 + riscv64-gem5-multicore-ref_defconfig | ~~NOT available yet~~ |


**NOTE**:
- Current scripts enforce Difftest (cosimulating against NEMU or spike).
If a user does not want Difftest, please manually edit `configs/example/xiangshan.py` and `configs/common/XSConfig.py` to disable it.
Simulation error without Difftest **will NOT be responded.**
- When running a GCB checkpoint, it is OK to use GCBV reference design but not vice versa.
- When running a GCB checkpoint, user must use GCB restorer but not GCBV restorer.

### Example command

#### Easy to run
Easy to run a single workload(not a checkpoint, just a single binary file)

```shell
# prepare the binary file
git clone https://github.com/OpenXiangShan/ready-to-run.git
# prepare nemu reference design
wget https://github.com/OpenXiangShan/GEM5/releases/download/2024-10-16/riscv64-nemu-interpreter-c1469286ca32-so
# set environment variables
export GCBV_REF_SO=`realpath riscv64-nemu-interpreter-c1469286ca32-so`
# run the workload
./build/RISCV/gem5.opt ./configs/example/xiangshan.py --raw-cpt --generic-rv-cpt=./ready-to-run/coremark-2-iteration.bin
# get the ipc
grep 'cpu.ipc' m5out/stats.txt
```
xiangshan.py is the default configuration for XS-GEM5.

raw-cpt means the input is a single binary file.

generic-rv-cpt is the path to the binary file.

Then you can see the output in the terminal, find gem5 output in the `m5out` directory.

Otherwise, if you want to run a checkpoint, you should ensure GEM5 is properly built and workloads are prepared by running a single workload:
``` shel
mkdir util/xs_scripts/example
cd util/xs_scripts/example
bash ../kmh_6wide.sh /path/to/a/single/checkpoint.gz
```

Then, for running multiple workloads in parallel, one can use the batch running script:
``` shel
mkdir util/xs_scripts/example
cd util/xs_scripts/example
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` $workloads_lst /top/dir/of/checkpoints a_fancy_simulation_tag
```
In this example, parallel_sim.sh will invoke kmh_6wide.sh with GNU parallel to run multiple workloads.
Through this, parallel simulation infrastructure is decouple from the simulation script.

#### run xs-gem5 in docker
In order to be able to run scores on servers without root access, we provide a simple docker script to run xs-gem5.
For more details see [README about run in docker](./util/xs_scripts/docker/README.md).

### About workload_lst

A line of `workload_lst` is a space-separated list of workload parameters.
For example, "hmmer_nph3_15858 hmmer_nph3/15858 0 0 20 20" represents the workload name, checkpoint path, skip insts (usually 0), functional warmup insts (usually 0),
detailed warmup insts (usually 20), and sample insts (usually 20), respectively.
`parallel_sim.sh` will `find hmmer_nph3/15858/*.gz` in the /top/dir/of/checkpoints to obtain the checkpoint gz file.
Then the gz file will be passed to `kmh_6wide.sh` to run the simulation.


More details can be found in comments and code of the example running scripts.

## Play with Arch DB

Arch DB is a database to store the micro-architectural trace of the program with SQLite.
You can access it with Python or other languages.
A Python example is given [here](util/arch_db/mem_trace.py).

## Build GCPT restorer

``` shell
git clone https://github.com/OpenXiangShan/NEMU.git
cd NEMU/resource/gcpt_restore
make
export GCB_RESTORER=`realpath build/gcpt.bin`
```

If users want to build RVV version, run the following command:

``` shell

git clone https://github.com/OpenXiangShan/NEMU.git -b gcpt_new_mem_layout
# Then similar as above
# ...
export GCBV_RESTORER=`realpath build/gcpt.bin`
```

## Difftest with NEMU

NEMU is used as a reference design for XS-GEM5.
Typical workflow is as follows.

```mermaid
graph TD;
build["Build NEMU in reference mode"]
so[/"./build/riscv64-nemu-interpreter-so"/]
cosim["Run XS-GEM5 or Xiangshan processor, turn on difftest, specify riscv64-nemu-interpreter-so as reference design"]

build-->so
so-->cosim
```

We the [gem5-ref-main branch of NEMU](https://github.com/OpenXiangShan/NEMU/tree/gem5-ref-main) for difftest with XS-GEM5.

``` shell
git clone https://github.com/OpenXiangShan/NEMU.git
cd NEMU
export NEMU_HOME=`pwd`
make riscv64-gem5-ref_defconfig
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
export GCB_REF_SO=`realpath build/riscv64-nemu-interpreter-so`
```

## Difftest with spike

``` shell
git clone https://github.com/OpenXiangShan/riscv-isa-sim.git -b gem5-ref spike
cd spike/difftest && make CPU=XIANGSHAN
```
Then use `difftest/build/riscv64-spike-so` similarly as NEMU.
``` shell
export GCBV_REF_SO=`realpath difftest/build/riscv64-spike-so`
```
# FAQ

## Python problems

If your machine has a Python with very high version, you may need to install a lower version of Python
to avoid some compatibility issues. We recommend to use miniconda to install Python 3.8.

Installation command, copied from official [miniconda website](https://docs.conda.io/projects/miniconda/en/latest/)

``` shell
mkdir -p ~/miniconda3
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O ~/miniconda3/miniconda.sh
bash ~/miniconda3/miniconda.sh -b -u -p ~/miniconda3
rm -rf ~/miniconda3/miniconda.sh
```

Then add conda to path in `~/.bashrc` or `~/.zshrc`. Note this will hide the system Python.

``` shell
# for bash
~/miniconda3/bin/conda init bash
# for zsh
~/miniconda3/bin/conda init zsh
```
Restart your terminal, and you should be able to use conda. Then create a Python 3.8 env:

``` shell
# create env
conda create --name py38 --file $gem5_home/ext/xs_env/gem5-py38.txt

# This is mudatory to avoid conda auto activate base env
conda config --set auto_activate_base false
```

Each time login, you need to activate the conda env before building GEM5:

``` shell
conda activate py38
```

In case that you don't like this or it causes problem, to completely remove Python and conda from your PATH, run:

``` shell
# for bash
conda init bash --reverse
# for zsh
conda init zsh --reverse
```



## It complains `Python not found`

This is often not Python missing, but other problems.
Because the build scripts (and scons) uses a strange way to find Python, see `site_scons/gem5_scons/configure.py` for more detail.
For example, when building with clang10, I encountered this problem:

```
Error: Check failed for Python.h header.
        Two possible reasons:
       1. Python headers are not installed (You can install the package python-dev on Ubuntu and RedHat)
       2. SCons is using a wrong C compiler. This can happen if CC has the wrong value.
       CC = clang
```

This is not becaues of Python, but because GCC and clang have different warning suppression flags.
To fix it, I apply this path:

``` shell
git apply ext/xs_env/clang-warning-suppress.patch
```

But Python complaints are also possible caused by other problems,
For similar errors, check `build/RISCV/gem5.build/scons_config.log` to get the real error message.


# Original README

The README for official GEM5 is here: [Original README](./official-README.md)
