# astar_rivers_8610：完整 CI 末窗口残余证据

同一 0.3c 切片、各归档两个完整块中的最后块；`R7 C` 即 `R1-frontend`，不重复当独立实验。各组约 20M 提交，JSON 保存完整计数器名、原值、描述、版本与归档：[astar-ci-residual-notes.json](astar-ci-residual-notes.json)。真实 ELF 是 `astar_rivers.elf`，热点及可读源定位另见 [actual-source-hotspot-map.md](actual-source-hotspot-map.md)；此处不使用 biglakes 数据。相关性不等于因果，新的 TEA 和动态事件链仍未完成。

| 配置 | IPC | CPI | 提交分支错误 | 条件分支错误 | RAW replay | RAW 队列满周期 | IEW IQFull 事件 |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline24 | 4.73706 | 0.211101 | 48222 | 45367 | 2040 | 2399 | 685445 |
| R1 frontend / C | 4.64588 | 0.215245 | 47746 | 44663 | 23374 | 11818 | 769620 |
| R4 四 load + FE | 4.75646 | 0.210241 | 47530 | 44251 | 78831 | 36968 | 591349 |
| R5 四 load + 窗口 + FE | 4.97594 | 0.200967 | 47426 | 43834 | 441 | 2383 | 1110195 |
| R7 A 单供给正常气泡 | 4.77028 | 0.209631 | 48340 | 45464 | 1889 | 1028 | 688806 |
| R7 B 双供给正常气泡 | 4.61867 | 0.216512 | 47430 | 44605 | 22671 | 11222 | 751594 |

**方向与恢复次数不能单独解释 FE 回退。** 条件分支总数各侧均 2,825,108，baseline→FE 条件错误 45,367→44,663；总体提交分支错误也下降 48,222→47,746。`branchClassMisses` 其它类在这点 direct/indirect call 和 jump 均 0、return 均 1；这些分类计数与提交分支错误不是同一个统计集合，不能强行要求相等。R7 A→B IPC 4.77028→4.61867，同时条件错误 45,464→44,605，正常 override bubble 441,949→528,133；C 的 overrideBubbleNum 为 0、IPC 4.64588。该序列指出双供给背景变化与气泡/其他压力可能相关，仍不证明气泡是净回退原因。

**RAW 队列压力值得单独检验。** baseline→FE 的 RAWReplay 2,040→23,374、物理满周期 2,399→11,818；R4 的 RAWReplay 78,831，R5 扩窗后 441。代码 `src/cpu/o3/lsq_unit.cc:1753-1758` 在 RAW 队列满的 replay 路径累加 RAWQueueFull；`:3440-3445` 单独按实际队列满计周期。前者是重试事件，后者可与其他压力重叠，不可加成净 CPI。R5 同时改变 ROB、PRF、LQ、SQ、RARQ、RAWQ 等多个容量，因此不能把恢复归因到 RAWQ。最有区分度的下一单轴是固定 R4 四 load + FE，其余不变，`RAWQEntries` 从 56→128；核对实际 INI、同版本 diff、difftest，比较 IPC、RAW replay/full cycles 和目标 PC 的 load-to-branch/retirement 时间，再用一个保护点反证。

**平均 cache miss 不能解释成 cache 全面改善或恶化。** baseline/FE/R4/R5 的 DCache `ReadReq.misses::cpu.data` 分别 206,072/204,725/215,267/226,617；累积 `missLatency::cpu.data` 分别 337,024,971/332,572,761/352,947,033/366,569,730 ticks，均值约 1,636/1,624/1,640/1,618 tick。FE 轻微减少需求 miss 与均值仍回退，R5 miss 反增仍加速；累计 miss 服务可并发，不是 stall 分解。BankConflictReplay 为 594,712/565,535/719,583/735,053；STLFReplay 为 114,932/188,689/185,184/206,116，均为可能重复的事件，不可判定净因果。下一步用正式窗口 TEA 锚定 `wayobj::makebound2` PC，跟踪 load→branch resolve、队列进入/退出、replay 和状态，区分真实关键服务与提前执行。

**窗口指标边界。** PRF `fullRegistersEvents` 六侧均 0；IEW `LSQFull` 均 0，不能据此声称 LQ/SQ 从不构成依赖或后端限制。ROBFull 分别 0/92/91/0/126/92，是事件数非独占周期；FE 整数 IQ0 平均占用 9.468，R4 为 9.297，R5 为 11.007，容量变化与动态排队共同影响平均值。IEW IQFull 表示被调度资源约束的事件，R5 反而更多（1,110,195），不能由此认定该配置更慢。`src/cpu/o3/issue_queue.cc:1265-1266` 的 portBusy 在遍历候选被拒时累加，尽管注释称 busy cycles，它不是 FU 独占忙周期；不要将其除 cycles 作为利用率。动态 PC、实际重放等待和资源竞争链仍缺。
