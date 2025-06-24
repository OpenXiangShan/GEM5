# ftb 小测试

#### 源代码说明
程序位置在

https://github.com/OpenXiangShan/nexus-am/blob/master/tests/frontendtest/tests/tage1.c

```cpp
#include "common.h"

void __attribute__ ((noinline)) branch_test(int cnt) {
    int tmp = 0;
    // 设计交替的分支模式：
    // 通过判断t2的奇偶性来实现T-NT交替
    #define ONE \
        "andi t4, t2, 1\n\t"     /* 检查t2是否为奇数 */ \
        "beqz t4, 2f\n\t"        /* 如果是偶数则跳转 */ \
        "nop\n\t" \
        "2:\n\t"

    asm volatile(
        // 初始化计数器
        "li t0, 0\n\t"      // 循环计数器
        "li t2, 0\n\t"      // 用于产生交替模式的计数器
        ".align 4\n\t"
        "1:\n\t"
        // 重复执行分支指令
        TEN                  // 每次循环执行10次分支指令
        "addi t2, t2, 1\n\t" // t2每次加1，产生0,1,2,3...序列
        "addi t0, t0, 1\n\t" // 增加循环计数器
        "blt t0, %1, 1b\n\t" // 循环控制
        : "+r"(tmp)
        : "r"(cnt)
        : "t0", "t2", "t3", "t4", "memory"
    );
}

int main() {
    branch_test(1000);  // 执行1000次循环
    return 0;
}

// 这种情况下，base 表几乎都完全命中，导致几乎不会分配新的tage 表项
```



```cpp
主要逻辑：
1. 外层循环执行1000次(cnt=1000)
2. 每次循环中：
   - 执行10次相同的分支指令(TEN = ONE * 10)
   - t2计数器每次加1
   - t0作为循环计数器

ONE宏的分支逻辑：
1. andi t4, t2, 1    // 取t2的最低位判断奇偶性
2. beqz t4, 2f       // 如果t4为0(t2为偶数)则跳转到2标签
3. nop              // 延迟槽指令
4. 2:               // 跳转目标

t2的变化：0,1,2,3,4,5,6,7...
对应的跳转行为：
- t4=0 (偶数): 跳转(Taken)
- t4=1 (奇数): 不跳转(Not Taken) 
- t4=2 (偶数): 跳转(Taken)
- t4=3 (奇数): 不跳转(Not Taken)
...

形成规律：T-NT-T-NT-T-NT...的交替模式
```



#### 反汇编
/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/tage1-riscv64-xs.txt

```cpp
    80000130:	0013fe93          	and	t4,t2,1
    80000134:	000e8363          	beqz	t4,8000013a <branch_test+0x1a>
    80000138:	0001                	nop
    8000013a:	0013fe93          	and	t4,t2,1
    8000013e:	000e8363          	beqz	t4,80000144 <branch_test+0x24>
    80000142:	0001                	nop
    80000144:	0013fe93          	and	t4,t2,1
    80000148:	000e8363          	beqz	t4,8000014e <branch_test+0x2e>
    8000014c:	0001                	nop
```

主要关注130 是起始地址

134 是第一个br, 其中为T-NT-T-NT 交错执行

如果T, target = 13a; NT target= 138

#### 测试输出
首先看CommitTrace 查看小测试执行情况是否如预期， 这表示程序最终commit 顺序

```cpp
./build/RISCV/gem5.debug  --outdir=debug/tage1 --debug-flags=CommitTrace --debug-file=tage1.commit ./configs/example/xiangshan.py --generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/tage1-riscv64-xs.bin --raw-cpt
```

```cpp
 549783: global: [c: 1651] system.cpu [sn:1500 pc:0x80000130] enDqT: 547452, exDqT: 547785, readyT: 547785, CompleT:548784, andi t4, t2, 1, res: 0
 549783: global: [c: 1651] system.cpu [sn:1501 pc:0x80000134] enDqT: 547452, exDqT: 547785, readyT: 548118, CompleT:549117, beq t4, zero, 6
 555444: global: [c: 1668] system.cpu [sn:1600 pc:0x8000013a] enDqT: 553446, exDqT: 553779, readyT: 553779, CompleT:554778, andi t4, t2, 1, res: 0
... 
 559440: global: [c: 1680] system.cpu [sn:1621 pc:0x80000130] enDqT: 557442, exDqT: 557775, readyT: 557775, CompleT:558774, andi t4, t2, 1, res: 0x1
 559773: global: [c: 1681] system.cpu [sn:1622 pc:0x80000134] enDqT: 557442, exDqT: 557775, readyT: 558108, CompleT:559107, beq t4, zero, 6
 559773: global: [c: 1681] system.cpu [sn:1623 pc:0x80000138] enDqT: 557442, exDqT: 557775, readyT: 557775, CompleT:559107, c_addi zero, 0
 559773: global: [c: 1681] system.cpu [sn:1624 pc:0x8000013a] enDqT: 557442, exDqT: 557775, readyT: 557775, CompleT:558774, andi t4, t2, 1, res: 0x1
```

能看出第一次 t4 = 0, 134 这个beq 发生taken, 跳到13a 的andi 指令执行

第二次 t4=1, 134 这个beq NT, 顺序执行138 这个nop指令（c_addi), 然后再执行13a 这个andi 指令



然后可以查看stats.txt 获取基本信息，一般主要关注如下

```plain
system.cpu.numCycles                            10861
system.cpu.numInsts                             28166 
system.cpu.numBranches                          11023
system.cpu.ipc                               2.587239 
system.cpu.commit.branchMispredicts                23   # commit 阶段的最终错误预测指令

# 来自哪个阶段的预测结果，基本都是0阶段
system.cpu.branchPred.predsOfEachStage::0         7246                       # the number of preds of each stage that account for final pred (Count)
system.cpu.branchPred.predsOfEachStage::1          979                       # the number of preds of each stage that account for final pred (Count)
system.cpu.branchPred.predsOfEachStage::2            0  

# fsq 中包含的指令数目，基本是2条，9条，10条
system.cpu.branchPred.commitFsqEntryHasInsts::2         5002     66.53%     66.56% # number of insts that commit fsq entries have (Count)
system.cpu.branchPred.commitFsqEntryHasInsts::9          500      6.65%     86.70% # number of insts that commit fsq entries have (Count)
system.cpu.branchPred.commitFsqEntryHasInsts::10         1000     13.30%    100.00% # number of insts that commit fsq entries have (Count)

system.cpu.branchPred.ftbHit                     6483                       # ftb hits (in predict block) (Count)
system.cpu.branchPred.ftbMiss                      25                       # ftb misses (in predict block) (Count)
# 主要关注updateMiss, 这是实际正确路径上ftb miss 情况，predMiss 包括错误路径的
system.cpu.branchPred.ftb.predMiss               1601                       # misses encountered on prediction (Count)
system.cpu.branchPred.ftb.predHit                6625                       # hits encountered on prediction (Count)
system.cpu.branchPred.ftb.updateMiss               23                       # misses encountered on update (Count)
system.cpu.branchPred.ftb.updateHit              6485                       # hits encountered on update (Count)
```

还有tage, 等各类计数器，选择性观看



最后可以看看 各类debug-flags=DecoupleBP,FTB

```bash
./build/RISCV/gem5.debug  --outdir=debug/tage1 --debug-flags=DecoupleBP,FTB --debug-file=tage1.bp2 --debug-end=1000000 ./configs/example/xiangshan.py --generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/tage1-riscv64-xs.bin --raw-cpt
```

输出文件位置在/nfs/home/yanyue/workspace/GEM5/debug/tage1/tage1.bp2 

```bash
 655011: system.cpu.branchPred: [c: 1967] looking up pc 0x80000134
 655011: system.cpu.branchPred: [c: 1967] Supplying fetch with target ID 161
 655011: system.cpu.branchPred: [c: 1967] Responsing fetch with FTQ:: 0x80000130 - [0, 0x80000150) --> 0, taken: 0, fsqID: 162, loop: 0, iter: 0, exit: 0
 655011: system.cpu.branchPred: [c: 1967] FSQ: 0x80000130-[0, 0) --> 0, taken: 0, predEndPC: 0x80000150, isHit: 1, falseHit: 0
 655011: system.cpu.branchPred: [c: 1967] FTB entry: valid 1, tag 0, fallThruAddr:0x80000150, slots:
 655011: system.cpu.branchPred: [c: 1967]     pc:0x80000134, size:4, target:0x8000013a, cond:1, indirect:0, call:0, return:0
 655011: system.cpu.branchPred: [c: 1967] Predict it not taken to 0x80000138

 656676: system.cpu.branchPred: [c: 1972] looking up pc 0x80000134
 656676: system.cpu.branchPred: [c: 1972] Supplying fetch with target ID 165
 656676: system.cpu.branchPred: [c: 1972] Responsing fetch with:: 0x80000130 - [0x80000134, 0x80000150) --> 0x8000013a, taken: 1, fsqID: 166, loop: 0, iter: 0, exit: 0
 656676: system.cpu.branchPred: [c: 1972] 0x80000130-[0x80000134, 0x80000138) --> 0x8000013a, taken: 1, predEndPC: 0x80000150, isHit: 1, falseHit: 0
 656676: system.cpu.branchPred: [c: 1972] FTB entry: valid 1, tag 0, fallThruAddr:0x80000150, slots:
 656676: system.cpu.branchPred: [c: 1972]     pc:0x80000134, size:4, target:0x8000013a, cond:1, indirect:0, call:0, return:0
 656676: system.cpu.branchPred: [c: 1972] Predicted pc: 0x8000013a, upc: 0, npc(meaningless): 0x8000013e, instSeqNum: 2830
 656676: system.cpu.branchPred: [c: 1972] Predict it taken to 0x8000013a
```

能看到第一次预测134 这个branch 时候，

ftq = 0x80000130 - [0, 0x80000150)

fsq = 0x80000130-[0, 0) --> 0, taken: 0, predEndPC: 0x80000150

ftb = 0x80000130  valid 1, tag 0, fallThruAddr:0x80000150, 有一个slot 但由于uftb预测NT, 也不跳



第二次预测134 

ftq = 0x80000130 - [0x80000134, 0x80000150) --> 0x8000013a, 预测134 指令跳转到13a

fsq = 0x80000130-[0x80000134, 0x80000138) --> 0x8000013a， taken: 1, predEndPC: 0x80000150  

内容和ftq基本一样

ftb = 0x80000130  valid 1, tag 0, fallThruAddr:0x80000150. 内部包含134 这个br 指令, 其预测taken了



如果观察第二条13e 这个branch 时候，比较有意思

```bash
 655011: system.cpu.branchPred: [c: 1967] looking up pc 0x8000013e
 655011: system.cpu.branchPred: [c: 1967] Supplying fetch with target ID 161
 655011: system.cpu.branchPred: [c: 1967] Responsing fetch with:: 0x80000130 - [0, 0x80000150) --> 0, taken: 0, fsqID: 162, loop: 0, iter: 0, exit: 0
 655011: system.cpu.branchPred: [c: 1967] 0x80000130-[0, 0) --> 0, taken: 0, predEndPC: 0x80000150, isHit: 1, falseHit: 0
 655011: system.cpu.branchPred: [c: 1967] FTB entry: valid 1, tag 0, fallThruAddr:0x80000150, slots:
 655011: system.cpu.branchPred: [c: 1967]     pc:0x80000134, size:4, target:0x8000013a, cond:1, indirect:0, call:0, return:0
 655011: system.cpu.branchPred: [c: 1967] Predict it not taken to 0x80000142

 657009: system.cpu.branchPred: [c: 1973] looking up pc 0x8000013e
 657009: system.cpu.branchPred: [c: 1973] Supplying fetch with target ID 166
 657009: system.cpu.branchPred: [c: 1973] Responsing fetch with:: 0x8000013a - [0x8000013e, 0x8000015a) --> 0x80000144, taken: 1, fsqID: 167, loop: 0, iter: 0, exit: 0
 657009: system.cpu.branchPred: [c: 1973] 0x8000013a-[0x8000013e, 0x80000142) --> 0x80000144, taken: 1, predEndPC: 0x8000015a, isHit: 1, falseHit: 0
 657009: system.cpu.branchPred: [c: 1973] FTB entry: valid 1, tag 0, fallThruAddr:0x8000015a, slots:
 657009: system.cpu.branchPred: [c: 1973]     pc:0x8000013e, size:4, target:0x80000144, cond:1, indirect:0, call:0, return:0
 657009: system.cpu.branchPred: [c: 1973] Predicted pc: 0x80000144, upc: 0, npc(meaningless): 0x80000148, instSeqNum: 2832
 657009: system.cpu.branchPred: [c: 1973] Predict it taken to 0x80000144
```

第一次预测13e, 发现仍然使用130-150 的这个fsq, ftq, ftb, 甚至ftb 都不包含13e 这个br, 但是由于都是预测NT, 所以都没有预测其taken， 还是沿用老的ftb 项

第二次预测13e

ftq=0x8000013a - [0x8000013e, 0x8000015a) --> 0x80000144, fsq 类似

ftb=  0x8000013a  FTB entry: valid 1, tag 0, fallThruAddr:0x8000015a, slots:

        pc:0x8000013e, size:4, target:0x80000144, cond:1, indirect:0, call:0, return:0

会生成并使用一个新的ftb entry, 同时产生新的ftq, fsq 项！

