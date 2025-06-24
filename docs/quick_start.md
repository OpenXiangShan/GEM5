# 快速开始

## 安装环境

### Ubuntu 22.04 依赖安装

```bash
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev libboost-all-dev pkg-config libsqlite3-dev zstd libzstd-dev
```

**Note:** 目前建议在ubuntu 22.04上进行编译，ubuntu 20.04可能存在兼容性问题。

### Python环境配置

如果您的机器Python版本过高，可能需要安装较低版本以避免兼容性问题。推荐使用miniconda安装Python 3.8：

```bash
mkdir -p ~/miniconda3
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O ~/miniconda3/miniconda.sh
bash ~/miniconda3/miniconda.sh -b -u -p ~/miniconda3
rm -rf ~/miniconda3/miniconda.sh

# for bash
~/miniconda3/bin/conda init bash
# for zsh
~/miniconda3/bin/conda init zsh
```

重启终端后，创建Python 3.8环境：

```bash
# 创建环境
conda create --name py38 --file $gem5_home/ext/xs_env/gem5-py38.txt
# 避免conda自动激活base环境
conda config --set auto_activate_base false
# 每次登录需要激活conda环境
conda activate py38
```

也可以使用pyenv安装Python 3.8：

```bash
pyenv install 3.8.13
pyenv local 3.8.13
```
激活环境：

```bash
pyenv activate py38
```

## 克隆与构建

1. 克隆仓库：
```bash
git clone https://github.com/OpenXiangShan/GEM5.git
cd GEM5
```

2. 安装DRAMSim3：
```bash
bash ./init.sh  # 克隆并构建DRAMSim3，只需要执行一次，后续构建GEM5时不需要重复执行
```

3. 构建GEM5：
```bash
scons build/RISCV/gem5.opt --gold-linker -j$(nproc) # 用gold-linker链接，可以加快编译速度
export gem5_home=`pwd`
```

## 最简单运行起来

以下为运行单个workload（二进制文件）的简单流程：

```bash
# 下载我们提供的二进制文件
git clone https://github.com/OpenXiangShan/ready-to-run.git
# 准备nemu参考设计，直接下载我们编译好的so文件
wget https://github.com/OpenXiangShan/GEM5/releases/download/2024-10-16/riscv64-nemu-interpreter-c1469286ca32-so
# 设置环境变量，指向nemu参考设计
export GCBV_REF_SO=`realpath riscv64-nemu-interpreter-c1469286ca32-so`
# 运行workload，注意输入的是bin文件
./build/RISCV/gem5.opt ./configs/example/xiangshan.py --raw-cpt --generic-rv-cpt=./ready-to-run/coremark-2-iteration.bin
# 获取IPC
grep 'cpu.ipc' m5out/stats.txt
```

- `xiangshan.py` 是XS-GEM5的默认配置脚本
- `raw-cpt` 表示输入为单一二进制文件，如果运行切片不需要添加这个选项
- `generic-rv-cpt` 指定二进制文件路径，默认均为bin文件，无论是切片还是裸机程序
- 仿真输出在 `m5out` 目录， 可以通过-d 指定输出目录


## Difftest配置

如果你不了解difftest，可以参考[Difftest](https://docs.xiangshan.cc/zh-cn/latest/tools/difftest/)。
简单理解就是，给定一个golden model，当运行GEM5时候，difftest可以比较GEM5模拟器和golden model的指令级差异、ISA级差异、内存差异等。
（每当GEM5 commit 一条指令后，比较GEM5和golden model 的寄存器值和CSR值等，如果不一致，则认为GEM5模拟器出错, 并打印出所有不一致的指令和寄存器值）。

默认情况下，GEM5使用NEMU作为golden model，请参考[NEMU](https://github.com/OpenXiangShan/nemu)。

**Note:** 你可以直接下载我们release页面提供的difftest版本，不需要自己编译。也可以按照以下步骤自己编译。

### 使用NEMU进行Difftest：

```bash
git clone https://github.com/OpenXiangShan/NEMU.git
cd NEMU
export NEMU_HOME=`pwd`
make riscv64-gem5-ref_defconfig # 配置NEMU作为reference model 模式
make -j 10
# 设置GEM5需要的环境变量
export GCB_REF_SO=`realpath build/riscv64-nemu-interpreter-so`
```

### 使用Spike进行Difftest：

```bash
git clone https://github.com/OpenXiangShan/riscv-isa-sim.git -b gem5-ref spike
cd spike/difftest && make CPU=XIANGSHAN
# 设置环境变量
export GCBV_REF_SO=`realpath difftest/build/riscv64-spike-so`
```

## 构建GCPT恢复器

**Note:** 目前新版本切片或者裸机程序，都不需要使用GCPT恢复器了，所以可以跳过这一步。同时设置GCPT恢复器环境变量为空
```bash
export GCB_RESTORER=
```

如果需要使用GCPT恢复器，请参考以下步骤：
```bash
git clone https://github.com/OpenXiangShan/NEMU.git
cd NEMU/resource/gcpt_restore
make
export GCB_RESTORER=`realpath build/gcpt.bin`

# 构建RVV版本
git clone https://github.com/OpenXiangShan/NEMU.git -b gcpt_new_mem_layout
# 然后类似上面的操作
export GCBV_RESTORER=`realpath build/gcpt.bin`
```

## 运行裸机程序
请先阅读[Abstract Machine](https://github.com/OpenXiangShan/nexus-am)裸机环境

你可以使用ready-to-run目录下编译好的coremark-2-iteration.bin，也可以按照如下步骤编译自己的裸机程序：

```bash
git clone https://github.com/OpenXiangShan/nexus-am.git
cd nexus-am
export AM_HOME=`pwd` # 设置AM_HOME
cd apps/coremark
# 需要下载riscv 交叉编译工具链才能编译
make ARCH=riscv64-xs

# 返回GEM5根目录
cd $GEM5_HOME
# 运行裸机程序
./build/RISCV/gem5.opt ./configs/example/xiangshan.py --raw-cpt --generic-rv-cpt=$AM_HOME/apps/coremark/build/coremark-riscv64-xs.bin
```

## 运行Checkpoint

如需运行checkpoint，请先准备好checkpoint文件：
可以参考[Checkpoint](https://docs.xiangshan.cc/zh-cn/latest/tools/simpoint/)。

还可以使用一键脚本生成checkpoint，参考[deterload](https://github.com/OpenXiangShan/deterload)。
**Note:** 目前deterload还在开发中，所以可能存在一些问题。


当准备好checkpoint文件后，可以运行以下命令来运行checkpoint：

```bash
mkdir util/xs_scripts/example
cd util/xs_scripts/example
bash ../kmh_6wide.sh /path/to/a/single/checkpoint.gz
```

上方命令等效于
```bash
./build/RISCV/gem5.opt ./configs/example/xiangshan.py --generic-rv-cpt=/path/to/a/single/checkpoint.gz
```

## 批量仿真

如需批量运行多个workload，可使用批量脚本：

```bash
mkdir util/xs_scripts/example
cd util/xs_scripts/example
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` $workloads_lst /top/dir/of/checkpoints a_fancy_simulation_tag
```

- `parallel_sim.sh` 会调用 `kmh_6wide.sh`，并用GNU parallel批量运行多个workload
- 仿真结果会输出到各自的目录

### 关于workload_lst

`workload_lst`的每一行是由空格分隔的workload参数列表。例如："hmmer_nph3_15858 hmmer_nph3/15858 0 0 20 20"分别表示workload名称、checkpoint路径、跳过指令数（通常为0）、功能预热指令数（通常为0）、详细预热指令数（通常为20）和采样指令数（通常为20）。`parallel_sim.sh`会在`/top/dir/of/checkpoints`中查找`hmmer_nph3/15858/*.gz`文件，然后将该gz文件传递给`kmh_6wide.sh`进行仿真。

## 在Docker中运行

为了能够在没有root访问权限的服务器上运行，我们提供了一个简单的docker脚本来运行xs-gem5。更多细节请参阅关于在docker中运行的README。

也可以使用我们提供的docker镜像，基于ubuntu22.04，镜像中已经安装了所有依赖。
```bash
docker pull zhangqianlong/ubuntu22_gem5:v1
docker run -it zhangqianlong/ubuntu22_gem5:v1
```


## Arch DB使用

Arch DB是一个使用SQLite存储程序微架构跟踪的数据库。您可以使用Python或其他语言访问它。

## 常见问题

### Python问题

如果出现"Python not found"错误，这通常不是Python缺失，而是其他问题。检查`build/RISCV/gem5.build/scons_config.log`获取真正的错误信息。

对于使用clang10时遇到的问题，可以应用以下补丁：
```bash
git apply ext/xs_env/clang-warning-suppress.patch
```

## 参考文档

- 官方GEM5文档：[GEM5官方文档](https://www.gem5.org/documentation/)
- OpenXiangShan GEM5项目：[https://github.com/OpenXiangShan/GEM5](https://github.com/OpenXiangShan/GEM5) 
- 参考[https://github.com/shinezyy/micro-arch-training](https://github.com/shinezyy/micro-arch-training)有一些练手项目