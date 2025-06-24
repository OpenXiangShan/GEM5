1. TopDown 分析（性能计数器中的 TopDown）

       通过Topdown分析初步定位性能差异的原因，并针对性地比较可能引发该原因的PMU counter，进行初步的根因分析；但由于当前的根因分析一般缺少严格的量化分析，因此建议作为参考；

       如果通过topdown分析无法继续进行，则需要进行下面的分析：

2. Trace 分析

找到 Trace 中的热点函数，热点函数占整体的比例，以及 RTL 与 GEM5 在每一轮热点函数中的提交周期差异

使用热点循环每一轮提交周期的差异 * 热点循环轮数找到这一段热点函数对整体性能差异的占比

使用工具：

    - （RTL/GEM5）perfcct [https://bosc.yuque.com/yny0gi/sggyey/mc8ybawwh10mg1zc](https://bosc.yuque.com/yny0gi/sggyey/mc8ybawwh10mg1zc)
    - （RTL/GEM5）热点函数分析工具 [https://bosc.yuque.com/yny0gi/sggyey/idg8foneqhxn6ufn](https://bosc.yuque.com/yny0gi/sggyey/idg8foneqhxn6ufn)
3. IPC rolling 分析？

找到 IPC 差异最大的一段区间

使用工具：

    - （RTL）rolling db [https://github.com/OpenXiangShan/XiangShan/pull/2319](https://github.com/OpenXiangShan/XiangShan/pull/2319)，[https://github.com/OpenXiangShan/XiangShan/pull/2228](https://github.com/OpenXiangShan/XiangShan/pull/2228)
    - （GEM5）rolling db [https://github.com/OpenXiangShan/GEM5/commit/fea0a48a2cfcfb6beb04ddd945f7a52c1e138a73](https://github.com/OpenXiangShan/GEM5/commit/fea0a48a2cfcfb6beb04ddd945f7a52c1e138a73) [https://github.com/OpenXiangShan/GEM5/commit/cd94031bf9b1294c6b099201140c7c1bcc818e38](https://github.com/OpenXiangShan/GEM5/commit/cd94031bf9b1294c6b099201140c7c1bcc818e38)
4. 波形与 Trace 对比

针对热点循环的一轮，对比 RTL 与 GEM5 的 Trace/波形 找出差异来源



注：

+ 2/3 可以并行开展
+ 若 3 不好开展，可以考虑使用 2 分析



一些参考：

+ RTL MemBlock 的关键性能计数器含义： [https://bosc.yuque.com/yny0gi/ngb8pp/rovbe0cs7muy4vne#9hIH](https://bosc.yuque.com/yny0gi/ngb8pp/rovbe0cs7muy4vne#9hIH)
+ Gem5 TopDown：[https://bosc.yuque.com/yny0gi/sggyey/qgpexvcq9gnr97w8](https://bosc.yuque.com/yny0gi/sggyey/qgpexvcq9gnr97w8)

