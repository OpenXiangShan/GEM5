# 实际可读源码与 ELF 热点对应

只读 `/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006` 和 profile ELF，未构建、未运行模拟；完整路径与 ELF SHA256 见 [actual-source-hotspot-map.json](actual-source-hotspot-map.json)。另有独立任务核对源码和构建 provenance，本报告不代替该核验。ELF 无精确 DWARF 行，故指令地址到具体一行仍属推断；静态汇编依赖、ELF 函数符号和可读源码结构分别有直接证据。

## mcf：两条指针链

ELF `mcf.elf` 中 `main+0x58c` 内联片段：

```text
108d2 lw t4,24(a5)  → 108d6 blez t4,...   # 控制后续路径
108da ld t6,8(a5)   → 108e4 ld t6,0(t6)  # 地址依赖
108de ld t5,16(a5)  → 108e8 ld t5,0(t5)  # 另一条地址依赖
108e2 ld a3,0(a5)   → 108ec sub → 108f0 add
```

源码 [defines.h](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/429.mcf/src/defines.h:126) 定义 `arc` 的 `cost, tail, head, ident`；`cost_t` 为 long（:68-69），[node](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/429.mcf/src/defines.h:107) 首字段是 `potential`。在 RV64 LP64 布局下字段顺序与 0/8/16/24 偏移相符；[primal_bea_mpp](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/429.mcf/src/pbeampp.c:162) 扫描 arc，先判 `ident > BASIC`，再算 `cost - tail->potential + head->potential`，与加载/控制/算术序列一致。这是强结构和语义匹配，仍不能将单个 PC 标为精确源码行；`108d2` 的值控制路径，并非两条指针地址的直接生产者。

## perlbench_checkspam：正则匹配和字符扫描

ELF `perlbench_checkspam.elf` 的 `S_regmatch` 有 `92e14 sh2add a5,s0,a5` → `92e18 lw a5,0(a5)` → `92e1a jr a5`：静态可见表索引、目标加载和间接跳转依赖。实际源码 [regexec.c](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/400.perlbench/src/regexec.c:2324) 循环推进 `scan`，[:2394](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/400.perlbench/src/regexec.c:2394) 计算下一节点并对 `OP(scan)` 做 switch；汇编与 switch jump-table lowering 一致，但表和具体 source case 的精确对应未经 DWARF/编译中间表示验证。

`S_find_byclass.isra.0` 的 `96cf8–96d26` 包含 byte load、字符变换与 `96d10 bgeu` 条件分支；同名函数源码从 [regexec.c:961](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/400.perlbench/src/regexec.c:961) 开始，含 UTF-8 与普通字符类扫描，普通扫描示例为 [:994](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/400.perlbench/src/regexec.c:994)。不能把 `96d10` 指定到某个 case 或精确源码行。两种控制流需用实际 ROI 分支目标、load 服务和解析时序区分。

## astar_rivers：makebound2 的网格邻居扩展

选用实际 `astar_rivers.elf` 和目标点 `astar_rivers_8610`。ELF 符号为 `wayobj::makebound2(int*, int, int*) [clone .constprop.0]`：

```text
1c1d0 lhu a6,578(a5)     # 候选当前 fillnum
1c1d4 sh2add t1,a4,a7    # 以 index 生成 4-byte 单元地址
1c1d8 lhu t3,0(t1) → 1c1dc beq t3,a6,1c216
1c1e0 ld t3,528(a5) → 1c1e8 lh t3,0(t3) → 1c1ec bnez
1c1f4 sw a4,0(a6)；1c1fe sh a6,0(t1)；1c206 sh a6,2(t1)
```

源码 [Way_.h](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way_.h:17) 的 `waymapcellt` 含两个 `w16` 字段 `fillnum`、`num`；类中有 `maparp`、`waymap`、`step`、`fillnum`（[:46](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way_.h:46)）。实际 [Way_.cpp](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way_.cpp:45) 循环扩展邻居：比较 fillnum、检查 `maparp[index1] == 0`，入队并更新 fillnum/num（如 [:61](/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src/benchspec/CPU2006/473.astar/src/Way_.cpp:61)）。ELF 4-byte 索引、半字比较、后续 map 检查及两次半字写入与此逻辑相符；对象偏移到字段及 `1c1dc` 属于多个类似邻居检查中的哪一行仍是推断，不能给出精确行号。

上述是语义定位，不直接证明性能因果；与同版本 TEA 热点、branch outcome、load 返回/resolve 时序和单轴干预配对后才能判断瓶颈。

## 新正式TEA对旧候选的更新

astar_rivers_8610正式baseline Top 5实测全部位于`way2obj::createway`，不是上文预先选择的`makebound2 / 1c1dc`；以[新Top 5](astar-formal-tea-baseline.md)为本切片依据。上文makebound2仅保留为真实指令的语义候选，不能当作该窗口热点。
