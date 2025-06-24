### 语义：用量子算法做因数分解
./libquantum 33 5

分解33， 随机种子5， 结果 33=3*11

同理，其他因数包括1397 = 11 * 127（用qemu 要跑7分钟)

400 = 4 * 100

143 = 13 * 11



### 按照qemu 插件获取的结果
（143 = 13 * 11）

```cpp
Total Dynamic Instructions: 280089787  1

Top 10 Most Executed Basic Blocks with Instructions:

Basic Block at 0x128b0 executed 15299278 times
Instructions:
  0x128b0: 00471793          slli                    a5,a4,4
  0x128b4: 97c2              add                     a5,a5,a6
  0x128b6: 6794              ld                      a3,8(a5)
  0x128b8: 00c6f5b3          and                     a1,a3,a2
  0x128bc: 02b60e63          beq                     a2,a1,60                # 0x128f8


Basic Block at 0x128c0 executed 13065768 times
Instructions:
  0x128c0: 0705              addi                    a4,a4,1
  0x128c2: 0007079b          sext.w                  a5,a4
  0x128c6: fea7c5e3          bgt                     a0,a5,-22               # 0x128b0


Basic Block at 0x129bc executed 7060103 times
Instructions:
  0x129bc: 681c              ld                      a5,16(s0)
  0x129be: 00471693          slli                    a3,a4,4
  0x129c2: 0705              addi                    a4,a4,1
  0x129c4: 97b6              add                     a5,a5,a3
  0x129c6: 6794              ld                      a3,8(a5)
  0x129c8: 0007061b          sext.w                  a2,a4
  0x129cc: 8ea9              xor                     a3,a3,a0
  0x129ce: e794              sd                      a3,8(a5)
  0x129d0: 405c              lw                      a5,4(s0)
  0x129d2: fef645e3          bgt                     a5,a2,-22               # 0x129bc


Basic Block at 0x126a0 executed 2920476 times
Instructions:
  0x126a0: 0685              addi                    a3,a3,1
  0x126a2: 0006879b          sext.w                  a5,a3
  0x126a6: fcc7c3e3          bgt                     a2,a5,-58               # 0x1266c


Basic Block at 0x1266c executed 2919464 times
Instructions:
  0x1266c: 00469713          slli                    a4,a3,4
  0x12670: 9742              add                     a4,a4,a6
  0x12672: 671c              ld                      a5,8(a4)
  0x12674: 00b7c8b3          xor                     a7,a5,a1
  0x12678: 8fe9              and                     a5,a5,a0
  0x1267a: c39d              beqz                    a5,38                   # 0x126a0
```

大致看出总共110亿条指令

前两个基本块共执行6亿，分别有5，3条指令，占据48亿，大约50%执行时间

可能和我编译选项设置会不会有影响？

切片用的都是gcc 12 -O2

### perf 结果
由于perf record qemu-riscv64 ./libquantum 得到的都是翻译后指令，无法查看

所以查看x86版本的



```cpp
  52.02%  libquantum_base  libquantum_base.x86-gcc11.4.0-o3  [.] quantum_toffoli                                                                                                                                            ▒
  18.34%  libquantum_base  libquantum_base.x86-gcc11.4.0-o3  [.] quantum_sigma_x                                                                                                                                            ◆
  16.63%  libquantum_base  libquantum_base.x86-gcc11.4.0-o3  [.] quantum_cnot  

      toffoli:
0.86 │18c:   mov    0x10(%r12),%r9                                                                                                                                                                                                                              ▒
 10.75 │191:┌─→mov    %rdx,%rax                                                                                                                                                                                                                                   ▒
 10.33 │    │  shl    $0x4,%rax                                                                                                                                                                                                                                   ▒
 10.09 │    │  add    %r9,%rax                                                                                                                                                                                                                                    ▒
 10.46 │    │  mov    0x8(%rax),%rcx                                                                                                                                                                                                                              ▒
 10.35 │    │  mov    %rcx,%rdi                                                                                                                                                                                                                                   ▒
 11.10 │    │  and    %rsi,%rdi                                                                                                                                                                                                                                   ▒
 10.83 │    │  cmp    %rdi,%rsi                                                                                                                                                                                                                                   ▒
  0.01 │    │↓ je     1d8                                                                                                                              ▒
 10.44 │    │  add    $0x1,%rdx                                                                                                                      ▒
 10.17 │    ├──cmp    %edx,%r8d                                              
       │    └──jg     191           
```

这段函数占据50%执行时间，其中有两个基本块，其中第一个不跳转，第二个跳转，就是for 循环生成的。

```cpp
基本块 0x128b0 (执行了15,299,278次)：
这是循环的主体部分。
1 slli a5,a4,4: 将a4（循环计数器i）左移4位（相当于乘以16）。
2 add a5,a5,a6: a6可能是一个基址，这步计算了一个内存地址。
3 ld a3,8(a5): 从计算出的地址加8的位置加载一个64位值。
4 and a1,a3,a2: 对加载的值进行掩码操作，a2可能是掩码。
5 beq a2,a1,60: 如果掩码操作的结果等于掩码本身，跳转到循环外（可能是找到了目标值）。

基本块 0x128c0 (执行了13,065,768次)：
这是循环的增量和条件检查部分。
6 addi a4,a4,1: 循环计数器i增加1。
7 sext.w a5,a4: 将新的计数器值符号扩展到a5。
8 bgt a0,a5,-22: 如果a0（可能是循环的上限）大于新的计数器值，跳回循环开始。


```

### 源码分析
```cpp
//差不多对应内容如下
for (int i = 0; i < max_iterations; i++) {
    long long *ptr = (long long *)((char *)base_addr + i * 16);
    long long value = ptr[1];
    if ((value & mask) == mask) {
        // 执行一些操作，然后跳出循环
        break;
    }
    // 循环继续
}

// 真实源代码
for(i=0; i<reg->size; i++)
	{
        // 读取 reg->node[i].state
	  if(reg->node[i].state & ((MAX_UNSIGNED) 1 << control1))
	    {
    	   if(reg->node[i].state & ((MAX_UNSIGNED) 1 << control2))
    		{
    		  reg->node[i].state ^= ((MAX_UNSIGNED) 1 << target);
    		}
	    }
	}
```

```plain
对上面的依赖分析可以发现依赖图！
6 影响之后的 6， 7， 1
6 -> 7
1 -> 2 -> 3 -> 4
那么横坐标代表一拍能执行的指令数目，纵坐标代表时间
2
2  2
1  2  2
1  1  2  2
   1  2  2  2
      1  1  2
         1  1
可以发现一拍最多6条指令，但是加上2条分支指令
那么在6发射处理器中，刚好能把流水线占满，ipc = 8
而在8发射中，刚好能把流水线占满，ipc = 8
目前6发射中，1拍最多占满所有指令
包括5个ALU指令（6, 7, 1, 2, 4）（add,and,slli,addi）, 1个load
```

应该最多1拍提交8条指令吧，目前发现在 CompleT:2109888 能最多提交6条指令



<font style="background-color:#FBDE28;">20241003 满洋：如果对iteration counter a4做依赖消除，可以将ILP翻倍，按照苹果的8ALU 3LD 2ST配置，可以吃满10宽度的fetch/decode/rename。如果在decode前做指令融合，比如对1-2，4-5，后端最多能支持到每周期14条指令的吞吐量。但此时前端供指基本没有可能达到12或14。</font>

<font style="background-color:#FBDE28;">addi.w a4, a4, 1的消除 Intel Alder Lake已实装，</font>[<font style="background-color:#FBDE28;">https://www.computerenhance.com/p/the-case-of-the-missing-increment</font>](https://www.computerenhance.com/p/the-case-of-the-missing-increment)



### libquantum 6发射 vs 8发射 ipc


| 切片号 | 权重 | <font style="color:#000000;">ipc(6发射）</font> | <font style="color:#000000;">ipc（8发射）</font> | <font style="color:#000000;">ipc提升值</font> |
| --- | --- | --- | --- | --- |
| <font style="color:#000000;">_100346</font> | <font style="color:#000000;">0.00731056</font> | <font style="color:#000000;">3.636472</font> | <font style="color:#000000;">5.46745</font> | <font style="color:#000000;">33.49%</font> |
| <font style="color:#000000;">_101164</font> | <font style="color:#000000;">0.00415776</font> | <font style="color:#000000;">5.98379</font> | <font style="color:#000000;">7.916369</font> | <font style="color:#000000;">24.41%</font> |
| <font style="color:#000000;">_14430</font> | <font style="color:#000000;">0.0209563</font> | <font style="color:#000000;">4.496246</font> | <font style="color:#000000;">4.743929</font> | <font style="color:#000000;">5.22%</font> |
| <font style="color:#000000;">_15361</font> | <font style="color:#000000;">0.0684355</font> | <font style="color:#000000;">4.261748</font> | <font style="color:#000000;">4.337766</font> | <font style="color:#000000;">1.75%</font> |
| <font style="color:#000000;">_18943</font> | <font style="color:#000000;">0.0232224</font> | <font style="color:#000000;">4.237812</font> | <font style="color:#000000;">4.243664</font> | <font style="color:#000000;">0.14%</font> |
| <font style="color:#000000;">_22269</font> | <font style="color:#000000;">0.0864361</font> | <font style="color:#000000;">4.269735</font> | <font style="color:#000000;">4.183884</font> | <font style="color:#000000;">-2.05%</font> |
| <font style="color:#000000;">_25990</font> | <font style="color:#000000;">0.0635782</font> | <font style="color:#000000;">4.43617</font> | <font style="color:#000000;">4.533867</font> | <font style="color:#000000;">2.15%</font> |
| <font style="color:#000000;">_37989</font> | <font style="color:#000000;">0.049903</font> | <font style="color:#000000;">4.460656</font> | <font style="color:#000000;">4.594532</font> | <font style="color:#000000;">2.91%</font> |
| <font style="color:#000000;">_39228</font> | <font style="color:#000000;">0.0140792</font> | <font style="color:#000000;">4.347752</font> | <font style="color:#000000;">4.352969</font> | <font style="color:#000000;">0.12%</font> |
| <font style="color:#000000;">_54695</font> | <font style="color:#000000;">0.0678444</font> | <font style="color:#000000;">4.655957</font> | <font style="color:#000000;">5.154718</font> | <font style="color:#000000;">9.68%</font> |
| <font style="color:#000000;">_58444</font> | <font style="color:#000000;">0.0497946</font> | <font style="color:#000000;">4.892461</font> | <font style="color:#000000;">5.000091</font> | <font style="color:#000000;">2.15%</font> |
| <font style="color:#000000;">_64624</font> | <font style="color:#000000;">0.24316</font> | <font style="color:#000000;">4.776473</font> | <font style="color:#000000;">4.773239</font> | <font style="color:#000000;">-0.07%</font> |
| <font style="color:#000000;">_67297</font> | <font style="color:#000000;">0.0188774</font> | <font style="color:#000000;">4.355898</font> | <font style="color:#000000;">5.106623</font> | <font style="color:#000000;">14.70%</font> |
| <font style="color:#000000;">_69779</font> | <font style="color:#000000;">0.0121284</font> | <font style="color:#000000;">4.573573</font> | <font style="color:#000000;">4.582266</font> | <font style="color:#000000;">0.19%</font> |
| <font style="color:#000000;">_72640</font> | <font style="color:#000000;">0.114092</font> | <font style="color:#000000;">4.540071</font> | <font style="color:#000000;">4.548779</font> | <font style="color:#000000;">0.19%</font> |
| <font style="color:#000000;">_96240</font> | <font style="color:#000000;">0.0423756</font> | <font style="color:#000000;">4.82169</font> | <font style="color:#000000;">4.974668</font> | <font style="color:#000000;">3.08%</font> |
| <font style="color:#000000;">_97821</font> | <font style="color:#000000;">0.0648098</font> | <font style="color:#000000;">4.409328</font> | <font style="color:#000000;">4.43158</font> | <font style="color:#000000;">0.50%</font> |
| <font style="color:#000000;">_98457</font> | <font style="color:#000000;">0.0488389</font> | <font style="color:#000000;">4.496824</font> | <font style="color:#000000;">4.50179</font> | <font style="color:#000000;">0.11%</font> |


应该再看看gem5 在运行libquantum 过程中，是否是我想的这样！

>  /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/cluster-0-0.json
>

发现101切片的ipc 最大可以到8！可惜权重太低了，对整体ipc影响不大

其中64624  这个切片权重最大，打印出其中的commitTrace

```json
./build/RISCV/gem5.opt configs/example/xiangshan.py --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/64624/_64624_0.243160_.zstd
```



```json
2099898: global: system.cpu [sn:2642 pc:0x12cba] enDqT: 2059272, exDqT: 2059605, readyT: 2059605, CompleT:2060604, c_addi a4, 1, res: 0x1b1004
2099898: global: system.cpu [sn:2643 pc:0x12cbc] enDqT: 2059272, exDqT: 2059605, readyT: 2059938, CompleT:2060937, addiw a5, a4, 0, res: 0x1b1004
2099898: global: system.cpu [sn:2644 pc:0x12cc0] enDqT: 2059272, exDqT: 2059605, readyT: 2060271, CompleT:2061270, blt a5, a1, -22
2100231: global: system.cpu [sn:2645 pc:0x12caa] enDqT: 2059272, exDqT: 2059605, readyT: 2059938, CompleT:2060937, slli a5, a4, 4, res: 0x1b10040
2100231: global: system.cpu [sn:2646 pc:0x12cae] enDqT: 2059272, exDqT: 2059605, readyT: 2060271, CompleT:2061270, c_add a5, a0, res: 0x2003b13050
2100231: global: system.cpu [sn:2647 pc:0x12cb0] enDqT: 2059605, exDqT: 2059938, readyT: 2060604, CompleT:2087911, c_ld a3, 8(a5), res: 0xd8802479400000, paddr: 0xf53db058
2100231: global: system.cpu [sn:2648 pc:0x12cb2] enDqT: 2059605, exDqT: 2059938, readyT: 2061602, CompleT:2089575, and a2, a3, s0, res: 0x400000
2100231: global: system.cpu [sn:2649 pc:0x12cb6] enDqT: 2059605, exDqT: 2059938, readyT: 2088909, CompleT:2089908, beq a2, s0, 60
```



现在的关键点在于，bge 之前还有一条sext.w 指令，而更之前的addi 指令才是核心的递增指令！

需要更好的识别到才行！

```json
slli a5,a4,4
add a5,a5,a6
ld a3,8(a5)
and a1,a3,a2
beq a2,a1,60
addi a4,a4,1
sext.w a5,a4
bgt a0,a5,-22  
```

目前这个指令序列有8条指令= 4 add + 1 sll + 2 branch + 1 load

如果在资源完全充足情况下，应该每拍能发射8条指令！

对于6发射处理器，只有4个alu, 可能导致无法完全发射

对于8发射处理器，有6个alu，2个bru，2个load， 应该可以完全发射



试试用llvm-mca 来分析下！

发现尝试了半天，目前llvm-mca, osaca 都支持x86 , aarch64，感觉如果想添加riscv64还是需要一定的工作量，我应该再看看gem5的详细结果！



如果只是运行64624 切片

似乎在运行8发射时候，ipc = 5.89

6发射时候，ipc = 4.46, 还是提升挺多的

后来发现这是warmup的ipc，应该看warmup 之后的ipc才对！

如果把所有切片全部计算， 8发射cpi=0.2158. ipc = 4.63

6发射， cpi = 0.22, ipc=4.545，提升很小！

### 看看commitTrace
> 发现运行101164 切片的ipc 已经到达8了！看看commitTrace吧
>

```cpp
./build/RISCV/gem5.opt --debug-start=2600000000 --debug-file=libquantum.commitTrace8_101.less.gz --debug-flags=CommitTrace configs/example/xiangshan.py --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/101164/_101164_0.004158_.zstd
```

目前发现101切片的大部分指令都不是我发现的那个热点块啊

```cpp
sllw a2, a1, a5, res: 0x800000
blt a3, a2, -26
ld a5, 24(s3), res: 0x2000002010, paddr: 0x80905b38
sh2add a5, a4, a5, res: 0x200010711c
sw zero, 0(a5), paddr: 0x80e7d11c
lw a5, 8(s3), res: 0x17, paddr: 0x80905b28
c_addi a4, 1, res: 0x41444
addiw a3, a4, 0, res: 0x41444
```

发现646 切片也不包含我关注的热点块啊！

```cpp
./build/RISCV/gem5.opt --debug-start=1500000000 --debug-file=libquantum.commitTrace8_646.less.gz --debug-flags=CommitTrace configs/example/xiangshan.py --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/64624/_64624_0.243160_.zstd
```

还是用python脚本直接分析出来，真是特别方便哈哈哈，claude太强了，几乎直接就帮我写好代码了， 这个基本块执行50万次，占总的900万条指令的一半了

用python脚本统计commitTrace 中最常出现的基本块

```cpp
python3 trace_bb.py libquantum.commitTrace8_646.less.gz > libquantum.commitTrace8_646.bb
```

```cpp
总共900万行指令，其中前两个基本块分别55万 *9 + 15万 * 12 = 675万
Top 10 most common basic blocks:
Count: 549857

Count: 671958 
Instructions: 3    i++块
  0x12aac: c_addi a3, 1
  0x12aae: addiw a5, a3, 0
  0x12ab2: blt a5, a6, -58

Count: 671936
Instructions:  6   if 条件判断块
  0x12a78: slli a4, a3, 4
  0x12a7c: c_add a4, a7
  0x12a7e: c_ld a5, 8(a4)  8(a4)=reg->node[i]
  0x12a80: xor a2, a5, a1
  0x12a84: c_and a5, a0
  0x12a86: c_beqz a5, 38

Count: 278101  和基本块1 很像，多了c_sd, lw, 但是最后的branch不相同！
Instructions:  5
  0x12a88: c_sd a2, 8(a4)  if 判断对了，需要store
  0x12a8a: lw a6, 4(s0)
  0x12a8e: c_addi a3, 1    i++ 块
  0x12a90: addiw a5, a3, 0
  0x12a94: bge a5, a6, 34

Count: 278065  这个只是比基本块2 多了一1条load！最后的branch 不同
Instructions: 7  也是if 条件判断
  0x12a98: ld a7, 16(s0)
  0x12a9c: slli a4, a3, 4
  0x12aa0: c_add a4, a7
  0x12aa2: c_ld a5, 8(a4)
  0x12aa4: xor a2, a5, a1
  0x12aa8: c_and a5, a0
  0x12aaa: c_bnez a5, -34
```

再看看具体的vim commmitTrace

配置了一下vim, 安装vim-quickhl 插件， 现在空格 + m 就能高亮选中区域了

感觉这4个基本块都是混合着来的，不太方便直接分析啊，并不是一个完整的循环样子？

我该如何去分析它目前为何空泡指令比较多呢？

看看objdump, 主要是quantum_cnot, 再12a78 -> 12ab2 循环执行

```cpp
   12a78:	00469713          	slli	a4,a3,0x4
   12a7c:	9746                	add	a4,a4,a7  addr
   12a7e:	671c                	ld	a5,8(a4)  load reg[i].state
   12a80:	00b7c633          	 xor	a2,a5,a1  if 条件判断
   12a84:	8fe9                	and	a5,a5,a0
   12a86:	c39d                	beqz	a5,12aac <quantum_cnot+0xe4> 不满足
       12a88:	e710                	sd	a2,8(a4) store
       12a8a:	00442803          	lw	a6,4(s0)     load
       12a8e:	0685                	addi	a3,a3,1
       12a90:	0006879b          	sext.w	a5,a3       i++
       12a94:	0307d163          	bge	a5,a6,12ab6 <quantum_cnot+0xee>  开头

       12a98:	01043883          	ld	a7,16(s0)    load
       12a9c:	00469713          	slli	a4,a3,0x4
       12aa0:	9746                	add	a4,a4,a7
       12aa2:	671c                	ld	a5,8(a4)
       12aa4:	00b7c633          	xor	a2,a5,a1
       12aa8:	8fe9                	and	a5,a5,a0
       12aaa:	fff9                	bnez	a5,12a88 <quantum_cnot+0xc0>
   12aac:	0685                	addi	a3,a3,1  i++
   12aae:	0006879b          	sext.w	a5,a3  i++
   12ab2:	fd07c3e3          	blt	a5,a6,12a78 <quantum_cnot+0xb0>
```

看看对应源代码

```c
for(i=0; i<reg->size; i++)
{	// 遍历一个大循环
    /* Flip the target bit of a basis state if the control bit is set */
    // load 一个值，比较，如果为1， xor 一下这个值
    if((reg->node[i].state & ((MAX_UNSIGNED) 1 << control)))
        reg->node[i].state ^= ((MAX_UNSIGNED) 1 << target);
}
```

对应的x86 perf report 汇编，时间占比

```c
  3.24 │ f8:   xor     %r9,%rcx    // xor 指令        	                                                                                                                                                                          ▒
       │     for(i=0; i<reg->size; i++)                                                                                                                                                                               ▒
  2.82 │       add     $0x1,%rdx   // i++                                                                                                                                                                                    ▒
       │     reg->node[i].state ^= ((MAX_UNSIGNED) 1 << target);                                                                                                                                                      ▒
  2.72 │       mov     %rcx,0x8(%rax)  // store                                                                                                                                                                                ▒
       │     for(i=0; i<reg->size; i++)                                                                                                                                                                               ▒
  3.45 │       mov     0x4(%rbp),%esi  // esi = reg->size                                                                                                                                                                             ▒
  2.65 │       cmp     %edx,%esi       	// i < esi ?                                                                                                                                                                             ▒
       │     ↓ jle     129          // 跳出循环                                                                                                                                                                                    ▒
  2.83 │10a:   mov     0x10(%rbp),%rdi                                                                                                                                                                                ▒
       │     if((reg->node[i].state & ((MAX_UNSIGNED) 1 << control)))                                                                                                                                                 ▒
 13.33 │10e:┌─→mov     %rdx,%rax                                                                                                                                                                                      ▒
 11.72 │    │  shl     $0x4,%rax                                                                                                                                                                                      ▒
 10.36 │    │  add     %rdi,%rax                                                                                                                                                                                      ▒
 12.16 │    │  mov     0x8(%rax),%rcx  // load                                                                                                                                                                               ▒
 14.00 │    │  test    %r8,%rcx        // 判断  	                                                                                                                                                                             ▒
       │    │↑ jne     f8              // 满足，执行f8 xor 指令，然后继续循环                                                                                                                                                                              ▒
       │    │for(i=0; i<reg->size; i++)                                                                                                                                                                               ▒
 10.03 │    │  add     $0x1,%rdx       // 不满足，i++, 继续循环开头                                                                                                                                                                               ▒
 10.68 │    ├──cmp     %edx,%esi                                                                                                                                                                                      ▒
       │    └──jg      10e
```

看来这个riscv 汇编应该是高度优化后的汇编代码，就是两个基本块有所变异

那么按照次数来说，进入if 条件内应该比较少，所以核心代码应该还是前两个基本块



目前loop buffer 只是对单个小循环不断重复的情况做的恨到

但是对这种两个循环交替执行的情况，if 条件内可能进去，可能不进去，做的不好，所以导致前端供应指令不够，每次之能取到4-6条指令，在8发射情况下做的不好，需要bi-taken 分支获取技术。

### topdown 全局分析
frag stall 来看， 总指令数目2000万

```cpp
fetchStallReason::NoStall     24786497
fetchStallReason::FragStall     39688863  3千万
decodeStallReason::NoStall     3109 2223
decodeStallReason::FragStall      1130768 100万
renameStallReason::NoStall     2004 2996
renameStallReason::FragStall     1217 5302   1200万
dispatchStallReason::NoStall     2004 1608
dispatchStallReason::FragStall     1218 9184  1200万

    StallReason::total      3352 0216
```

也是fetch stall 特别大

