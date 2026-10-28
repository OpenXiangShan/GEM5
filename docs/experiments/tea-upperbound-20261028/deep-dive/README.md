# 12宽最终组合：深入分析入口

完整0.3c结果已收尾：原基线26.72579，理想前端28.99774，理想前端＋4load＋容量30.20846。正式分数以[主报告](../report.md)与[CI索引](../runs.json)为准。专题研究的组合固定12宽、原整数FU、4load/2store与七项容量扩容，不使用旧16宽整包替代。

## 当前证据

- [旧报告的方法](method-reference.md)：先找重要程序/相位，再从热点PC回到依赖链，结合正交计数器与单轴反证；旧1.0c、8事件结果只用于线索。
- [源码与ELF溯源](source-provenance.md)：用户已开放实际构建目录；当前源码可读，但10月重建产物不是8月checkpoint的精确ELF，应用也未启用`-g`。
- [实际源码与指令结构](actual-source-hotspot-map.md)：mcf两条指针链、perlbench正则匹配分发、astar rivers网格邻居检查。精确指令与源码语义匹配分别标注。
- [完整CI代表点计数](../data/final-twelve-ci-counters.json)：mcf_6753、perlbench_checkspam_5886、astar_rivers_8610，同一正式20M窗口的基线、前端、4load与容量组合。
- [翻译计数的反例](mcf-translation-counter-notes.md)：约6倍initial lookup miss并不等于6倍PTW请求、替换或唯一延迟指令；不能按单个累计计数选择优化方向。

## 新TEA采集与验收

新的基线/最终组合配对TEA正在采集，固定上述三个0.3c代表点，20M预热＋20M正式测量、当前九事件采样器、period=1，启用difftest。脚本还采集mcf两侧及perlbench组合的TEA关闭对照。

[八组完整CI的1,160点窗口复核](../data/full-ci-window-audit.md)全部通过。原本地2M消融实际未完成预热，已降级为冷启动探针，详见[纠正审计](../data/local-window-correction-audit.md)。本轮不能只凭maxinsts正常退出或TEA守恒宣称窗口有效：总上限40M、两个完整统计块、预热重置、末块20M以及与相同CI统计逐值匹配均需满足。

```mermaid
flowchart LR
    A[完整加权收益与回退] --> B[匹配相位与正式窗口]
    B --> C[新TEA状态和热点PC]
    C --> D[精确ELF依赖链]
    D --> E[源码语义及正交计数]
    E --> F[单轴干预与反例]
    F --> G[净周期和完整加权复核]
```

PC总权重含Computing，PSV事件可重叠；CSV没有PC×四状态联合字段。计数器先核对定义与重复计数路径，再与净周期对照。下一步专题将在新TEA窗口验收后补充实际热点、残余瓶颈和优先级，不预填尚未获得的结果。
