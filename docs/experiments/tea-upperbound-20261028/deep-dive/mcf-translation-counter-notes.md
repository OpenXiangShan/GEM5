# mcf_6753 20M ROI：翻译计数边界

四组均为正式 CI 末尾完整 20M ROI；baseline、FE、4ldFE、W4ldFE 的 IPC 分别 1.1630、1.2138、1.2334、1.2603，branch MPKI 分别 6.8592、6.8856、6.8707、6.8610。以下是同一点固定退休区间的相关现象，不是 whole-suite 因果。

| 计数 | baseline | FE | 4ldFE | W4ldFE |
|---|---:|---:|---:|---:|
| DTLB initial lookup miss | 3,476,167 | 21,821,861 | 23,766,157 | 23,029,742 |
| DTLB read miss / write miss | 3,222,372 / 253,795 | 21,136,243 / 685,618 | 23,053,940 / 712,217 | 22,354,390 / 675,352 |
| L1 DTLB remove | 1,853,819 | 1,830,205 | 1,822,705 | 1,844,128 |
| DTB walker PTW memory requests | 1,100,855 | 1,092,234 | 1,094,830 | 1,093,318 |
| DTB walker in-flight busy cycles | 6,229,694 | 6,192,475 | 6,235,812 | 6,299,052 |
| load TLBMissReplay events | 2,266,328 | 2,301,398 | 2,425,709 | 2,431,855 |
| store TLB replay events | 232,131 | 232,314 | 230,835 | 230,340 |
| read-prefetch TLB lookups | 2,057,381 | 2,016,505 | 2,025,087 | 2,019,052 |

四项独立证据限制 6 倍初次 miss 的解读：PTW 接受内存请求数量基本稳定；L1 移除数量基本稳定；load/store TLB replay 仅温和变化；read prefetch 另行计数且数量稳定，不解释大幅增加的 non-prefetch initial 计数。它们不能证明每次查找均重复或 DTLB 容量毫无影响，也不能直接测得唯一页面或唯一 miss 请求。

`tlb.cc:2488-2495` 的 initial 是每次 `doTranslate` 中的首次非预取 L1 lookup，不是每个动态指令/请求的唯一首次访问；`pagetable_walker.cc:284-306` 的 PTW miss queue 可调用 `retryTimingPtwMiss`，经 `tlb.cc:1936-1942,2870-2918` 再进入 `doTranslate`，同一请求可能重复累计。四组 INI 都启用 PTW level limit，queue=40，level3/2/1=1、level0=6；这证明机制配置可达，现有统计没有 queue retry 专用计数，不能证明其具体占比。load translation pending 后 `lsq_unit.cc:1529-1537` 标记 TEA ST_TLB 并走 defer；`lsq.cc:3229-3252` 重入时可复用 savedRequest，因此不得把 lookup、TLB replay 和 ST_TLB 当成同一指标。

`readprefetchAccesses` 排除在 `l1InitialLookupMisses` 外（`tlb.cc:394-420,2487-2496`）。`ptwMemCount` 是成功发送的 PTW read packet 数，`ptwMemCycle` 是至少一请求在途的并集周期（`pagetable_walker.cc:81-92,319-325,511-521`），不是 CPU stall；现有 `ptwAvgMemLatency = ptwMemCycle / ptwMemCount` 并非逐请求 latency 求均值。多个请求可能重叠。

理想取指 `fetch.cc:3510-3516` 调用 functional translation 时显式 Execute；`arch/generic/mmu.hh:94-100` 先选择 ITB。注意 RISC-V `tlb.cc:3062` 在需翻译时仍显式使用 data walker，但该路径走独立 funcState 的 `sendFunctional`（`pagetable_walker.cc:444-449,847-873`），与 `sendTiming` 的 PTW timing 计数路径分开，直接 L1 refill 在 `!functional` 分支（1639 起）。因此不能说完全不碰 data walker；已检查路径不支持直接增加所列 DTLB initial lookup 或 timing PTW 计数。前端仍可能通过时序改变 data 指令推测/队列状态；需事件 trace 或计数插桩区分，不能凭现有数据断言重复查找是主要原因。可用更大 DTLB 单轴反证，并同时核对 IPC、initial misses、PTW/replay/TEA，注意压缩与复用；单次变化也无法独立区分容量和队列交互。
