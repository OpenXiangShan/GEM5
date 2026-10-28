# 第一轮 探针实现与代表点验证

本轮在已测量的24分源码 e34d8f6e7901 上，实现了单线程宽后端、串行双预测、理想指令供给与双取。代表点组合中，后端与前端共同放松有明显正向信号；当前只有短窗口局部结果，完整0.3c分数仍需CI验证。

## 配置与行为

| 探针 | 配置变化 | 建模范围 |
| --- | --- | --- |
| Backend | --upperbound-backend；SpMT拓扑但enableSpMT=false；10整数输出口、4 ld、2 STA/2 STD；IQ32、ROB640、IntRF512、FpRF384、LQ256、SQ128，decode/rename/commit16 | 无线程归属，取消RF端口仲裁；10是整数端口，不是所有类型合计发射上限 |
| Predict2 | idealPredictionsPerCycle=2，idealNoOverrideBubbles=true | 每CPU周期只推进一次tick；第一项独立FTQ快照与PC/RAS/history更新完成后再次预测；与PairTAGE、SMT双线程组合互斥 |
| Fetch2 | idealInstructionFetch=true，enableTwoFetch=true | 用Execute functional翻译读取实际指令；理想ITLB/Icache无等待，最多消费两项FTQ，可跨向后或远跳目标；不使用REF未来控制流 |
| Frontend | Predict2 + Fetch2 | 预测带宽、override latency和指令供给共同放松，后续需消融三者 |
| Combined | Backend + Frontend | 用于检验前后端瓶颈交互 |

原始默认开关全部关闭，保留原timing取指与每周期一次预测路径。ABTB ahead状态和S3 fast training会随每次虚拟预测推进两次，是本轮明确接受的理想化条件。

## 局部结果

![代表点局部IPC变化](figures/round01-local-probes.svg)

| 输入 | Baseline IPC | Backend IPC | Frontend IPC | Combined IPC | 组合相对基线 |
| --- | --- | --- | --- | --- | --- |
| CoreMark 300k | 1.557988 | 1.627383 | 1.701037 | 1.776562 | +14.029% |
| gcc_expr_1687 | 2.113776 | 2.123489 | 2.246737 | 2.267512 | +7.273% |
| mcf_12886 | 0.765291 | 0.969840 | 0.753957 | 0.989290 | +29.270% |
| astar_biglakes_5863 | 3.432317 | 4.188556 | 3.491296 | 4.301561 | +25.325% |
| omnetpp_18492 | 0.684971 | 0.820740 | 0.702907 | 0.835355 | +21.955% |

CoreMark为300k提交指令；四个SPEC代表点为约1M提交指令，无本轮正式20M预热。各变体到达同一个maxinsts阈值，实际退出提交量会有最多十余条的超额，增量使用真实insts/cycles。上表不能替代benchmark加权结果，也不能推算26–28目标已经实现。

mcf中仅打开前端反而略退，宽后端与组合明显改善；这支持继续区分窗口/LSU并发与前端资源的交互。gcc本点的后端单独改善较小，前端较有效。下一轮应通过完整点集确认这些方向是否能迁移到其他相位。

## 正确性与激活证据

- gem5.opt构建通过；修改区域style、Python语法和git diff检查通过。
- CoreMark六个模式正常退出，difftest启用、未报不一致；后端两个模式的首轮验证器因把IQ参数名当作INI对象路径而失败，修正验证器后重跑通过，模拟本身未失败。
- 四个checkpoint × baseline/backend/frontend/combined，共16个约1M窗口均通过正确性和激活检查。
- Combined CoreMark中，第二预测成功入队75,853项，双取成功72,878次；真实配置有4个独立load IQ、enableSpMT=false。
- Sv39与Sv48裸机回归均到达success m5_exit，difftest未报错；检查X-only、NX、非canonical别名、页末RVC、跨页32位指令的cause/PC/tval，并复用训练后的间接跳转检验错误NX预测后的恢复。
- 独立Astra Review发现functional Execute缺少canonical地址检查；修正有效satp/vsatp模式下的检查，并用上述回归验证。原始审查与定向复核分别保存在review-initial.md、review-followup-b.md。
- 正确性证据限于这些用例；没有声称覆盖全部H扩展、trace、SMT或全部ISA边界。idealInstructionFetch显式拒绝trace与多线程组合。

## 待完成

0.3c完整145点的baseline与高资源R0对照仍运行中。下一步在同一被测SHA上跑Backend、Frontend、Combined完整对照，保留每个benchmark的回退，并在组合后重新用TEA/计数器分析剩余瓶颈。最终26–28目标及整体报告尚未完成。

原始局部数据见[data/local-probes.csv](data/local-probes.csv)；命令、完整stats、config与日志位于 /tmp/tea-upperbound-20261028 下对应目录。标准可重复验证入口为 util/xs_scripts/upperbound_smoke.py。最后增补的构造参数保护只拒绝trace组合，普通FS测试路径不变。
