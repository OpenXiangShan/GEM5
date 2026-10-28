# 0.3c 热点源码与反汇编准备

只读检查共享 profile 的 ELF，并对三个位置执行 `riscv64-linux-gnu-objdump`；未运行模拟。机器可读的 ELF SHA256、选点权重、指令及假设见 [deep-dive-prep.json](deep-dive-prep.json)。旧热点来自 `fdbb5057`、1.0c、旧 TEA，用于选择当前候选，不是当前 515/365 的热点或可消除周期。当前 0.3c 选点来自 run1667 的 cluster，原始权重不是单独归一后的权重。

实际构建源码不可读，应用无精确 DWARF 行；下方 `/nfs/home/share/speccpu_install/cpu2006v99/` 是**可读参考源码**，与实际构建源是否逐字一致未验证。指令及函数是实测 ELF 反汇编事实，源代码对应仅为语义候选。历史根文件系统与 ELF 哈希一致的旧审计见 `/nfs/home/yanyue/tmp/tea-analysis-39234318080/evidence/elf-provenance.json`；本次只核对当前 ELF SHA，并未重新校验 rootfs。

## mcf：指针加载链

ELF `/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/elf/mcf.elf`，`main+0x58c`：`108d2 lw t4,24(a5)` → `108d6 blez` 影响是否进入主体；`108da ld t6,8(a5)` → `108e4 ld t6,0(t6)` 是明确地址依赖；另一链为 `108de ld t5,16(a5)` → `108e8 ld t5,0(t5)`。参考 [pbeampp.c](/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/429.mcf/src/pbeampp.c:165) 的 arc 扫描/ident 筛选和 tail/head potential 计算与该汇编结构相符，但非精确映射。当前选点 6753、12886；旧报告在12886发现两 PC 归因集中，见 `/nfs/home/yanyue/tmp/tea-analysis-39234318080/reports/baseline/detailed-report.md:31-43`。

待证伪方向：

- 若串行地址产生主导，单增第四 load 对 `108da→108e4` 的 response-to-issue 时间及该 PC CPI 不应显著改善；查该动态链的发射/返回、L1/L2 demand miss 和 load-to-use，避免用总吞吐代替关键链。
- 若 L2 data 或 directory 冲突造成关键服务等待，固定其它资源单改一类 bank，比较各 slice 的仲裁重试、demand 平均及热点加载服务时间和 IPC；重试是事件次数，不是独占周期。
- 若翻译是主导，需求 DTLB initial miss、PTW memory count/占用与热点 load translation 时间应同步支持；再只改一层 PTW 限制，检查预取翻译干扰反例。`ptwMemCycle` 是至少一请求在途的周期，并不代表队列满。

## perlbench_checkspam：间接跳转与字符扫描

ELF `/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/elf/perlbench_checkspam.elf`：`S_regmatch+0x66` 的 `92e14 sh2add` → `92e18 lw a5,0(a5)` → `92e1a jr a5` 构成表加载后间接跳转；`S_find_byclass.isra.0+0x3a2` 的 `96d10 bgeu s2,a3,96cee` 位于字符加载与比较路径。参考 [regexec.c](/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/400.perlbench/src/regexec.c:2296) 的 regmatch 循环与 [switch](/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/400.perlbench/src/regexec.c:2398)、[find_byclass](/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/400.perlbench/src/regexec.c:961) 是函数级语义候选，不证明特定编译器 jump table 来源。旧 TEA `92e18` 和 `96d10` 的 dominant point 均为当前 0.3c `5886`；另保留 28089 作反例。

待证伪方向：

- 若间接目标供给限制前端，比较 A/B/C 的逐 PC target 错误、redirect、FTQ 供给和退休 IPC，并在相同背景做间接预测器单轴；分支 PC 累计统计含预热时不能用来估算 ROI 每次罚时。
- 若表 load 到 `jr` 的真实数据链影响分支解析，查 `92e18` 的 cache 自身 miss/service、load 返回到 `92e1a` resolve 的事件间隔；固定预测器做单缓存轴对照。正确预测可能隐藏等待，不能仅从依赖静态汇编判断损失。
- 若字符扫描条件主导恢复，采集 `96d10` 的 ROI 方向、历史上下文、错误来源和 override/覆盖气泡，与第二个点交叉；只在证据支持时做单预测器轴对照。

## astar_biglakes：网格标记和循环边界

ELF `/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/elf/astar_biglakes.elf` 的 `way2obj::createway` clone：`1db7a lhu a6,16(s0)`、`1db8a lhu a7,0(a5)` 两个值由 `1db8e bne a7,a6,1db08` 比较；`1dba2 bne s3,s8,1db6e` 是邻近循环回边。参考 [Way2_.cpp](/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/473.astar/src/Way2_.cpp:180) 的二维网格和 fillnum 判定是语义候选。0.3c 点 5863、6185；旧版 `1db8e` 在 6185 权重较突出，旧 `1e516` 另见 `RegWay_.cpp:118` 的循环候选。

待证伪方向：

- 若标记分支的方向/别名是关键，收 `1db8e` ROI outcome 与 predictor/source/history、提交错误和恢复，再做单一预测器改变；静态源码不证明天然不可预测。
- 若双供给负收益源于改变推测/训练或供给时序，比较 A/B/C 的 target enqueues、override/FTQ 与该 PC 正确率、squash；用单轴控制检验，不把关联计数当因果。
- 若标记加载推迟分支解析，追两条 lhu 返回到分支 resolve 的动态时序，并并列 cache 自身 demand miss/latency；单缓存轴如无对应服务改善即可否定该方向。

下一步新 TEA 到达后，用相同采样器版本与正式提交数核验全局状态、事件及 unresolved 守恒；每 PC 只报告总归因和可重叠事件，避免把热点权重称为可回收周期。后续对照先验证配置、hash 和 difftest，再保留反例及权重覆盖。
