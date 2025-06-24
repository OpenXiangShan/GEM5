# spec 分析方法

目前工作环境都在大机房，其中105号机器（172.19.20.105）可以使用perf

### spec 环境准备
1. cpu 2006 源代码：/nfs/home/share/cpu2006v99
    1. ./install.sh  安装
    2. 默认的编译选项无法使用，可以新建config/riscv.cfg, 添加如下内容

```cpp
# Tester Description
test_sponsor       = Test Sponsor (Optional, defaults to hw_vendor)
tester             = (Optional, defaults to hw_vendor)

# System Description
hw_model           = Unknown HW Model
hw_memory          = 4 GB
hw_disk            = 1 1TB Mystery Disk
hw_vendor          = Berkeley Architecture Research
hw_other           = None
hw_avail           = Dec-9999

# CPU description
# See http://www.spec.org/cpu2006/Docs/runrules.html#cpucount
# for a discussion of these fields

hw_cpu_name        = Unknown RISC-V CPU 
hw_cpu_char        =
hw_cpu_mhz         = 2000
hw_fpu             = Integrated
hw_nchips          = 1
hw_ncores          = number of cores enabled
hw_ncoresperchip   = number of cores manufactured into each chip
hw_nthreadspercore = number of threads enabled per core
hw_ncpuorder       = 1,2 chips

# Cache description

hw_pcache          = 9999 MB I + 9999 MB D on chip per chip
hw_scache          = 9999 MB I+D on chip per chip
hw_tcache          = 9999 MB I+D off chip per chip
hw_ocache          = None

# Tester description 

license_num     = 0

# Operating system, file system

sw_os           = RISC-V Proxy-Kernel Version Unknown
sw_file         = Unknown File System
sw_state        = Multi-user
sw_other        = None
 
## SW config
sw_compiler        = gcc, g++ 10.3.0
sw_avail           = Dec-2022
sw_base_ptrsize    = 64-bit
sw_peak_ptrsize    = 64-bit
 
#######################################################################
# End of SUT section
# If this config file were to be applied to several SUTs, edits would
# be needed only ABOVE this point.
######################################################################

ignore_errors = yes
tune          = base
basepeak      = yes
ext           = rv64gcb-gcc12.2.0-o3-gromacs
output_format = asc,csv,html

# The publicly-accessible PathScale flags file at the URL below works
# with the settings in this file.  If you change compilers or compiler
# settings, you'll likely need to use a different flags file.
#flagsurl0     = $[top]/config/flags/riscv64-gcc-flags-revA.xml
#flagsurl1     = $[top]/config/flags/riscv64-linux-platform-revA.xml

reportable    = yes

default=default=default=default:
#####################################################################
#
# Compiler selection
#
#####################################################################

# below we assume that the binaries will be run on top of the riscv-pk, 
# which needs a static binary with the program loaded at 0x10000.  
# Ideally, we would use newlib (riscv64-unknown-elf-gcc) which uses 
# those settings by default, however some of the SPEC benchmarks require 
# glibc.

CC  = riscv64-unknown-linux-gnu-gcc -static
CXX = riscv64-unknown-linux-gnu-g++ -static
FC  = riscv64-unknown-linux-gnu-gfortran -static

#####################################################################
# Optimization
#####################################################################

#default=base=default=default:
default=base=default=default:
#COPTIMIZE      = -O3 -fno-strict-aliasing -mabi=lp64d -march=rv64gcv -std=gnu99
#CXXOPTIMIZE    = -O3 -fno-strict-aliasing -mabi=lp64d -march=rv64gcv -std=gnu99
#FOPTIMIZE      = -O3 -fno-strict-aliasing -mabi=lp64d -march=rv64gcv -std=gnu99


COPTIMIZE      = -O3 -fno-strict-aliasing -flto -ffp-contract=off 
CXXOPTIMIZE    = -O3 -fno-strict-aliasing -flto -ffp-contract=off
FOPTIMIZE      = -O3 -fno-strict-aliasing -flto -ffp-contract=off

#default=base=default=default:
#COPTIMIZE      = -O3 -fno-strict-aliasing -Wno-deprecated-non-prototype -flto -ffp-contract=off
#CXXOPTIMIZE    = -O3 -fno-strict-aliasing -flto -ffp-contract=off                                                                                            
#FOPTIMIZE      = -O3 -fno-strict-aliasing -flto -ffp-contract=off

#####################################################################
# 32/64 bit Portability Flags - all
#####################################################################

default=base=default=default:
PORTABILITY    = -DSPEC_CPU_LP64

#####################################################################
# Portability Flags
#####################################################################

400.perlbench=default=default=default:
CPORTABILITY   = -DSPEC_CPU_LINUX_X64 -std=gnu89 

416.gamess=default=default=default:
CPORTABILITY   =  -funconstrained-commons
FPORTABILITY   = -std=legacy -funconstrained-commons -fno-strict-aliasing

462.libquantum=default=default=default:
CPORTABILITY   =  -DSPEC_CPU_LINUX

464.h264ref=default=default=default:
CPORTABILITY   =  -fsigned-char

482.sphinx3=default=default=default:
CPORTABILITY   =  -fsigned-char

483.xalancbmk=default=default=default:
CXXPORTABILITY = -DSPEC_CPU_LINUX -include cstring

481.wrf=default=default=default:
CPORTABILITY   = -DSPEC_CPU_CASE_FLAG -DSPEC_CPU_LINUX
FPORTABILITY   = -fallow-argument-mismatch

447.dealII=default=default=default:
CXXPORTABILITY = -fpermissive -include cstring

450.soplex=default=default=default:
CXXPORTABILITY = -std=gnu++98
```

    3. 对应修改编译器也可以生成x86.cfg,编译x86版本的， CC  = gcc
    4. source ./shrc
    5. runspec --config=x86.cfg --size=test --noreportable --tune=base --iterations=1 libquantum
    6. 具体命令行含义参考官网[runspec (CPU2006)](https://www.spec.org/cpu2006/Docs/runspec.html#about_config)
    7. 可以在对应的运行目录下 benchspec/CPU2006/462.libquantum/run/run_base_test，查看具体的speccmds.cmd 获取对应的运行指令
    8. x86 版本直接 ./libquantum 33 5
    9. riscv 版本需要套一层qemu 运行 qemu-riscv64  ./libquantum 33 5
    10. qemu-riscv64 版本可以直接拉取官网的qemu,  ../configure --target-list=riscv64-linux-user 编译用户态qemu
2. 另一种编译环境： https://github.com/OpenXiangShan/CPU2006LiteWrapper  
    1. 会把源代码copy 到其中
    2. 按照对应的编译选项编译和运行，封装的更完整，但是还是建议完整的看看spec 官网过程，以及程序比对过程



### perf 分析
1. 目前只能分析x86 版本的程序，如果perf qemu-riscv64 ./libquantum; 套上qemu 来分析，由于qemu已经进行指令翻译了，只能看到code cache中翻译后的代码，而不是原始的热点代码，并且qemu 自己执行一些代码会影响原始程序语义
2. 注意只能在105号机器（172.19.20.105）可以使用perf
3. 编译出x86 版本的spec 之后，_./libquantum_base.x86-gcc11.4.0-o3 400 5 _可以适当调整输入保证运行时间在1分钟左右
4. _perf record -g ./libquantum_base.x86-gcc11.4.0-o3 400 5_ , 运行一次程序，会生成perf.data, -g 选项能查看函数调用关系
5. _perf report _查看热点时间
6. 建议在x86.cfg  添加 -g 编译选项再进行编译，方便perf report能查看函数C 代码和对应的汇编代码
7. ![](images/1724062753411-4330cdd9-70fc-4fd2-b336-3a4c67cc3b51.png)
8. 发现54%时间都是toffoli 函数
9. ![](images/1724062800673-fd2804b4-a936-4775-8e7a-a60b2d991d80.png)
10. 进入toffoli 函数可以看到基本再执行for 循环。
11. 具体分析结论可以参考[libquantum 分析](https://bosc.yuque.com/yny0gi/sggyey/bd9aa3ltpx7fedrh) 文章



### qemu plugin 分析
1. git clone  [jensen-yan/qemu_plugins (github.com)](https://github.com/jensen-yan/qemu_plugins)
2. 注意这个版本的插件支持qemu 8.X.X 或者更早的， 不支持最新的qemu 9.X(9.X 的插件API 更新了很多，需要scoreboard ， 写着好麻烦，还是用之前的吧）
3. make all
4. _qemu-riscv64  -plugin ./libtbstat.so -d plugin /nfs/home/yanyue/tools/CPU2006LiteWrapper/cpu2006_build/libquantum_base.gcc 143 8_
5. 套上对应的qemu 插件来运行某个spec 程序，能把最常出现的基本块打印到top_blocks_with_instructions.txt 文件中

```cpp
Total Dynamic Instructions: 11178713121

Top 10 Most Executed Basic Blocks with Instructions:

Basic Block at 0x12caa executed 682077872 times
Instructions:
  0x12caa: 00471793          slli                    a5,a4,4
  0x12cae: 97aa              add                     a5,a5,a0
  0x12cb0: 6794              ld                      a3,8(a5)
  0x12cb2: 0086f633          and                     a2,a3,s0
  0x12cb6: 02860e63          beq                     a2,s0,60                # 0x12cf2

Basic Block at 0x12aac executed 122812878 times
Instructions:
  0x12aac: 0685              addi                    a3,a3,1
  0x12aae: 0006879b          sext.w                  a5,a3
  0x12ab2: fd07c3e3          bgt                     a6,a5,-58               # 0x12a78
```

6. 具体分析可以参考[libquantum 分析](https://bosc.yuque.com/yny0gi/sggyey/bd9aa3ltpx7fedrh) 文章

### Gem5 commitTrace 分析
前两个方法都只能分析全局程序，如果想要分析某个切片的热点路径怎么办？

那就只能用gem5 运行一下这个切片，打出所有的指令trace, 然后用脚本分析

例如发现646号切片是libquantum 占比最大的切片（权重24%），

同时注意gem5 是需要先预热的，然后再运行这个切片，所以需要打印预热后程序trace



1. ._/build/RISCV/gem5.opt configs/example/xiangshan.py  --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/64624/_64624_0.243160_.zstd _
2. 首先完整运行一次646切片，查看m5out/stats.txt

```cpp
---------- Begin Simulation Statistics ----------
simSeconds                                   0.001129                       # Number of seconds simulated (Second)
simTicks                                   1129320549                       # Number of ticks simulated (Tick)
finalTick                                  1129320549 

---------- End Simulation Statistics   ----------
上方为预热2千万条指令，后方为真正执行2千万条指令
---------- Begin Simulation Statistics ----------
simSeconds                                   0.001395                       # Number of seconds simulated (Second)
simTicks                                   1395278991                       # Number of ticks simulated (Tick)
finalTick                                  2524599540                       # Number of ticks from beginning of simulation (restored from checkpoints and never reset) (Tick)

```

3. 发现预热花了11亿拍，执行用了13亿拍，总共用了25亿拍

```bash
./build/RISCV/gem5.opt --debug-start=1300000000 --debug-file=libquantum.commitTrace8_646.less.gz --debug-flags=CommitTrace configs/example/xiangshan.py --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/64624/_64624_0.243160_.zstd
```

4. 让大概在13亿拍再开始打印debug 信息，打印CommitTrace， 输出到.gz 文件（很大，大概200M, 用vim 打开阅读）

```python
import re
from collections import Counter
import sys
import gzip

def parse_instruction(line):
    match = re.search(r'pc:(0x[0-9a-f]+).*CompleT:\d+,\s+(.*?)(?:,\s+(?:res:|paddr:)|$)', line)
    if match:
        return match.groups()   # pc + instruction
    return None

def is_branch_instruction(instr):
    branch_instructions = ['beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'beqz', 'bnez', 'j', 'jal', 'jalr', 'ret']
    branch_instructions += ['c_beqz', 'c_bnez', 'c_j', 'c_jal', 'c_jr', 'c_jalr']
    return any(instr.split()[0].startswith(branch) for branch in branch_instructions)

def analyze_commit_trace(file_path):
    basic_blocks = Counter()
    current_block = []

    open_func = gzip.open if file_path.endswith('.gz') else open
    mode = 'rt' if file_path.endswith('.gz') else 'r'

    with open_func(file_path, mode) as file:
        for line in file:
            if 'global: system.cpu' in line:
                parsed = parse_instruction(line)
                if parsed:
                    current_block.append(parsed)
                    if is_branch_instruction(parsed[1]):
                        if current_block:
                            # print(current_block)
                            basic_blocks[tuple(current_block)] += 1
                            current_block = []
            else:
                if current_block:
                    basic_blocks[tuple(current_block)] += 1
                    current_block = []

        if current_block:
            basic_blocks[tuple(current_block)] += 1

    return basic_blocks

def main(file_path):
    basic_blocks = analyze_commit_trace(file_path)

    print("Top 10 most common basic blocks:")
    for block, count in basic_blocks.most_common(10):
        print(f"Count: {count}")
        print("Instructions:")
        for pc, instr in block:
            print(f"  {pc}: {instr}")
        print()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Please provide the file path as a command line argument.")
        sys.exit(1)
    file_path = sys.argv[1]
    main(file_path)
```

5. 可以用上方的trace_bb.py 处理一下commitTrace, 能打印这个切片最常出现的基本块
6. python3 trace_bb.py libquantum.commitTrace8_646.less.gz > libquantum.commitTrace8_646.bb

```python
Top 10 most common basic blocks:
Count: 671958
Instructions:
  0x12aac: c_addi a3, 1
  0x12aae: addiw a5, a3, 0
  0x12ab2: blt a5, a6, -58
```

7. 能发现这样统计出来的基本块，和qemu 统计的基本块是相同过的！前提是编译选项要一致（gcc 12 -O3), 连地址都完全相同，只是指令名字略有差异，并且指令占比都是一致的



### 结论
通过以上三种方法应该能较好的分析spec 程序的热点代码

![](images/1724064346037-483aa4b2-f7e3-4010-8db0-d1d5ecc6c9a8.png)

用

[GitHub - shinezyy/gem5_data_proc: data preprocessing scripts for gem5 output](https://github.com/shinezyy/gem5_data_proc)

还能统计更多spec 程序在运行切片时候的top down 信息以及分数信息，已经有相关文档，这里就暂时不写了。

