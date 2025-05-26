

### 介绍
目前riscv zicond 扩展指令集只有两条指令

```c
czero.eqz rd, rs1, rs2
    rd = (rs2 == 0) ? 0 : rs1
    
czero.nez rd, rs1, rs2
    rd = (rs2 != 0) ? 0 : rs1

// 扩展: 条件加法
czero.nez  rd, rs2, rc
add        rd, rs1, rd
rd = (rc == 0) ? rs1 + rs2 : rs2
```



### 编译器支持
1. gcc:

需要在riscv-gnu-toolchain 中添加 zicond

```c
./configure --prefix=/nfs/home/yanyue/tools/RISCV  --disable-multilib --with-arch=rv64gcv_zicond_zba_zbb_zbc_zbs
make linux -j200

riscv64-unknown-linux-gnu-gcc -v
--with-arch=rv64gc_zicond
```

编译参数选择

MARCH ?= rv64gc_zicond_zba_zbb_zbc_zbs_zbkb_zbkc_zbkx_zknd_zkne_zknh_zkr_zksed_zksh_zkt

最后在coremark objdump 中发现找不到czero 指令

只有嵌入式汇编强行写入的有这个指令

```c
int64_t test_czero_eqz(int64_t a, int64_t b) {
    int64_t result;
    asm ("czero.eqz %0, %1, %2" : "=r"(result) : "r"(b), "r"(a));
    return result;
}

int res = test_czero_eqz(0, 42);	// 0
```



2. llvm:

同样在riscv-gnu-toolchain

```bash
./configure --prefix=$RISCV --enable-llvm --enable-linux --with-arch=rv64gcv_zicond_zba_zbb_zbc_zbs --with-abi=lp64d
make all -j200	// clone llvm 会比较慢

clang -v --target=riscv64-unknown-linux-gnu \
      --gcc-toolchain=$RISCV \
      --sysroot=$RISCV/sysroot \
      -I$RISCV/sysroot/usr/include \
      -march=rv64gc_zicond \
      -O2 zicond_test.c -o zicond_test_llvm --static
qemu-riscv64 zicond_test_llvm	// 成功运行

// 更简单的编译方式，这样和gcc 方式很类似
riscv64-unknown-linux-gnu-clang -march=rv64gc_zicond \
      -O2 zicond_test.c -o zicond_test_llvm --static
```

尽量使用riscv64-unknown-linux-gnu-clang， 而不要用clang --target=riscv64-unknown-linux-gnu!

### coremark测试
问题：

用qemu-system-riscv64 运行coremark 会卡死在czero.eqz  中，可能qemu版本太老，能升级吗

```bash
make ARCH=riscv64-xs ITERATIONS=10	// 参数传递迭代次数
qemu-system-riscv64 -d in_asm -M nemu -bios ./build/coremark-riscv64-xs.elf -nographic -m 8G -smp 1 -cpu rv64,v=true,vlen=128,h=false,sv39=true,sv48=false,sv57=false,sv64=false
```

切换到高版本9.0.0

```bash
git fetch
git checkout 9.0.0_checkpoint
cd build
../configure --target-list=riscv64-softmmu --enable-debug --enable-zstd --enable-plugins
make -j200
```

但还是卡住，可以尝试用nemu或者spike试试

需要加上zicond=true, 这样就能跑了！

```bash
qemu-system-riscv64 -d in_asm -M nemu -bios ./build/coremark-riscv64-xs.elf -nographic -m 8G -smp 1 -cpu rv64,zicond=true,v=true,vlen=128,h=false,sv39=true,sv48=false,sv57=false,sv64=false
```



目前gem5 fs 跑hello 需要1分钟，跑coremark N=1 在测试中，1分钟跑完

需要关闭debug-flags! 快很多！

很好，gem5 也需要使用nemu-ref-so ， spike-so 似乎有问题，现在都能正常运行带有zicond指令的程序了，包括gem5和qemu, qemu-system(xs)



现在

1. 如何让gcc 生成更多zicond指令
2. 试试clang能否生成zicond指令

确实发现clang更有可能生成zicond指令，现在是如何用clang来编译coremark

可以先试试hello



目前能成功编译hello以及coremark了，但是仍然不生成czero指令啊



### spec2006测试
直接编译spec 2006吧

```bash
COPTIMIZE      = -O3 -fno-strict-aliasing -flto -ffp-contract=off -march=rv64gc_zicond
CXXOPTIMIZE    = -O3 -fno-strict-aliasing -flto -ffp-contract=off -march=rv64gc_zicond

CC  = riscv64-unknown-linux-gnu-clang -static
CXX = riscv64-unknown-linux-gnu-clang++ -static
FC  = riscv64-unknown-linux-gnu-gfortran -static	// gfortran 可能还得改

runspec --action=build --config=riscv_llvm.cfg --size=test --noreportable --tune=base --iterations=1 int

Build errors: 401.bzip2(base), 456.hmmer(base), 473.astar(base), 483.xalancbmk(base)
Build successes: 400.perlbench(base), 403.gcc(base), 429.mcf(base), 445.gobmk(base), 458.sjeng(base), 462.libquantum(base), 464.h264ref(base), 471.omnetpp(base), 999.specrand(base)


看看perlbench
cat ../run_base_test_x86-gcc11.4.0-o3.0000/speccmds.cmd 
-C /nfs/home/yanyue/tools/cpu2006v99/benchspec/CPU2006/400.perlbench/run/run_base_test_x86-gcc11.4.0-o3.0000
-o attrs.out -e attrs.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib attrs.pl
-o gv.out -e gv.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib gv.pl
-o makerand.out -e makerand.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib makerand.pl
-o pack.out -e pack.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib pack.pl
-o redef.out -e redef.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib redef.pl
-o ref.out -e ref.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib ref.pl
-o regmesg.out -e regmesg.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib regmesg.pl
-o test.out -e test.err ../run_base_test_x86-gcc11.4.0-o3.0000/perlbench_base.x86-gcc11.4.0-o3 -I. -I./lib test.pl
```



尝试用qemu-plugin 来统计指令数目占比

修改qemu 插件，用9.0版本的新的API, 便于管理多线程情况下的计数器

```cpp
// 法一，一个scoreboard 有多个值
typedef struct {
    uint64_t bb_count;
    uint64_t insn_count;
} CPUCount;
static struct qemu_plugin_scoreboard *counts;
static qemu_plugin_u64 bb_count;
static qemu_plugin_u64 insn_count;

struct qemu_plugin_scoreboard *scoreboard = qemu_plugin_scoreboard_new(sizeof(YourStructType));	// 定义
qemu_plugin_u64 field = qemu_plugin_scoreboard_u64_in_struct(scoreboard, YourStructType, field_name);	// 内部具体值
YourStructType *data = qemu_plugin_scoreboard_find(scoreboard, cpu_index); // 查找特定CPU
qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, field, value);		// 内联++ or 特定exec 函数++
uint64_t total = qemu_plugin_u64_sum(field);  // 计算总和

// 法二， 单独一个值就是一个scoreboard
static qemu_plugin_u64 insn_count;
insn_count = qemu_plugin_scoreboard_u64(
        qemu_plugin_scoreboard_new(sizeof(uint64_t)));
```

```cpp
qemu-riscv64 -plugin /nfs/home/yanyue/tools/qemu/tests/plugin/libinsn_count.so -d plugin ./perlbench_base.rv64gcb_zicond-llvm -I. -I./lib attrs.pl

Total instructions: 13734097
czero.eqz instructions: 27044
czero.nez instructions: 19430  
统计perlbench，总指令数目1千万，cero指令4万，占比千分之4
```

用CPULiteWrapper 来编译，只有astar, xalancbmk 有问题

并且只能用llvm 才能编译出来，gcc 完全编译不出来！

目前看几乎所有的spec程序的指令数目占比都在万分之一到百分之1左右，占比还是非常小啊

只有hmmer比较高，在6%，两个加起来有13%！

```bash
make run-int-test   
echo "Running test on 400.perlbench"
Running test on 400.perlbench
echo "Running test on 401.bzip2"
Running test on 401.bzip2
Total instructions: 11849220800
czero.eqz instructions: 35155045  0.002
czero.nez instructions: 57070143
Total instructions: 23978169260
czero.eqz instructions: 143520593  0.005
czero.nez instructions: 250428999
echo "Running test on 403.gcc"
Running test on 403.gcc
Total instructions: 5101063458
czero.eqz instructions: 15795514   0.003
czero.nez instructions: 17344216
echo "Running test on 429.mcf"
Running test on 429.mcf
Total instructions: 3222414561
czero.eqz instructions: 4234487    0.0013
czero.nez instructions: 3960885
echo "Running test on 445.gobmk"
Running test on 445.gobmk
Total instructions: 400003139
czero.eqz instructions: 289188    0.0007
czero.nez instructions: 304888
Total instructions: 3857967804
czero.eqz instructions: 45469624  0.011
czero.nez instructions: 45965363
Total instructions: 120127100
czero.eqz instructions: 426357   0.0035
czero.nez instructions: 441372
Total instructions: 15433424767
czero.eqz instructions: 173868941  0.011
czero.nez instructions: 177076843

echo "Running test on 456.hmmer"
Running test on 456.hmmer
Total instructions: 20559288545
czero.eqz instructions: 1395891105  6%
czero.nez instructions: 1395891105
```

统计一下x86程序吧，看看-march=zen4 可能会多生成一些谓词指令

神奇，发现x86程序的cmov指令也非常少，挺少的，对性能影响真的大吗，平均也在0.01%到2.5%(只有一个）

这里我的编译参数没有变化，都还是默认的参数，要不要跑一下ref程序？



使用nix 安装的gcc13 仍然比例位0.01% 到 1.8%， llvm18测试相似

```bash
let
  pkgs = import <nixpkgs> {};
in
pkgs.mkShell {
    buildInputs  = with pkgs; [
            glibc
            glibc.static
            gcc
            glibc.dev
            binutils
            pkg-config	# 需要它来找到对应的glibc库！
            file
            # for llvm
            llvmPackages_latest.clang
            llvmPackages_latest.libclang
            llvmPackages_latest.llvm
        ];
    }

make ARCH=x86_64 OPTIMIZE="-O3 -flto" SUBPROCESS_NUM=5 build_int_456.hmmer -j 100
```

hummer 最高8%！继续测试-O3 优化 -flto 优化后的程序吧，还是hummer 最高8%

```bash
make ARCH=x86_64 OPTIMIZE="-O3 -flto" SUBPROCESS_NUM=20 build-int -j 100
export ARCH=x86_64
```



想看看hummer 为何这么多，核心基本块是什么

再看看gcc 10 试试，cmov 只有2.5%，基本还是用的cmp

```bash
make -s -C 456.hmmer CROSS_COMPILE=riscv64-unknown-linux-gnu- -n
测试
```



### hmmer 分析
参考[hmmer 分析](https://bosc.yuque.com/yny0gi/sggyey/yuq36u0uf0p848k1)

最后再试试clang编译吧， llvm 也会有大约6%

```c
Total instructions: 14065656383
czero.eqz instructions: 0
czero.nez instructions: 0
cmov instructions: 862977549
cmov / total instructions: 6.14%
```

但是，如果gcc 13 -march=znver4 之后，不会生成cmov指令，而是vmov 指令，有自动向量化了

```c
if ((sc = dpp[k-1] + tpdm[k-1]) > mc[k])  mc[k] = sc;                                                                                                                                                          ▒
  1.12 │        vmovd         (%r8,%rdx,4),%xmm18                                                                                                                                                                            ▒
  1.17 │        vmovd         (%r15,%rdx,4),%xmm17                                                                                                                                                                           ▒
  1.15 │        vpaddd        %xmm18,%xmm17,%xmm17                                                                                                                                                                           ▒
       │      if ((sc = xmb  + bp[k])         > mc[k])  mc[k] = sc;                                                                                                                                                          ▒
       │      mc[k] += ms[k];                                                                                                                                                                                                ◆
  1.26 │        vmovq         %xmm5,%r8                                                                                                                                                                                      ▒
       │      if ((sc = dpp[k-1] + tpdm[k-1]) > mc[k])  mc[k] = sc;                                                                                                                                                          ▒
  1.05 │        vpmaxsd       %xmm0,%xmm17,%xmm17                                                                                                                                                                            ▒
  1.26 │        vmovd         %xmm17,(%rcx) 
```



### 结论
现在总结一下

1. gcc 10 几乎不会生成cmov 指令
2. gcc 13 只有hmmer 程序多，大约8%， 无论是否加上-march=native 都是这么多，加上-march=znver4之后会做自动向量化，产生vmov指令
3. llvm 18编译之后，hmmer 6%，其他程序仍然非常低
4. riscv 架构用llvm 编译运行test 测试，hmmer  13%, sjeng 6%, 其他都特别少



### sjeng 程序看看吧
参考[sjeng](https://bosc.yuque.com/yny0gi/sggyey/zbqg3zx88pnefyxv)



 /nfs/share/riscv-toolchain-gcc15-240613-noFFnoSeg2  

这里是带有V 扩展的，需要添加来进行编译

```bash
LDFLAGS  += -march=rv64gcv_zicond_zba_zbb_zbc_zbs
CFLAGS   += -march=rv64gcv_zicond_zba_zbb_zbc_zbs
```

下一步对sjeng 来进一步的分析并且



我发现现在常常需要重新编译多次，一会在编译x86, 一会再编译riscv, 并且每次的编译变量都不太一样，有时候在用perf运行，有时候在用qemu运行，都需要修改参数然后重新编译，最后再看看吧，sjeng

最好还是按照qemu那种方式，每次build，生成一个带有时间戳的二进制，但这样还要改运行的逻辑，需要选择最近时间戳的程序来运行，还是有点麻烦



我现在在想，假设sjeng 的分数还有提升空间，取决于以下几个前提

1. sjeng ref 中使用llvm 编译会产生大约5%的cero 指令
2. 目前处理器对于条件指令支持较差，一旦运行cero指令就会加剧指令依赖，导致运行cmov指令时候出现很多问题
3. 假设使用值预测能优化很多条件指令，并且前提条件是
    1. 原本分支预测做的不好
    2. 现在用条件指令后，直接导致



算了，我先生成一个切片来试试吧



```bash
[    4.040419] sjeng[30]: unhandled signal 4 code 0x1 at 0x000000000001d9b0 in sjeng[d9b0,10000+92000]
[    4.041031] CPU: 0 PID: 30 Comm: sjeng Not tainted 6.10.2 #1
[    4.041365] Hardware name: freechips,rocketchip-unknown (DT)
[    4.041671] epc : 000000000001d9b0 ra : 000000000001d96a sp : 0000003fdb40ff70
[    4.042025]  gp : 00000000000a81b8 tp : 0000000038b01760 t0 : 0000000000319914
[    4.042389]  t1 : 0000000000000012 t2 : 0000000000000000 s0 : 000000000010be00
[    4.042742]  s1 : 000000000010be00 a0 : 0000000000000000 a1 : 00000000000dbba1
[    4.043088]  a2 : 00000000000000f5 a3 : 0000000000000000 a4 : 0000000000000000
[    4.043440]  a5 : fffffffffbad2887 a6 : 00000000003199fc a7 : 0000000000000040
[    4.043797]  s2 : 0000000000001c20 s3 : 000000000010a1e0 s4 : 0000000000000000
[    4.044153]  s5 : 000000000010a000 s6 : 0000000000000020 s7 : 0000003fdb413970
[    4.044514]  s8 : 00000000000f4240 s9 : 0000000000000023 s10: 00000000000eb008
[    4.044865]  s11: 0000000000000000 t3 : 0000000000000000 t4 : 0000000000000007
[    4.045212]  t5 : 0000000000000073 t6 : 0000000000000068
[    4.045489] status: 8000000200006620 badaddr: 000000009e803757 cause: 0000000000000002
[    4.045902] Code: 8d4d 8905 8dd2 3757 9e80 07e3 c405 a8b9 8905 8dd2 (3757) 9e80 
Illegal instruction

在运行切片时候报错


```



似乎有两个切片成功了，就是

```bash
2.8G    spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-06-11-37
2.7G    spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-06-11-40
```

/nfs/home/yanyue/workspace/checkpoint_scripts/checkpoint_scripts/archive/spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-06-11-40/

那么接下来来用gem5 来运行一下试试吧



现在用我的nemu，以及对应的gcbv_restorer

```bash
Obtained ref_so from GCBV_REF_SO:  /nfs/home/yanyue/tools/gem5-tools/riscv64-nemu-interpreter-so
Obtained gcpt_restorer from GCB_RESTORER:  /nfs/home/yanyue/tools/gem5-tools/zyy/gcb-restorer.bin
**** REAL SIMULATION ****
build/RISCV/sim/simulate.cc:194: info: Entering event queue @ 0.  Starting simulation...
  61938: global: system.cpu [sn:1 pc:0x80000000] enDqT: 59940, exDqT: 60273, readyT: 60273, CompleT:61272, lui s0, 128, res: 0x80000
build/RISCV/cpu/base.cc:1277: warn: Start memcpy to NEMU from 0x7631bc100000, size=8589934592
build/RISCV/cpu/base.cc:1280: warn: Start regcpy to NEMU
  62271: global: system.cpu [sn:2 pc:0x80000004] enDqT: 59940, exDqT: 60273, readyT: 60606, CompleT:61605, addiw s0, s0, 1, res: 0x80001
  62604: global: system.cpu [sn:3 pc:0x80000008] enDqT: 59940, exDqT: 60273, readyT: 60939, CompleT:61938, slli s0, s0, 12, res: 0x80001000
  62937: global: system.cpu [sn:4 pc:0x8000000c] enDqT: 59940, exDqT: 60273, readyT: 61272, CompleT:62271, addi s0, s0, -256, res: 0x80000f00
 120213: global: system.cpu [sn:5 pc:0x80000010] enDqT: 59940, exDqT: 60273, readyT: 61605, CompleT:119547, ld t1, 0(s0), res: 0, paddr: 0x80000f00
 120213: global: system.cpu [sn:6 pc:0x80000014] enDqT: 59940, exDqT: 60606, readyT: 60606, CompleT:61605, lui t2, 12, res: 0xc000
 120213: global: system.cpu [sn:7 pc:0x80000018] enDqT: 60273, exDqT: 60606, readyT: 60939, CompleT:61938, addiw t2, t2, -273, res: 0xbeef
 121545: global: system.cpu [sn:8 pc:0x8000001c] enDqT: 60273, exDqT: 60606, readyT: 62603, CompleT:120879, beq t1, t2, 8
 121545: global: system.cpu [sn:9 pc:0x80000020] enDqT: 60273, exDqT: 60606, readyT: 60606, CompleT:61605, jal zero, 655328
 132867: system.cpu.commit: [sn:17 pc:0x800a0000] c_addi4spn s0, sp, 0 has a fault, mepc: 0, mcause: 0, mtval: 0
build/RISCV/cpu/o3/fetch.cc:838: warn: Address 0 is outside of physical memory, stopping fetch, 139527
build/RISCV/cpu/o3/commit.cc:710: warn: cpu may be stucked
build/RISCV/cpu/o3/commit.cc:708: panic: cpu stucked!!
Memory Usage: 17212580 KBytes
Program aborted at tick 6782211
[1]    3948688 IOT instruction (core dumped)  /nfs/home/yanyue/workspace/GEM5/build/RISCV/gem5.opt --debug-flags=CommitTrac 
```

大概能执行10条指令，然后遇到

原来用脚本生成的切片现在又不要用restorer覆盖了



包含zicond指令的切片

spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-06-11-40

终于算出分数了，默认六发射，居然分数还更高了一点

sjeng 37的分数相同

```bash
================ Int =================
          time  ref_time   score  coverage
sjeng  350.139   12100.0  11.519       1.0

原本的6发射
sjeng	381.382	12100	10.576
```



接下来我重新编译了sjeng，保证它不含sjeng指令，然后再次切片，来计算一下分数吧

同时估计切片还得10h左右，等中秋之后再来看吧，快乐！



现在spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-14-17-51  17-51 对应生成了三次切片的sjeng，无zicond指令

```bash
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` /nfs/home/yanyue/workspace/checkpoint_scripts/checkpoint_scripts/archive/spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-14-17-51/checkpoint-0-0-0/checkpoint.lst  /nfs/home/yanyue/workspace/checkpoint_scripts/checkpoint_scripts/archive/spec06_llvm18.1.8_rv64gcbv_zicond_base_intFppOff_for_qemu_dual_core_NEMU_archgroup_2024-09-14-17-51/checkpoint-0-0-0/sjeng sjeng_nozicond0
```

现在用这个程序来运行，大约需要半小时吧，11点半来看看

```bash
================ Int =================
          time  ref_time   score  coverage
sjeng  373.658   12100.0  10.794       1.0
sjeng  372.312   12100.0  10.833       1.0
不开zicond，10.8分，开了zicond，11.5分
大约提升了6%
看看主要的cachemiss 这些有没有变化，对比一下吧

```

性能提升了6%

指令数目 也大概差了1.8%左右

zicond    2481912253379

nozicond 2528581702873





| | <font style="color:#000000;">Atomic</font> | <font style="color:#000000;">BpStall</font> | <font style="color:#000000;">CommitSquash</font> | <font style="color:#000000;">Cycles</font> | <font style="color:#000000;">DTlbStall</font> | <font style="color:#000000;">FetchBufferInvalid</font> | <font style="color:#000000;">FragStall</font> | <font style="color:#000000;">ITlbStall</font> | <font style="color:#000000;">IcacheStall</font> | <font style="color:#000000;">InstMisPred</font> | <font style="color:#000000;">InstNotReady</font> | <font style="color:#000000;">InstSquashed</font> | <font style="color:#000000;">Insts</font> | <font style="color:#000000;">IntStall</font> | <font style="color:#000000;">LoadL1Bound</font> | <font style="color:#000000;">LoadL2Bound</font> | <font style="color:#000000;">LoadL3Bound</font> | <font style="color:#000000;">LoadMemBound</font> | <font style="color:#000000;">MemCommitRateLimit</font> | <font style="color:#000000;">MemNotReady</font> | <font style="color:#000000;">MemSquashed</font> | <font style="color:#000000;">NoStall</font> | <font style="color:#000000;">OtherFetchStall</font> | <font style="color:#000000;">OtherMemStall</font> | <font style="color:#000000;">OtherStall</font> | <font style="color:#000000;">ResumeUnblock</font> | <font style="color:#000000;">SerializeStall</font> | <font style="color:#000000;">SquashStall</font> | <font style="color:#000000;">StoreL1Bound</font> | <font style="color:#000000;">StoreL2Bound</font> | <font style="color:#000000;">StoreL3Bound</font> | <font style="color:#000000;">StoreMemBound</font> | <font style="color:#000000;">TrapStall</font> | <font style="color:#000000;">cpi</font> | <font style="color:#000000;">coverage</font> |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| <font style="color:#000000;">sjeng_zicond</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">8.07E+05</font> | <font style="color:#000000;">8.27E+06</font> | <font style="color:#000000;">8.46E+06</font> | <font style="color:#000000;">3.76E+05</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">6.48E+06</font> | <font style="color:#000000;">1.68E+02</font> | <font style="color:#000000;">2.95E+06</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">3.23E+02</font> | <font style="color:#000000;">1.48E+06</font> | <font style="color:#000000;">2.00E+07</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">5.07E+04</font> | <font style="color:#000000;">6.22E+04</font> | <font style="color:#000000;">2.20E+04</font> | <font style="color:#000000;">3.35E+06</font> | <font style="color:#000000;">1.63E+01</font> | <font style="color:#000000;">4.26E+05</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">2.57E+07</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">5.18E+03</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">6.20E+01</font> | <font style="color:#000000;">3.07E+05</font> | <font style="color:#000000;">9.25E+04</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">4.23E-01</font> | <font style="color:#000000;">1.00E+00</font> |
| <font style="color:#000000;">sjeng_nozicond</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">9.70E+05</font> | <font style="color:#000000;">9.40E+06</font> | <font style="color:#000000;">8.87E+06</font> | <font style="color:#000000;">3.58E+05</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">6.92E+06</font> | <font style="color:#000000;">9.93E+01</font> | <font style="color:#000000;">3.47E+06</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">5.57E+02</font> | <font style="color:#000000;">1.78E+06</font> | <font style="color:#000000;">2.00E+07</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">7.36E+04</font> | <font style="color:#000000;">6.22E+04</font> | <font style="color:#000000;">1.92E+04</font> | <font style="color:#000000;">3.22E+06</font> | <font style="color:#000000;">2.07E-01</font> | <font style="color:#000000;">4.45E+05</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">2.58E+07</font> | <font style="color:#000000;">6.95E-02</font> | <font style="color:#000000;">9.59E+02</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">2.55E+01</font> | <font style="color:#000000;">3.61E+05</font> | <font style="color:#000000;">7.03E+04</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">0.00E+00</font> | <font style="color:#000000;">8.78E-01</font> | <font style="color:#000000;">4.43E-01</font> | <font style="color:#000000;">1.00E+00</font> |


发现其实也没有差距太大，奇怪

看看MPKI 有没有区别

L1Icache MPKI 从无zicond 的0.437 下降到有zicond 的0.24！导致topdown 的icache stall变化

其余各级cache miss 没有差别

| | score | insts | L1I.MPKI | ipc | icache_stall |
| --- | --- | --- | --- | --- | --- |
| zicond | 11.519 | 2481912253379 | 0.24 | 2.38 | <font style="color:#000000;">2.95E+06</font> |
| nozicond | 10.794 | 2528581702873 | 0.437 | 2.27 | <font style="color:#000000;">3.47E+06</font> |
| diff | 6% | 1.8% | 50% |  |  |


均为6发射默认配置

