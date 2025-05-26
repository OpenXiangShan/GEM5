perf 结果

```bash
  78.37%  hmmer_base.x86-  hmmer_base.x86-gcc11.4.0-o3  [.] Viterbi*                                                                                                                                                     ◆
   8.31%  hmmer_base.x86-  hmmer_base.x86-gcc11.4.0-o3  [.] FChoose                                                                                                                                                      ▒
   7.57%  hmmer_base.x86-  hmmer_base.x86-gcc11.4.0-o3  [.] sre_random  
```



核心循环

```bash
P7Viterbi()
    for (k = 1; k <= M; k++) {
      mc[k] = mpp[k-1]   + tpmm[k-1];
      if ((sc = ip[k-1]  + tpim[k-1]) > mc[k])  mc[k] = sc;
      if ((sc = dpp[k-1] + tpdm[k-1]) > mc[k])  mc[k] = sc;
      if ((sc = xmb  + bp[k])         > mc[k])  mc[k] = sc; 
      mc[k] += ms[k];
      if (mc[k] < -INFTY) mc[k] = -INFTY;  

      dc[k] = dc[k-1] + tpdd[k-1];
      if ((sc = mc[k-1] + tpmd[k-1]) > dc[k]) dc[k] = sc;
      if (dc[k] < -INFTY) dc[k] = -INFTY;  

      if (k < M) {
	ic[k] = mpp[k] + tpmi[k];
	if ((sc = ip[k] + tpii[k]) > ic[k]) ic[k] = sc; 
	ic[k] += is[k];
	if (ic[k] < -INFTY) ic[k] = -INFTY; 
      }
    }
```

有很多if 条件判断，很像谓词逻辑，参考

[条件指令](https://www.yuque.com/yuqueyonghury5sjc/pgmott/gv0gql7zzggtucon)

如果什么都不加，编译出来很多mov, cmp 指令

如果加上B 扩展，就会生成好很多吧

zba, zbb, zbc, zbs 具体哪一个影响大呢？

或许可以用qemu 跑分来试试，看看运行时间因该能看出来

qemu 套着跑hmmer test

-march = gcb   time = 5.7s, insts = 116,721,122,501

-march = gc      time = 9.3s, insts = 168,642,744,640



用qemu plugins tbstat.so

gcb: insts = 18*10^9

gc: insts = 20*10^9

gcb 中出现大量max 指令 大约有1.3*10^9, 占比5%了，还是挺高的！

说明if (mc[k] < -INFTY) mc[k] = -INFTY;    会编译为 mc[k] = max(mc[k], -INFTY)





根据qemu plugins tbstat.so 统计出来运行test 输入时候的常见指令

注意这里统计的test 输入，切片是使用spec ref 输入，有一定误差，使用test 统计只需要1分钟

这里的程序二进制地址位于/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/elf/hmmer_base.riscv64-linux-gnu-gcc12.2.0

程序输入位于/nfs/home/yanyue/tools/cpu2006v99/benchspec/CPU2006/456.hmmer/data/test/input

运行参数位于/nfs/home/yanyue/tools/CPU2006LiteWrapper/456.hmmer/run-test.sh

```python
qemu-riscv64 -plugin $PLUGINS/libtbstat.so -d plugin ./hmmer_base.riscv64-linux-gnu-gcc12.2.0  --fixed 0 --mean 325 --num 45000 --sd 200 --seed 0 bombesin.hmm
```

```python
cat top_blocks_with_instructions.txt          
Total Dynamic Instructions: 17630787992

Top 10 Most Executed Basic Blocks with Instructions:

Basic Block at 0x1a9e2 executed 159793642 times
Instructions:
  0x1a9e2: flw                     fa4,0(a4)
  0x1a9e6: fadd.s                  dyn,fa5,fa5,fa4
  0x1a9ea: flt.s                   a3,fa0,fa5
  0x1a9ee: beqz                    a3,-20                  # 0x1a9da


Basic Block at 0x1a9da executed 144113680 times
Instructions:
  0x1a9da: addi                    a5,a5,1
  0x1a9dc: addi                    a4,a4,4
  0x1a9de: beq                     s5,a5,220               # 0x1aaba


Basic Block at 0x17ee2 executed 141152715 times
Instructions:
  0x17ee2: lw                      t1,0(a2)
  0x17ee6: lw                      a1,0(a0)
  0x17ee8: addi                    a2,a2,4
  0x17eea: addi                    a0,a0,4
  0x17eec: addw                    a1,a1,t1
  0x17ef0: max                     a6,a1,a6
  0x17ef4: bne                     a2,t3,-18               # 0x17ee2


Basic Block at 0x17cdc executed 125469080 times
Instructions:
  0x17cdc: ld                      a2,24(sp)
  0x17cde: lw                      a0,0(s1)
  0x17ce0: ld                      s2,32(sp)
  0x17ce2: add                     a2,a2,a1
  0x17ce4: lw                      a2,0(a2)
  0x17ce6: add                     s2,s2,a1
  0x17ce8: add                     s3,s4,a1
  0x17cec: addw                    a0,a0,a2
  0x17cee: sw                      a0,0(t3)
  0x17cf2: lw                      a2,0(t2)
  0x17cf6: lw                      s5,0(s2)
  0x17cfa: ld                      s2,40(sp)
  0x17cfc: addi                    s1,s1,4
  0x17cfe: addw                    a2,a2,s5
  0x17d02: max                     a2,a2,a0
  0x17d06: add                     s2,s2,a1
  0x17d08: sw                      a2,0(t3)
  0x17d0c: lw                      s5,0(s3)
  0x17d10: lw                      a0,0(s2)
  0x17d14: ld                      s2,64(sp)
  0x17d16: addi                    a6,a6,4
  0x17d18: addw                    a0,a0,s5
  0x17d1c: add                     s3,s2,a1
  0x17d20: max                     a0,a0,a2
  0x17d24: sw                      a0,0(t3)
  0x17d28: lw                      a2,4(s3)
  0x17d2c: ld                      s3,56(sp)
  0x17d2e: ld                      s2,8(sp)
  0x17d30: addi                    t2,t2,4
  0x17d32: add                     s10,s3,a1
  0x17d36: ld                      s3,48(sp)
  0x17d38: add                     s2,s2,a1
  0x17d3a: addiw                   t1,t1,1
  0x17d3c: add                     s5,s3,a1
  0x17d40: ld                      s3,0(sp)
  0x17d42: addi                    t3,t3,4
  0x17d44: addi                    t0,t0,4
  0x17d46: addw                    a2,a2,s3
  0x17d4a: max                     a2,a2,a0
  0x17d4e: sw                      a2,-4(t3)
  0x17d52: lw                      a0,4(s2)
  0x17d56: ld                      s2,112(sp)
  0x17d58: addw                    a0,a0,a2
  0x17d5a: max                     a0,a0,a4
  0x17d5e: sw                      a0,-4(t3)
  0x17d62: lw                      a2,0(s10)
  0x17d66: lw                      a0,-8(t0)
  0x17d6a: add                     s3,s2,a1
  0x17d6e: ld                      s2,120(sp)
  0x17d70: addw                    a0,a0,a2
  0x17d72: sw                      a0,-4(t0)
  0x17d76: lw                      a2,-8(t3)
  0x17d7a: lw                      s5,0(s5)
  0x17d7e: add                     s2,s2,a1
  0x17d80: ld                      s10,16(sp)
  0x17d82: addw                    a2,a2,s5
  0x17d86: max                     a2,a2,a0
  0x17d8a: max                     a2,a2,a4
  0x17d8e: sw                      a2,-4(t0)
  0x17d92: lw                      a2,4(s3)
  0x17d96: lw                      a0,0(s1)
  0x17d98: add                     s10,s10,a1
  0x17d9a: addi                    a1,a1,4
  0x17d9c: addw                    a0,a0,a2
  0x17d9e: sw                      a0,-4(a6)
  0x17da2: lw                      a2,0(t2)
  0x17da6: lw                      s2,4(s2)
  0x17daa: addw                    a2,a2,s2
  0x17dae: max                     a2,a2,a0
  0x17db2: sw                      a2,-4(a6)
  0x17db6: lw                      a0,4(s10)
  0x17dba: addw                    a0,a0,a2
  0x17dbc: max                     a0,a0,a4
  0x17dc0: sw                      a0,-4(a6)
  0x17dc4: bne                     a7,t1,-232              # 0x17cdc


Basic Block at 0x11f96 executed 15745916 times
Instructions:
  0x11f96: addi                    sp,sp,-32
  0x11f98: sd                      s2,8(sp)
  0x11f9a: lw                      a5,-1552(gp)
  0x11f9e: sd                      s0,24(sp)
  0x11fa0: sd                      s1,16(sp)
  0x11fa2: blez                    a5,364                  # 0x1210e


Basic Block at 0x1210e executed 15745915 times
Instructions:
  0x1210e: ld                      a5,-1392(gp)
  0x12112: ld                      a4,-1400(gp)
  0x12116: addi                    t6,gp,-1320
  0x1211a: j                       -216                    # 0x12042


Basic Block at 0x12042 executed 15745915 times
Instructions:
  0x12042: lui                     a2,13
  0x12044: addi                    a2,a2,420
  0x12048: rem                     a3,a5,a2
  0x1204c: div                     a5,a5,a2
  0x12050: lui                     a2,10
  0x12052: addi                    a2,a2,-946
  0x12056: mul                     a3,a3,a2
  0x1205a: lui                     a2,3
  0x1205c: addi                    a2,a2,-77
  0x12060: mul                     a5,a5,a2
  0x12064: sub                     a3,a3,a5
  0x12066: bgez                    a3,14                   # 0x12074


Basic Block at 0x5b82a executed 15683695 times
Instructions:
  0x5b82a: andi                    a5,a0,7
  0x5b82e: andi                    a4,a1,255
  0x5b832: bnez                    a5,14                   # 0x5b840


Basic Block at 0x5b88c executed 15683689 times
Instructions:
  0x5b88c: lbu                     a3,-8(a5)
  0x5b890: beq                     a3,a4,-72               # 0x5b848


Basic Block at 0x5b84a executed 15683689 times
Instructions:
  0x5b84a: andi                    a1,a1,255
  0x5b84e: slli                    a5,a1,8
  0x5b852: add                     a5,a5,a1
  0x5b854: slli                    t1,a5,16
  0x5b858: add                     t1,t1,a5
  0x5b85a: slli                    a5,t1,32
  0x5b85e: add                     t1,t1,a5
  0x5b860: auipc                   a6,41                   # 0x84860
  0x5b864: ld                      a6,1896(a6)
  0x5b868: addi                    t3,zero,-1
  0x5b86a: ld                      a1,0(a0)
  0x5b86c: addi                    a5,a0,8
  0x5b870: xor                     a7,t1,a1
  0x5b874: add                     a3,a1,a6
  0x5b878: add                     a2,a7,a6
  0x5b87c: xor                     a3,a3,a1
  0x5b87e: xor                     a2,a2,a7
  0x5b882: and                     a3,a3,a2
  0x5b884: or                      a3,a3,a6
  0x5b888: beq                     a3,t3,88                # 0x5b8e0
```

