# 第七轮 区分双供给与override气泡

为分离带宽和预测阶段等待，固定同一个12宽、ROB352、3load/2store后端，所有候选使用理想指令供给。

| 档位 | 每周期预测/取指片段数 | override气泡 |
| --- | --- | --- |
| A | 最多1/1 | 保留 |
| B | 最多2/2 | 保留 |
| C | 最多2/2 | 清除 |

B/A衡量保留气泡时双供给的条件效果，C/B衡量双供给背景下去气泡的条件效果。两者不同，也不能把统计的名义等待量直接当CPU可节省周期。

## 局部结果

两切片实际采用恢复后前约2M的冷启动窗口；原先“20M预热＋2M正式窗口”的标注已撤回，详见[窗口纠正](data/local-window-correction-audit.md)。均启用difftest，正常maxinsts退出。B和C复用第六轮的已验证数据；另一次同参数B运行逐值复现了周期和提交量，不作为独立样本。

| 切片 | A IPC | B IPC | C IPC | 双供给B/A | 去气泡C/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench_checkspam_5886 | 3.859484 | 4.352968 | 4.450973 | +12.786% | +2.251% |
| libquantum_69175 | 5.844837 | 5.830196 | 5.809664 | −0.251% | −0.352% |

perlbench此窗口的收益主要来自双供给；libquantum此点几乎不受益。该结论不能直接外推整个SPEC。原始普通前端到A的改变还包括理想取指路径，不能只按Icache/ITLB miss次数解释：请求响应、命中延迟和fetch buffer处理也同时变化。

## 完整对照

- [A：理想单供给、保留override](https://github.com/OpenXiangShan/GEM5/actions/runs/40651698971)。
- [B：理想双供给、保留override](https://github.com/OpenXiangShan/GEM5/actions/runs/40651748060)。
- C复用已完成的[理想前端CI](https://github.com/OpenXiangShan/GEM5/actions/runs/40641369657)，完整INT29.00。

A/B/C均已完成145点，CI通过，INT分别26.97364、28.79706、28.99774。相对原前端，A提高0.9274%；在A上双供给B提高6.7600%；在B上去气泡C再提高0.6969%。全部保持同一个12宽后端，不带4load或容量扩容。代码为36584；C的515eb与36584的新增4load逻辑默认关闭，后端配置一致。精确参数见[runs.json](runs.json)。

完整12项与统一coverage见[全量比较表](data/r7-full-comparison.md)、[JSON](data/r7-full-comparison.json)，B的最后两点与145点名单合并审计见[data/r7-B-full-audit.json](data/r7-B-full-audit.json)。以下143点暂定表仅保存提前分析过程，现以完整结果为准。

### 相同143点的暂定分解

按用户允许先忽略慢点的要求，固定四侧相同143个完成点重新加权。全部55输入都有覆盖，各benchmark的coverage完全一致；缺少`bzip2_source_3760`与`bzip2_source_16629`，其输入内原始权重分别为0.127347、0.1217。该表只用于配对比较，不能称为完整145点得分，也不能把其中B分数与完整A/C直接相除。

| 配对INT | 原前端 | A | B | C |
| --- | ---: | ---: | ---: | ---: |
| 143共同点暂定分数 | 26.84357 | 27.08645 | 28.92443 | 29.12108 |
| 相邻档位条件收益 | — | +0.905% | **+6.786%** | **+0.680%** |

| 已完整子项 | 理想单供给A/原前端 | 双供给B/A | 去气泡C/B |
| --- | ---: | ---: | ---: |
| perlbench | +0.732% | +22.052% | +0.336% |
| gcc | +2.285% | +10.016% | +0.943% |
| sjeng | +0.762% | +9.903% | +2.511% |
| xalancbmk | +0.536% | +12.972% | +0.611% |
| mcf | −0.115% | +3.704% | −0.058% |
| astar | +0.454% | −1.579% | +0.379% |

这里的双供给条件收益大于去气泡，支持优先研究带宽；并不意味着任一程序都如此，也没有证明真实硬件可以清除全部等待。短窗口perlbench去气泡为2.25%，完整子项为0.34%，说明局部阶段不能代替加权benchmark结果。A相对原前端的变化含理想取指整体路径，不仅是消除Icache/ITLB miss。

全部12子项与相同覆盖审计见[data/r7-common143-comparison.md](data/r7-common143-comparison.md)、[JSON原值](data/r7-common143-comparison.json)。

## 模型边界

正常override由最早与最终结果一致的预测stage决定等待，期间预测不能入FTQ。每个CPU tick最多老化一次等待。理想开关只将新生成的等待清零，并未实现真实ahead pipeline；阶段选择、训练与推测时序仍会相互影响。

`idealOverrideBubblesSaved`累加每次lookup被清零的名义等待量，同一tick可能累加两次，不能解释为净CPU周期。已有ABTB ahead结构也不足以证明全部stage零气泡或RTL时序可行。代码位置与详细定义见[模型边界](override-model-boundary.md)。

原始数据与哈希：[局部三档对照](data/override-separation-local.json)。
