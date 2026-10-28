# 第八轮 从完整点集压力选择Sbuffer和L2 bank单轴

本轮在已测得30.20846分的12宽组合上继续探索。89个INT点的正式统计提供了两个较明确的候选：bzip2的Sbuffer接收阻塞，以及mcf的L2数据bank重试。两项分别测试，缓存总容量、其余CPU参数和点集保持不变；当前CI尚未出分。

## 为什么选这两项

| 候选 | 正式CI证据 | 实验与反证 |
| --- | --- | --- |
| Sbuffer容量 | bzip2_source_3760无法接收条目323,182周期，约2.96%；combined_5764约2.54% | 只改Sbuffer16→32，驱逐阈值仍8；若阻塞下降但净周期不改善，不把满计数当作收益 |
| L2 data banks | mcf_12886的MCP2重试1,712,795次，6753为579,377次；astar/libquantum很少 | 只改data banks2→4，directory仍2；若数据重试下降但净周期不改善，可能不是关键服务或被其它约束接替 |

Sbuffer满/拒绝、队列平均占用和驱逐阈值计数不是同一个指标；L2请求重试可能重复、与其它活动重叠，也不是CPU损失周期。候选来自计数定义和多点对照，结果仍由完整加权分数决定。

[89点LSU压力](deep-dive/final-int-lsu-pressure.md)还发现omnetpp的RAW实际满约1.27%、h264ref某点RAR约0.62%。这些留作后续容量/回收速率候选，先不与本轮两个轴混在一起。[缓存压力与预取计数](deep-dive/final-memory-pressure.md)不足以证明应扩L2/L3总容量或关闭某个预取源。

## 配置与真实模型路径

共同基线是[当前12宽配置](best-config.md)，性能源码`36584adaf67da1e848d7b3a61d5ab18e84b2c12e`。两组只分别追加：

```text
# Sbuffer单轴
--param=system.cpu[0].SbufferEntries=32

# L2 data bank单轴
--param=system.l2_wrappers[0].data_sram_banks=4
```

两项均通过CoreMark 300k冷启动功能/参数检查：difftest开启、正常退出、12宽与目标参数核验通过。该检查不是SPEC得分。[检查记录](data/r8-coremark-smoke.json)

L2 bank参数不是仅修改wrapper显示值：实例化前配置覆盖进入C++共享配置，wrapper向每个slice绑定bank索引回调，[L2CacheWrapper.hh](../../../src/mem/cache/xs_l2/L2CacheWrapper.hh#L91)再由[L2MainPipe.cc](../../../src/mem/cache/xs_l2/L2MainPipe.cc#L99)用于MCP2冲突仲裁。data bank从`set_idx & 1`变为`& 3`；directory仍2，L2总32MiB、四slice和每slice16个MSHR不变，L3仍64MiB/64 MSHR。它改变并发资源，不扩总容量。尚未从该静态路径推断性能收益。

## 完整0.3c对照

| 实验 | Run | 当前状态 |
| --- | --- | --- |
| 最终12宽基线 | [40648104599](https://github.com/OpenXiangShan/GEM5/actions/runs/40648104599) | 145点完整，INT30.20846 |
| 仅Sbuffer32 | [40672006278](https://github.com/OpenXiangShan/GEM5/actions/runs/40672006278) | 已创建，执行中，尚未计分 |
| 仅L2 data banks4 | [40672028525](https://github.com/OpenXiangShan/GEM5/actions/runs/40672028525) | 已创建，执行中，尚未计分 |

workflow ref沿用`tea-main55-ci24-20261026`、workflow head `7d184a523b977d6c39a52feb155858919b2124f7`；真正被测源码由`branch`输入固定为上述36584。使用`kmhv3.py`、SPEC06 GCC16 RVA23 novec 0.3c全145点、20M预热＋20M正式测量、默认分布式服务器与每台32任务。精确payload见[runs.json](runs.json)，不以workflow head误代性能源码。

结果返回后核对归档身份、REF、profile、12宽与实际参数，再按标准工具计分。重点检验bzip2、mcf及弱影响保护点，保留负项。若暂时忽略少量慢点，先冻结相同完成集合和覆盖，最终用全量替代临时估分。
