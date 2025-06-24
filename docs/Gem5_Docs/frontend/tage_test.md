# tage 测试

源代码

/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/tests/tage2.c
https://github.com/OpenXiangShan/nexus-am/blob/master/tests/frontendtest/tests/tage2.c

```cpp
#include "common.h"

void __attribute__ ((noinline)) branch_test(int cnt) {
    int tmp = 0;
    // 设计一个需要更长历史的分支模式：
    // 使用t2的多个位来决定分支方向
    // 比如：当t2的bit[2:0]为111=7时不跳转，其他情况都跳转
    // 修改后的分支模式：
    #define ONE \
    "andi t3, t2, 7\n\t"    /* 取低3位 */ \
    "li t4, 7\n\t"          /* 目标值111 */ \
    "bne t3, t4, 2f\n\t"     /* 如果不等于111则跳转 */ \
    "nop\n\t" \
    "2:\n\t"

    asm volatile(
    // 初始化计数器
    "li t0, 0\n\t"      // 循环计数器
        "li t2, 0\n\t"      // 模式计数器
        ".align 4\n\t"
        "1:\n\t"
        // 重复执行分支指令
        TEN                  // 每次循环执行10次分支指令
        "addi t2, t2, 1\n\t" // t2每次加1
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
```

反汇编

```cpp
    80000130:	0073fe13          	and	t3,t2,7
    80000134:	4e9d                	li	t4,7
    80000136:	01de1363          	bne	t3,t4,8000013c <branch_test+0x1c>
    8000013a:	0001                	nop
    8000013c:	0073fe13          	and	t3,t2,7
    80000140:	4e9d                	li	t4,7
    80000142:	01de1363          	bne	t3,t4,8
```

主要起始地址为130， 

136 的分支，7次taken, 1次NT



1000个循环中，有128个循环都是NT，每个循环有10个branch, 说明至少1280次NT, 这些分支一定需要用tage 来预测。剩下的taken 情况，用uftb 的简单方向预测就能命中，tage 都不用纠正它，也就不会更新tage了

stats.txt

```plain
system.cpu.numBranches                          11021
system.cpu.ipc                               2.428763 
system.cpu.commit.branchMispredicts                30 
# 
system.cpu.branchPred.predsOfEachStage::0        10878                       # the number of preds of each stage that account for final pred (Count)
system.cpu.branchPred.predsOfEachStage::1          238                       # the number of preds of each stage that account for final pred (Count)
system.cpu.branchPred.predsOfEachStage::2            0   

# 主要看tage update 部分统计
system.cpu.branchPred.tage.bank_0.updateTableHits::samples         1815                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::mean     0.812121                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::stdev     1.047108                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::underflows            0      0.00%      0.00% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::0         1077     59.34%     59.34% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::1          122      6.72%     66.06% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::2          496     27.33%     93.39% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::3          120      6.61%    100.00% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::overflows            0      0.00%    100.00% # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::min_value            0                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::max_value            3                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateTableHits::total         1815                       # hit of each tage table on update (Count)
system.cpu.branchPred.tage.bank_0.updateNoHitUseBim          128                       # use bimodal when no hit on update (Count)
system.cpu.branchPred.tage.bank_0.updateUseAlt          140                       # use alt on update (Count)
system.cpu.branchPred.tage.bank_0.updateUseAltCorrect          133                       # use alt on update and correct (Count)
system.cpu.branchPred.tage.bank_0.updateUseAltWrong            7                       # use alt on update and wrong (Count)
```

其中发现tage 预测了1800次，其中60% 都在T0 命中，剩下的其他级命中

而useAlt 基表的情况只有140次，且基本都正确



关注debug-flags=FTBTAGE

```plain
./build/RISCV/gem5.debug  --outdir=debug/tage2 --debug-flags=FTBTAGE --debug-file=tage2.tage --debug-end=1000000 ./configs/example/xiangshan.py --generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/tage2-riscv64-xs.bin --raw-cpt
```

先看预测阶段

```plain
 968364: system.cpu.branchPred.tage: [c: 2908] table 3, index 353, lookup tag 223, tag 213, useful 0, counter -1, v 1, match 0
 968364: system.cpu.branchPred.tage: [c: 2908] table 2, index 871, lookup tag 134, tag 134, useful 0, counter 3, v 1, match 1
 968364: system.cpu.branchPred.tage: [c: 2908] lookup startAddr 0x80000130 cond 0, provider_counts 1, main_table 2, main_table_index 871, use_alt 0
```

对于130 开头的第一个136 branch, 能看到这里命中到了table2, 其实发现对应的useflu=0?

```plain
2555775: system.cpu.branchPred.tage: [c: 7675] table 3, index 353, lookup tag 213, tag 213, useful 1, counter -4, v 1, match 1
2555775: system.cpu.branchPred.tage: [c: 7675] lookup startAddr 0x80000130 cond 0, provider_counts 1, main_table 3, main_table_index 353, use_alt 0
```

这里命中表3, ixd=353， 对应的useful=1， 合理，要比较晚才更新对useful=1吧



再看更新阶段

先看后期稳定状态的情况了，已经7800拍之后了

```plain
2613384: system.cpu.branchPred.tage: [c: 7848] update startAddr: 0x80000130
2613384: system.cpu.branchPred.tage: [c: 7848] try to update cond 0 
2613384: system.cpu.branchPred.tage: [c: 7848] prediction provided by table 3, idx 353, updating corresponding entry
2613384: system.cpu.branchPred.tage: [c: 7848] useful bit set to 1
2613384: system.cpu.branchPred.tage: [c: 7848] squashType 0, squashPC 0x555a9c67e280, slot pc 0x80000136
2613384: system.cpu.branchPred.tage: [c: 7848] this_cond_mispred 0, use_alt_on_main_found_correct 0, needToAllocate 0
```

在后期发现table3,idx353, 更新useful=1. 并且更新pc 就是136 这条分支指令。

同时没有错误预测，也不需要分配新的表项了。



再看看初期更新状态：第一次更新

```plain
 639360: system.cpu.branchPred.tage: [c: 1920] update startAddr: 0x80000130
 639360: system.cpu.branchPred.tage: [c: 1920] try to update cond 0 
 639360: system.cpu.branchPred.tage: [c: 1920] prediction provided by base table idx 152, updating corresponding entry
 639360: system.cpu.branchPred.tage: [c: 1920] squashType 2, squashPC 0x80000136, slot pc 0x80000136
 639360: system.cpu.branchPred.tage: [c: 1920] this_cond_mispred 1, use_alt_on_main_found_correct 0, needToAllocate 1
 639360: system.cpu.branchPred.tage: [c: 1920] allocate new entry
 639360: system.cpu.branchPred.tage: [c: 1920] found allocatable entry, table 0, index 103, tag 155, counter -1
```

第一次发现是base 表给出的预测结果，idx152. 同时base预测错误（实际NT, 预测为T）

 尝试分配新的一项，在T0 这里分配了一个表项，idx103

```plain
 653679: system.cpu.branchPred.tage: [c: 1963] update startAddr: 0x80000130
 653679: system.cpu.branchPred.tage: [c: 1963] try to update cond 0 
 653679: system.cpu.branchPred.tage: [c: 1963] prediction provided by base table idx 152, updating corresponding entry
 653679: system.cpu.branchPred.tage: [c: 1963] squashType 2, squashPC 0x80000136, slot pc 0x80000136
 653679: system.cpu.branchPred.tage: [c: 1963] this_cond_mispred 1, use_alt_on_main_found_correct 0, needToAllocate 1
 653679: system.cpu.branchPred.tage: [c: 1963] allocate new entry
 653679: system.cpu.branchPred.tage: [c: 1963] found allocatable entry, table 3, index 1894, tag 230, counter 0
```

第二次又发现tage 中base 152 预测错误(实际T， 预测NT)，直接分配了T3 的表项，lfsr随机生成的位置

```plain
 663336: system.cpu.branchPred.tage: [c: 1992] update startAddr: 0x80000130
 663336: system.cpu.branchPred.tage: [c: 1992] try to update cond 0 
 663336: system.cpu.branchPred.tage: [c: 1992] prediction provided by table 0, idx 103, updating corresponding entry
 663336: system.cpu.branchPred.tage: [c: 1992] useful bit set to 0
 663336: system.cpu.branchPred.tage: [c: 1992] use_alt_on_provider_weak, alt correct, updating use_alt_counter
 663336: system.cpu.branchPred.tage: [c: 1992] squashType 2, squashPC 0x80000136, slot pc 0x80000136
 663336: system.cpu.branchPred.tage: [c: 1992] this_cond_mispred 1, use_alt_on_main_found_correct 0, needToAllocate 1
 663336: system.cpu.branchPred.tage: [c: 1992] allocate new entry
 663336: system.cpu.branchPred.tage: [c: 1992] found allocatable entry, table 3, index 153, tag 74, counter 0
```

第三次发现tage T0 103表项预测错误，useful 改成0，同时直接分配T3 153表项。

此时T0 103, T3 153(t74) 都共存， 接下来立刻预测tage 表项时候发现， 索引了T0 103, 当时历史不同，索引到的T3 143? 而不是T3 153?

```plain
 666666: system.cpu.branchPred.tage: [c: 2002] table 3, index 143, lookup tag 155, tag 0, useful 0, counter 0, v 0, match 0
 666666: system.cpu.branchPred.tage: [c: 2002] table 2, index 879, lookup tag 196, tag 0, useful 0, counter 0, v 0, match 0
 666666: system.cpu.branchPred.tage: [c: 2002] table 1, index 1892, lookup tag 248, tag 0, useful 0, counter 0, v 0, match 0
 666666: system.cpu.branchPred.tage: [c: 2002] table 0, index 103, lookup tag 155, tag 155, useful 0, counter 0, v 1, match 1
 666666: system.cpu.branchPred.tage: [c: 2002] lookup startAddr 0x80000130 cond 0, provider_counts 1, main_table 0, main_table_index 103, use_alt 1
```

更新生成索引153 时候，buf=pc>>1 = 130>>1=0x98, updateIndexFoldedHist=1, index=0x98^1 = 0x99=153

预测生成索引143 时候，indexFoldedHist=0x17, index=0x98^0x17=0x8f=143. 本质上是索引的折叠历史不同。那为何索引T0 时候历史相同呢？应该要看折叠历史的生成过程

第一次更新索引，用的是meta->indexFoldedHist， 也就是上次预测时候的历史。然后再次预测时候，预测历史已经不同了（推测更新了），所以历史不同；而T0的预测历史，本身历史范围小，所以两次预测历史相同，生成的索引历史也相同？再确认下

```plain
111111111111111
 660672: system.cpu.branchPred.tage: [c: 1984] getTageIndex pc 0x80000130, t 0, indexFoldedHist 255
 660672: system.cpu.branchPred.tage: [c: 1984] table 0, index 103, lookup tag 155, tag 155, useful 0, counter -1, v 1, match 1
 660672: system.cpu.branchPred.tage: [c: 1984] lookup startAddr 0x80000130 cond 0, provider_counts 1, main_table 0, main_table_index 103, use_alt 0
111111111111011
 663003: system.cpu.branchPred.tage: [c: 1991] getTageIndex pc 0x80000130, t 0, indexFoldedHist 247
 663003: system.cpu.branchPred.tage: [c: 1991] table 0, index 111, lookup tag 131, tag 0, useful 0, counter 0, v 0, match 0
 663003: system.cpu.branchPred.tage: [c: 1991] lookup startAddr 0x80000130 cond 0, provider_counts 0, main_table -1, main_table_index -1, use_alt 1
10111111111111
 666666: system.cpu.branchPred.tage: [c: 2002] getTageIndex pc 0x80000130, t 0, indexFoldedHist 255
 666666: system.cpu.branchPred.tage: [c: 2002] table 0, index 103, lookup tag 155, tag 155, useful 0, counter 0, v 1, match 1
 666666: system.cpu.branchPred.tage: [c: 2002] lookup startAddr 0x80000130 cond 0, provider_counts 1, main_table 0, main_table_index 103, use_alt 1
```

能看出对于T0, 他在预测时后，indexFoldedHist 变化过255->247->255

本质原因是全局历史从8个1，变成011, 又变回8个1了，不同的全局历史产生不同的index 索引，进而影响到不同表项。对于低级表，其历史只会关注全局历史低部分位，关注历史长度有限。

