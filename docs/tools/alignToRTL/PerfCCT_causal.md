# PerfCCT 因果证据 MVP

PerfCCT 在原生命周期记录上增加动态身份、事件和提交判定区间，提供
**采集与只读证据查询**。它用于追踪已观察的局部机制，不自动
证明根因、关键路径或可恢复 IPC。首轮机制验收以单线程标量为主。

## 采集与运行范围

在支持 ArchDB 的 O3 SE 或 checkpoint 配置命令后添加：

```sh
--enable-arch-db --arch-db-file=/absolute/path/trace.db --arch-db-dump-causal
```

causal 同时启用 lifetime；仅采原表时使用 `--arch-db-dump-lifetime`。
OFF 性能对照不添加上述 tracing 开关。采集从运行开始，不随 stats reset
或 `dumpGlobal` 切换；查询 ROI 不会缩短已写入的数据。

切片可使用以下窗口；`--maxinsts` 是总指令上限，须确认实际两段统计：

```sh
--warmup-insts-no-switch=5000000 --maxinsts=10000000
```

固定 binary、配置、checkpoint、REF 和 ROI，并记录命令及 SHA256。
GCPT 的 difftest REF 按仓库运行流程解析，不以旧路径仍可读作为兼容证明。
较长运行的 SQLite 体积和查询耗时可能很大，先估算预算。

## Schema、身份和事件

查询 `schema` 获得实际版本、列、时钟与采集范围，不只根据文件名推断能力。
schema 2/3 增量缺失时，查询器保留 unavailable/unknown。

| 表 | 内容 |
|---|---|
| `PerfCCTMeta` | schema、时钟、身份映射和采集范围 |
| `PerfCCTInst` | `(Cpu,TID,SeqNum)`、PC、反汇编、终止状态、`CommitID` |
| `PerfCCTEvent` | tick、attempt、事件、原因、`RelatedSeq`、`ReasonMask` |
| `PerfCCTCommitSpan` | 提交判定区间、head、实际 blocker、已提交数量 |
| `PerfCCTCacheEvent` | L1D 请求、MSHR、target、拒绝和持有者快照 |
| `PerfCCTFetchTransfer`（可选） | 单线程部分传送末条指令锚点及空槽 |

完整指令身份必须包含 CPU/TID，不能把原表自增 ID 当成 SeqNum。
`CommitID` 连接 `LifeTimeCommitTrace.ID`；未提交对象可能没有该字段。
拆分 store 内部操作共享 fetch SeqNum。融合对象通过 `fused_into`/
`EndKind` 的 `RelatedSeq` 指向保留身份；保留对象的反汇编更新为融合结果。
同 tick 按事件 `ID` 排序；相关身份未知时不能按地址或时间补造关系。

`[StartTick,EndTick)` 是半开区间，所有时间为 gem5 tick。转换 cycles
须核对 CPU 时钟。`SampleCycles` 是实际判定次数，ROI 裁剪不会同比缩放。
`Committed=0` 不一定是 LSU 阻塞；`empty`、`squash` 等仍保留原状态。
`Committed>0` 是 partial，并不保证发生失败；`commit_window_exhausted`
可能表示正常用满窗口。只有 `group_not_ready` / `commit_head_blocked`
明确记录 readiness/提交判定失败。blocker 是首个观察到的失败成员，
不是唯一关键成员。`trace_end` 保留末样本与未闭合对象；`gap` 是不连续
采样，均不能解释为等待已解除或持续阻塞。

load/STA 的 `attempt_begin` 标记执行尝试，不是 Request 身份。
`replay` 保留实际原因与 bit mask，位含义由元数据解码。
`wait_begin` / `wake` 是已观察等待及唤醒来源；`cache_hint` 不等于数据
完成，`translation_observed_complete` 是 IQ 观察时间。`replay_enqueue`
不等于实际重试，`response.Attempt` 不能严格配对请求生命周期。
`dependency` 是 rename 值来源，含源角色及 ready 状态；推测唤醒不作为
值完成证据。`store_data_ready` 来自 STD，不能替代 STA/提交条件。
IQ 选择、取消、端口受阻事件保留各次观察，不重建完整 readiness 区间。

资源身份 `(Cache,MSHR)` 使用递增 generation，不是地址或槽号。
RequestID 区分 Request 对象，TargetID 在 cache 内区分逐 target 占用。
通过 metadata 中的 requestor/context 精确关联动态指令；未知请求不猜。
`reject` 是实际 admission 失败；`blocked` 状态或高占用本身不是损失。
`ParentID` 连接拒绝与 owner/target_owner 快照，快照是候选持有者集合。
`send` 只是 in-service，`release` 不保证 fake-mainpipe credit 已归还。
历史成员不等于拒绝时仍占用的 target；未闭合与截断必须保留。

## 只读查询入口

所有查询使用 SQLite 只读连接；原 `util/perfcct.py` 仍提供生命周期视图。
先确认 schema 与采集参数，再选提交窗口并追查完整动态身份：

```sh
python3 util/perfcct_query.py trace.db schema
python3 util/perfcct_query.py trace.db stalls --kind zero --top 10
python3 util/perfcct_query.py trace.db stalls --reason group_not_ready --reason commit_head_blocked
python3 util/perfcct_query.py trace.db stalls --kind partial --start 100000 --end 200000
python3 util/perfcct_query.py trace.db inst 1234 --cpu system.cpu --tid 0
python3 util/perfcct_query.py trace.db chain 1234 --cpu system.cpu --tid 0 --depth 8 --nodes 64
python3 util/perfcct_query.py trace.db resources --reject 100
python3 util/perfcct_query.py trace.db resources --cache system.cpu.dcache --mshr 12 --at 300000
```

输出 JSON，`stalls --reason` 可重复使用、取原因并集；默认保留全部原因。
`stalls` 按 ROI 内的 `covered_ticks` 排序，同时保留原始起止与未闭合状态。
`--start` 包含起点，`--end` 不包含终点；未指定 CPU/TID 可能匹配多个身份。
`inst` 只筛选事件/区间，始终保留匹配指令的身份元数据及原始 lifetime。
指令作为 head 或 blocker 的区间分别返回；不能只查 head 推断组内成员。
局部事件数量有上限，保留截断标记；旧库缺少可选表时明确 unavailable。

`chain` 仅跟随已记录的 rename dependency/STLF 关系，保留证据 ID，
限制深度与节点数。可达关系不等于关键路径；不能推断唯一或最后到达的前驱。
`resources` 支持 `--request-id`、`--target-id`，以及 `--at` 的 target 状态。
重建依赖实际 allocate/merge/removal/replacement 事件；不把 owner 快照
当作完整生命周期，缺事件时保持 incomplete，旧 schema 保持 unavailable。
资源观察限定 L1D，不能从一次下层 response 补造 L2/DRAM 服务过程。

## 资源事件的读法

| 事件 | 观察意义 |
|---|---|
| `allocate` / `merge` | 首请求分配 / CPU请求加入现有 MSHR |
| `send` / `retry` | 转为 in-service / 再次 pending，不等于 DRAM 发出 |
| `response` / `release` | 下层响应 / MSHR将归还，不保证所有占用已解除 |
| `blocked` / `unblocked` | 原因位变化，不等于有请求损失周期 |
| `reject` / `port_retry` | CPU admission 实际失败 / CPU端口重试通知 |
| `owner` / `owner_credit` | 拒绝时条目与 credit 持有者快照 |
| `target_add` / `target_remove` | cache内 target 的占用变化 |
| `credit_hold` / `credit_release` | fake-mainpipe credit 占用 / 归还 |
| `open_mshr` / `open_credit` / `open_target` | trace end 时未闭合占用 |

`BlockedMask` 在状态转换前取样；原因位0/1/2为 NoMSHRs/NoWBBuffers/
NoTargets。allocate 的 Allocated 是分配后数量，release 是释放前数量，
credit 的 HeldCredits 是变化后数量。NoMSHRs 快照可能同时含已分配 MSHR
和 fake-mainpipe credit，NoTargets 快照关联限制 target 的 MSHR。
资源事件不具有通用 CPU/TID 字段：按 metadata 做请求映射，不把所有
owner 限于 ROI 中创建的指令；对象可能较老或来自错误路径。

## OFF/ON 验收

先确认正常终止或明确的 maxinst 退出、guest 结果/Difftest、完整 stats，
再比较相同窗口的 OFF/ON。逐项比较全部非 host stats，排除 host 速率、
RSS 和 wall 时间；warmup 与 measurement 分开比较，不只看 IPC。
核对 schema/metadata、trace end 与退出 tick、身份关联、时间段连续性、
原 lifetime 表兼容性及必要的事件覆盖；SQLite 自洽不证明事件齐全。
记录 binary/REF/配置/checkpoint 指纹；不同程序/窗口的计数不能混比。
不把尚未正常完成的运行、少量SQL读成功或exit0单独称为完整验收。

```sh
python3 -m unittest discover -s util -p 'test_perfcct_query.py'
python3 -m unittest discover -s util/perfcct_workload -p 'test_*.py'
python3 util/perfcct_workload/check_trace.py trace.db
```

合成测试覆盖只读连接、ROI、身份隔离、缺表、未知事件、未闭合对象、
资源关联和截断。开发期受控 SE 与单核 GCPT 配对观察到 OFF/ON 非 host
stats 一致；结论仅限相应窗口，不证明任意配置无回归。微程序与独立
配对 runner 见 [README](../../../util/perfcct_workload/README.md)。

## 证据边界

Store 翻译与数据就绪事件不覆盖所有完成/提交条件；response.Attempt
不是请求代号。Pinned、多写者、非标量依赖等未覆盖路径保持 unknown。
长 replay/wait 可能还含唤醒后调度等待，不能把整个间隔归给选中的原因。
多个年轻指令可以是同一阻塞对象的受害者，重叠等待不能相加为损失周期。
`StoreL1Bound` 可来自已 issued、无 pendingCacheReq 的 ROB-head store；
`IQFull` 可混合容量/输入失败并传播到所有 dispatch lanes；均不证明 SB/L1 根因。
Fetch 部分传送锚点只定位传送批次，不表示造成空槽的分支或前端根因。
当前没有完整下层 cache/DRAM、前端恢复、非标量/SMT 的端到端因果链，
也没有反事实执行模型。状态、关系与局部时间交叠均不直接证明收益；
优化仍需单机制干预及匹配工作量的性能复验。
