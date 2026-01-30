# NEMU + XS-GEM5 使用流程

<div style="text-align: center;">
  编写时间：2025-11-11&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;作者：李旭
</div>
## 0.前言

​	本教程的主要目的是帮助初学者完成nemu + xs-gem5 的全流程使用，具体来说，主要是详细说明了以下三个方面该如何执行：

1. ​	如何产生基于 opensbi 的自定义负载

2. ​	如何在 NEMU 上执行上述产生的负载并生成对应切片

3. ​	如何能够在 XS-GEM5 上成功运行上述切片并得到相关输出结果

   本教程主要依赖以下教程与自身实验的总结：

   https://docs.xiangshan.cc/zh-cn/latest/workloads/opensbi-kernel-for-xs/

   https://github.com/xyyy1420/checkpoint_scripts

   https://github.com/OpenXiangShan/GEM5

   https://docs.xiangshan.cc/zh-cn/latest/tools/simpoint/


## 1.环境安装

​	本教程是在 Ubuntu 22.04 的环境上完成的，接下来将详细说明其余环境的安装过程。

### 1.1 gcc 安装

​	本教程所采用的的 gcc 版本为 15.2.0。

##### 1.1.1 gcc 版本查询

​	可以通过在终端输入：

```bash
gcc --version
```

​	可以看到如下输出：

```
    gcc (GCC) 15.2.0
    Copyright (C) 2025 Free Software Foundation, Inc.
    This is free software; see the source for copying conditions.  There is NO
    warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
```

​	如果 gcc 版本为 15.2.0，请跳转到 1.1.2 节，否则继续以下内容。

##### 1.1.2 gcc 版本升级

​	进行 gcc 对应版本的安装前需要安装以下相关工具：

```bash
sudo apt install -y build-essential bison flex texinfo libmpc-dev libmpfr-dev libgmp-dev
```

​	在终端上输入以下命令来下载相关源码：

```bash
wget https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.gz
```

​	然后进行解压：

```bash
tar -xzvf gcc-15.2.0.tar.gz
```

​	接着进入解压后的目录并进行编译：

```bash
cd gcc-15.2.0
mkdir build
cd build
../configure --prefix=/usr/local/gcc-15.2.0 --enable-languages=c,c++ --disable-multilib
# ../configure   --prefix=/usr/local/gcc-15.2.0   --enable-languages=c,c++,fortran   --disable-multilib
make -j$(nproc)
```

​	编译过程需要等待较长时间，在完成编译过程后进行安装：

```bash
sudo make install
```

​	然后进行环境变量的添加：

```bash
echo 'export PATH=/usr/local/gcc-15.2.0/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

​	全部完成后可以通过 1.1.1 的命令检查 gcc 版本

### 1.2 riscv 交叉编译链安装

​	首先需要安装一些准备工具：

```bash
$ sudo apt-get install autoconf automake autotools-dev curl python3 python3-pip python3-tomli libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev ninja-build git cmake libglib2.0-dev libslirp-dev
```

​	然后下载源代码:

```bash
git clone https://github.com/riscv/riscv-gnu-toolchain
```

​	本教程下载时对应的 riscv-gnu-toolchain 的 tag 为 `2025.10.18` 。

​	接着进行 riscv 交叉编译链的正式安装：

```bash
cd riscv-gnu-toolchain
./configure --prefix=/opt/riscv
make
```

​	此时会得到 `riscv64-unknown-elf-` ，此外还需要进行如下操作：

```bash
cd riscv-gnu-toolchain
./configure --prefix=/opt/riscv
# ./configure   --prefix=/opt/riscv   --enable-multilib   --with-arch=rv64gcv   --with-abi=lp64d
make linux
```

​	此时会得到 `riscv64-unknown-linux-gnu-` ，将其导入环境变量：

```bash
export PATH=$PATH:/opt/riscv/bin
```

​	此时应该能在终端检查两者对应版本：

```bash
riscv64-unknown-elf-gcc --version
riscv64-unknown-linux-gnu-gcc --version
```

### 1.3 其余软件源码下载

​	本教程中所用到的其余软件源码还有 opensbi、linux kernel、nemu_board、NEMU、LibCheckpointAlpha、riscv-rootfs和 XS-GEM5，下载链接如下：

```bash
git clone https://github.com/riscv-software-src/opensbi.git
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.10.3.tar.xz
git clone https://github.com/OpenXiangShan/nemu_board.git
git clone https://github.com/OpenXiangShan/NEMU.git
git clone https://github.com/OpenXiangShan/LibCheckpointAlpha.git
git clone https://github.com/OpenXiangShan/riscv-rootfs.git
git clone https://github.com/OpenXiangShan/GEM5 
```

​	对应的 commit  hash 如下：

| repository         | commit hash                              |
| ------------------ | ---------------------------------------- |
| opensbi            | 55296fd27c0ce12a2024c7eacfdbab2dfd39476b |
| nemuboard          | bb48824de93af53934c4064999d81aae0d91604f |
| NEMU               | d3b94c7302b08d8f86cb2d5980576d680cd1d963 |
| LibCheckpointAlpha | 492de34e54a0450b096649b403d2f42b39235b8a |
| riscv-rootfs       | be5a3f0843fc7b05fa70d9f892b66203d513a75d |
| GEM5               | 3dc8edbbe422dc477faf866285f84093fb68e609 |

## 2.生成负载

​	生成负载的整个流程如下图所示：

<img src="./image-20251112160550251.png" alt="image-20251112160550251" style="zoom:33%;" />

### 2.1 设置环境变量

```bash
export RISCV_LINUX_HOME=/path/to/linux_kernel
export RISCV_ROOTFS_HOME=/path/to/riscv-rootfs
export WORKLOAD_BUILD_ENV_HOME=/path/to/nemuboard
export OPENSBI_HOME=/path/to/opensbi
export GCPT_HOME=/path/to/LibCheckpointAlpha
export RISCV=/opt/riscv
export ARCH=riscv
export CROSS_COMPILE=$RISCV/bin/riscv64-unknown-linux-gnu-
```

​	其中 `/path/to` 路径要指向自己机器的对应存放路径。

### 2.2 构建 rootfs

#### 2.2.1 编译负载

​	首先我们进入对应目录：

```bash
cd $RISCV_ROOTFS_HOME
```

​	查看此目录文件下的 Makefile 文件，其主要功能就是编译 `APPS` 指定的负载，其负载对应与`$RISCV_ROOTFS_HOME/apps`文件夹下的同名负载。

```makefile
$(shell mkdir -p rootfsimg/build)

APPS = hello busybox before_workload trap qemu_trap dtc lkvm-static

APPS_DIR = $(addprefix apps/, $(APPS))

.PHONY: rootfsimg $(APPS_DIR) clean

rootfsimg: $(APPS_DIR)

$(APPS_DIR): %:
	-$(MAKE) -s -C $@ install

clean:
	-$(foreach app, $(APPS_DIR), $(MAKE) -s -C $(app) clean ;)
	-rm -f rootfsimg/build/*
	-rm -rf apps/busybox/repo
```

​	先进行最基础的编译，注释掉其余所有负载，只保留 `hello`，将上述 Makefile 文件对应部分修改为：

```makefile
APPS = hello #busybox before_workload trap qemu_trap dtc lkvm-static
```

​	执行编译：

```bash
make
```

​	编译之后我们会在`$RISCV_ROOTFS_HOME/rootfsimg/build/`目录下发现编译好的对应文件`hello`。

​	此外，通过在`$RISCV_ROOTFS_HOME/apps`加入自己的自定义负载，并设置好自己自定义负载的 Makeflie（具体可以仿照已给项目的 Makefile 和该负载自身的Makefile进行构建），并在`$RISCV_ROOTFS_HOME`目录下的Makefile文件对应的`APPS`变量中加入自己负载对应的文件夹名，就可以同样方法编译自己的自定义负载。特别要注意的是，如果想要编译`cpp`文件，同时想要用`$RISCV_ROOTFS_HOME`文件夹下对应的Makefile.compile文件，需要将其修改为：

```makefile
include $(RISCV_ROOTFS_HOME)/Makefile.app

# Compilation flags

CROSS_COMPILE ?= riscv64-unknown-linux-gnu-

AS = $(CROSS_COMPILE)gcc
CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
# LD = $(CROSS_COMPILE)gcc
LD_C = $(CROSS_COMPILE)gcc
LD_CPP = $(CROSS_COMPILE)g++

LINKER = $(if $(filter %.cpp,$(SRCS)),$(LD_CPP),$(LD_C))

INCLUDES  = $(addprefix -I, $(INC_DIR))

CFLAGS   += -O2 -MMD $(INCLUDES)
CXXFLAGS += -O2 -MMD $(INCLUDES)

# Files to be compiled
OBJS = $(addprefix $(DST_DIR)/, $(addsuffix .o, $(basename $(SRCS))))

# Compilation patterns
$(DST_DIR)/%.o: %.cpp
	@echo + CXX $<
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c -o $@ $<
$(DST_DIR)/%.o: %.c
	@echo + CC $<
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

# $(APP): $(OBJS)
#	@echo + LD $@
#	$(LD) $(LDFLAGS) -o $(APP) $(OBJS)
$(APP): $(OBJS)
	@echo + LD $@
	@$(LINKER) $(LDFLAGS) -o $(APP) $(OBJS)

# Dependencies
DEPS = $(addprefix $(DST_DIR)/, $(addsuffix .d, $(basename $(SRCS))))
-include $(DEPS)

```

​	这是因为在`ld`的时候，原先的文件统一采用了`gcc`，此时我们应该对其进行判断，从而选择`g++`。

#### 2.2.2 修改initramfs

​	在仿真环境中，我们想要 Linux Kernel 在 ramfs 上启动，所以我们需要准备好想要运行的initramfs文件，里面存放着我们想要运行的 workload，默认情况下使用的是`riscv-rootfs/rootfsimg/initramfs-emu.txt`(可以在 Linux Kernel 的 menuconfig 中设置，详情见 2.3 和 2.4)，以其为例：

```
dir /dev 755 0 0

nod /dev/console 644 0 0 c 5 1
nod /dev/null 644 0 0 c 1 3

# for emu
file /init ${RISCV_ROOTFS_HOME}/rootfsimg/build/hello 755 0 0

```

​	第一行是标志着要创造目录，第三行与第四行则是创建设备文件，第七行代表要把哪些本机中的文件复制到我们将要启动的ramfs的对应位置上，启动时会自动执行`/init`对应的程序，如果想要执行自定义的负载，可以参照`riscv-rootfs/rootfsimg/initramfs.txt`文件进行修改，特别主要要把负载运行依赖的库文件添加进去，利用`file`指定文件的时候，一定要注意对应的路径上所有文件夹都被`dir`产生过。

### 2.3 构建设备树

​	按照以下命令执行：

```bash
cd $WORKLOAD_BUILD_ENV_HOME
ln -s $WORKLOAD_BUILD_ENV_HOME/dts/platform_noop.dtsi $WORKLOAD_BUILD_ENV_HOME/dts/platform.dtsi
cd dts
./build_single_core_for_nemu.sh
```

​	此时会在` $WORKLOAD_BUILD_ENV_HOME/dts/build`文件夹下生成`xiangshan.dtb`的设备树文件。

​	同时注意，`$WORKLOAD_BUILD_ENV_HOME/configs/xiangshan_defconfig`保存着 Linux kernel 的一些编译的基本设置：

```makefile
CONFIG_DEFAULT_HOSTNAME="(lvna)"
# CONFIG_CROSS_MEMORY_ATTACH is not set
CONFIG_LOG_BUF_SHIFT=15
CONFIG_BLK_DEV_INITRD=y
CONFIG_INITRAMFS_SOURCE="${RISCV_ROOTFS_HOME}/rootfsimg/initramfs-emu.txt"
CONFIG_EXPERT=y
# CONFIG_SYSFS_SYSCALL is not set
# CONFIG_FHANDLE is not set
CONFIG_NONPORTABLE=y
CONFIG_SMP=y
CONFIG_RISCV_SBI_V01=y
# CONFIG_BLOCK is not set
# CONFIG_BINFMT_SCRIPT is not set
# CONFIG_SLAB_MERGE_DEFAULT is not set
# CONFIG_COMPAT_BRK is not set
# CONFIG_COMPACTION is not set
# CONFIG_VM_EVENT_COUNTERS is not set
# CONFIG_STANDALONE is not set
# CONFIG_PREVENT_FIRMWARE_BUILD is not set
# CONFIG_FW_LOADER is not set
# CONFIG_ALLOW_DEV_COREDUMP is not set
# CONFIG_INPUT_KEYBOARD is not set
# CONFIG_INPUT_MOUSE is not set
CONFIG_SERIO_LIBPS2=y
# CONFIG_VT_CONSOLE is not set
# CONFIG_UNIX98_PTYS is not set
# CONFIG_LEGACY_PTYS is not set
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_8250_DW=y
CONFIG_SERIAL_OF_PLATFORM=y
CONFIG_SERIAL_EARLYCON_RISCV_SBI=y
CONFIG_SERIAL_UARTLITE=y
CONFIG_SERIAL_UARTLITE_CONSOLE=y
CONFIG_HVC_RISCV_SBI=y
CONFIG_HW_RANDOM_TIMERIOMEM=y
# CONFIG_DEVMEM is not set
# CONFIG_HWMON is not set
# CONFIG_USB_SUPPORT is not set
# CONFIG_VIRTIO_MENU is not set
# CONFIG_FILE_LOCKING is not set
# CONFIG_DNOTIFY is not set
# CONFIG_INOTIFY_USER is not set
CONFIG_PROC_CHILDREN=y
# CONFIG_SYSFS is not set
# CONFIG_MISC_FILESYSTEMS is not set
CONFIG_PRINTK_TIME=y
CONFIG_STACKTRACE=y
CONFIG_RCU_CPU_STALL_TIMEOUT=300
# CONFIG_RCU_TRACE is not set
# CONFIG_FTRACE is not set
# CONFIG_RUNTIME_TESTING_MENU is not set

```

​	特别注意这里的`CONFIG_INITRAMFS_SOURCE`，该项指向的就是我们所要的initramfs.txt文件的地址，可以直接修改这里从而不使用默认的`${RISCV_ROOTFS_HOME}/rootfsimg/initramfs-emu.txt`。

### 2.4 构建Linux Kernel

​	执行如下命令：

```bash
cd $RISCV_LINUX_HOME
cp $WORKLOAD_BUILD_ENV_HOME/configs/xiangshan_defconfig $RISCV_LINUX_HOME/arch/riscv/configs/xiangshan_defconfig
make xiangshan_defconfig
```

​	这个时候就会按照我们2.3节中修改的`xiangshan_defconfig`配置内核，如果想要更多的自定义配置，可以通过menuconfig 进行修改：

```bash
make menuconfig
```

​	修改完配置后编译即可：

```bash
make -j$(nproc)

```

​	会在`$RISCV_LINUX_HOME/arch/riscv/boot/`目录下得到`Image`文件。

### 2.5 构建opensbi 并链接内核

​	执行如下命令：

```bash
cd $OPENSBI_HOME
make PLATFORM=generic FW_PAYLOAD_PATH=$RISCV_LINUX_HOME/arch/riscv/boot/Image FW_FDT_PATH=$WORKLOAD_BUILD_ENV_HOME/dts/build/xiangshan.dtb FW_PAYLOAD_OFFSET=0x200000
```

​	会在`$OPENSBI_HOME/build/platform/generic/firmware/`目录下得到`fw_payload.bin`文件。

​	特别主要，如果 2.4 节中的 `Image` 文件大于 32MB，需要计算`FW_PAYLOAD_FDT_ADDR` =（该文件大小 + 2M + 0x80000000）以 1M 对齐，作为 OpenSBI 重定向设备树的地址并使用该命令构建 OpenSBI：

```bash
make PLATFORM=generic FW_PAYLOAD_PATH=$RISCV_LINUX_HOME/arch/riscv/boot/Image FW_FDT_PATH=$WORKLOAD_BUILD_ENV_HOME/dts/build/xiangshan.dtb FW_PAYLOAD_OFFSET=0x100000 FW_PAYLOAD_FDT_ADDR=$(FW_PAYLOAD_FDT_ADDR)
```

### 2.6 使用 gcpt 链接 opensbi

​	执行如下命令：

```bash
cd $GCPT_HOME
make GCPT_PAYLOAD_PATH=$OPENSBI_HOME/build/platform/generic/firmware/fw_payload.bin
```

​	会在`$GCPT_HOME/build/gcpt`目录下得到`gcpt.bin`文件，该文件可以由 nemu 直接启动。特别注意的是，理论上可以使用 XS-GEM5 加上`--raw-cpt`直接运行该文件，但是在带上 nemu 后，已经很久没有完整用 XS-GEM5 来启动linux了，特别是初始化寄存器和csr 等等不能保证还能用，主要目前还是关注切片来测性能，不建议使用 XS_GEM5 直接启动该文件。

## 3 基于 NEMU 生成 checkpoint

​	执行如下命令：

```bash
export NEMU_HOME=/path/to/nemu
cd $NEMU_HOME
git submodule update --init
cd $NEMU_HOME/resource/simpoint/simpoint_repo
make
```

​	此时会在`$NEMU_HOME/resource/simpoint/simpoint_repo/bin/`文件夹下得到`simpoint`文件。

​	再执行如下命令：

```bash
cd $NEMU_HOME
make riscv64-xs-cpt_defconfig
```

​	此时会按照`$NEMU_HOME/configs/riscv64-xs-cpt_defconfig`的设置配置 NEMU 的编译选项，特别要注意的是该文件中的`CONFIG_RV_SV39`与`CONFIG_RV_SV48`这两个选项，NEMU 默认的是`CONFIG_RV_SV48`，但是 XS-GEM5 目前只支持`CONFIG_RV_SV39`，所以默认情况下编译产生的checkpoint 无法在 XS-GEM5 上正常执行，需要将对应内容修改为：

```makefile
CONFIG_RV_SV39=y
# CONFIG_RV_SV48 is not set
```

​	编译即可：

```bash
make -j$(nproc)
```

​	可以在`$NEMU_HOME/build/riscv64-nemu-interpreter`找到对应的执行文件，其执行的选项可以参考https://github.com/OpenXiangShan/NEMU/blob/54894f558d61a5833260f1f158452d24ea6237c1/src/monitor/monitor.c#L232。

​	利用如下命令就可以执行我们在2节生成的负载：

```bash
./build/riscv64-nemu-interpreter -b path/to/gcpt.bin
```

​	此外，默认情况下，NEMU并不会进入checkpoint模式，需要使用NEMU自定义指令进行模式转换，nemu trap具体如下：

1. NEMU 使用 nemu_trap 指令（0x6b），进行 `结束运行`，`关闭时钟中断`，`进入 Simpoint Profiling 模式` ，具体行为由 `a0` 寄存器内容决定。
2. a0：0x100 - 关闭时钟中断
3. a0：0x101 - 进入 Simpoint Profiling 模式
4. a0：其他 - NEMU 结束执行，返回 a0 内容。若 a0 内容为 0，NEMU 认为程序 GOOD_TRAP。

​	因此在真正执行目标程序之前，需要完成两个操作 关闭时钟中断 和 进入 Simpoint Profiling 模式。可以将如下程序编译后，在运行目标程序之前执行，以完成上述两个操作。在目标程序结束后，可以执行 GOOD_TRAP 。

```c
#define DISABLE_TIME_INTR 0x100
#define NOTIFY_PROFILER 0x101
#define GOOD_TRAP 0x0

void nemu_signal(int a){
    asm volatile ("mv a0, %0\n\t"
                  ".insn r 0x6B, 0, 0, x0, x0, x0\n\t"
                  :
                  : "r"(a)
                  : "a0");
}

int main(){
    nemu_signal(DISABLE_TIME_INTR);
    nemu_signal(NOTIFY_PROFILER);
}
```

​	该文件可以在2.2节中的`$RISCV_ROOTFS_HOME/apps`对应下的`beforeworkload`文件夹下找到，当然也可以把这个`nemu_signal`指令直接放入到你的程序中，从而更精确的定位workload的部分作为你的测量，而不是默认的测量整个workload。

随后就可以只用相关脚本进行checkpoint的生成了：

​	SimPoint

```bash
#!/bin/bash

# prepare env

export NEMU_HOME=
export NEMU=$NEMU_HOME/build/riscv64-nemu-interpreter
export GCPT=$NEMU_HOME/resource/gcpt_restore/build/gcpt.bin
export SIMPOINT=$NEMU_HOME/resource/simpoint/simpoint_repo/bin/simpoint

export WORKLOAD_ROOT_PATH=
export LOG_PATH=$NEMU_HOME/checkpoint_example_result/logs
export RESULT=$NEMU_HOME/checkpoint_example_result
export profiling_result_name=simpoint-profiling
export PROFILING_RES=$RESULT/$profiling_result_name
export interval=$((20*1000*1000))

# Profiling
# using config: riscv64-xs-cpt_defconfig
profiling(){
    set -x
    workload=$1
    log=$LOG_PATH/profiling_logs
    mkdir -p $log

    $NEMU ${WORKLOAD_ROOT_PATH}/${workload}.bin \
        -D $RESULT -w $workload -C $profiling_result_name    \
        -b --simpoint-profile --cpt-interval ${interval} > $log/${workload}-out.txt 2>${log}/${workload}-err.txt
}

export -f profiling

profiling bbl

# Cluster

cluster(){
    set -x
    workload=$1

    export CLUSTER=$RESULT/cluster/${workload}
    mkdir -p $CLUSTER

    random1=`head -20 /dev/urandom | cksum | cut -c 1-6`
    random2=`head -20 /dev/urandom | cksum | cut -c 1-6`

    log=$LOG_PATH/cluster_logs/cluster
    mkdir -p $log

    $SIMPOINT \
        -loadFVFile $PROFILING_RES/${workload}/simpoint_bbv.gz \
        -saveSimpoints $CLUSTER/simpoints0 -saveSimpointWeights $CLUSTER/weights0 \
        -inputVectorsGzipped -maxK 30 -numInitSeeds 2 -iters 1000 -seedkm ${random1} -seedproj ${random2} \
        > $log/${workload}-out.txt 2> $log/${workload}-err.txt
}

export -f cluster

cluster bbl

# Checkpointing
# using config: riscv64-xs-cpt_defconfig
checkpoint(){
    set -x
    workload=$1

    export CLUSTER=$RESULT/cluster
    log=$LOG_PATH/checkpoint_logs
    mkdir -p $log
    $NEMU ${WORKLOAD_ROOT_PATH}/${workload}.bin \
         -D $RESULT -w ${workload} -C spec-cpt  \
         -b -S $CLUSTER --cpt-interval $interval \
         --checkpoint-format zstd > $log/${workload}-out.txt 2>$log/${workload}-err.txt
}

export -f checkpoint

checkpoint bbl

# Checkpoint store in flash
# using config: riscv64-xs-cpt-with-flash_defconfig
checkpoint_in_flash(){
    set -x
    workload=$1

    export CLUSTER=$RESULT/cluster
    log=$LOG_PATH/checkpoint_flash_logs
    mkdir -p $log
    $NEMU ${WORKLOAD_ROOT_PATH}/${workload}.bin \
        -D $RESULT -w ${workload} -C spec-cpt \
        -b -S $CLUSTER --cpt-interval $interval \
        --store-cpt-in-flash \
        --checkpoint-format zstd > $log/{workload}-flash-out.txt 2>$log/${workload}-flash-err.txt
}

export -f checkpoint_in_flash

checkpoint_in_flash bbl
```

​	Uniform Checkpoint

```bash
#!/bin/bash

# prepare env

export NEMU_HOME=
export NEMU=$NEMU_HOME/build/riscv64-nemu-interpreter
export GCPT=$NEMU_HOME/resource/gcpt_restore/build/gcpt.bin
export SIMPOINT=$NEMU_HOME/resource/simpoint/simpoint_repo/bin/simpoint

export WORKLOAD_ROOT_PATH=
export LOG_PATH=$NEMU_HOME/checkpoint_example_result/logs
export RESULT=$NEMU_HOME/checkpoint_example_result
export profiling_result_name=simpoint-profiling
export PROFILING_RES=$RESULT/$profiling_result_name
export interval=$((20*1000*1000))

uniform_cpt(){
    set -x
    workload=$1
    log=$LOG_PATH/uniform
    mkdir -p $log
    name="uniform"

    $NEMU ${WORKLOAD_ROOT_PATH}/${workload}.bin \
        -D $RESULT -w $workload -C $name      \
        -b -u --cpt-interval ${interval}   --dont-skip-boot         \
        --checkpoint-format zstd > $log/${workload}-out.txt 2>${log}/${workload}-err.txt
}

export -f uniform_cpt

uniform_cpt bbl
```

​	可以仿照上述两种命令格式生成自己的 checkpoint。

## 4.使用 XS-GEM5 运行切片

### 4.1 安装 XS-GEM5

​	首先进行相关依赖的安装：

```bash
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev libboost-all-dev pkg-config libsqlite3-dev zstd libzstd-dev
```

​	然后下载相关源码：

```bash
git clone https://github.com/OpenXiangShan/GEM5.git
export gem5_home=/path/to/GEM5
```

​	安装 DRAMSim3:

```bash
cd $gem5_home
bash ./init.sh  # 克隆并构建DRAMSim3，只需要执行一次，后续构建GEM5时不需要重复执行
```

​	构建XS-GEM5：

```bash
scons build/RISCV/gem5.opt --gold-linker -j$(nproc)
```

​	等待一段时间后，即可完成XS-GEM5的构建。

### 4.2 运行checkpoint

​	执行如下命令：

```bash
cd $gem5_home
./build/RISCV/gem5.opt ./configs/example/kmhv3.py --generic-rv-cpt=/path/to/a/single/checkpoint.gz
```

​	即可成功运行该切片`checkpoint.gz`，同时需要注意的是，存在`checkpoint.zstd`的切片，XS-GEM5均兼容，具体产生的类型取决于3节中的`--checkpoint-format`选项，如果想要了解具体有什么类型的参数，可以查阅https://github.com/OpenXiangShan/GEM5/blob/xs-dev/configs/common/Options.py，特别需要注意的是`--maxinsts`选项，当XS-GEM5运行到其所指定的指令数后就会停止，可以指定其为 0 从而避免停止。

​	在运行结束后，默认的仿真输出结果会在`$gem5_home/m5out`目录下，可以在该目录下查阅所需内容，此外 XS-GEM5 的更详细内容可以参考 https://xs-gem5.readthedocs.io/zh-cn/latest/quick_start/。

## 5 后记

​	特别地，如果觉得以上流程过于繁琐，可以参考https://github.com/xyyy1420/checkpoint_scripts 的相关脚进行修改，完成自动化地执行，建议在对整个流程熟悉之后再进行该自动化脚本的阅读与修改，这样更方便理解该自动化脚本的操作内容，便于自己进行修改。

​	特别感谢香山的晏悦师兄提供的相关帮助
