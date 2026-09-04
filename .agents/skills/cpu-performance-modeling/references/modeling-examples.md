# CPU 性能建模示例

这些示例展示如何把复杂机制压成“资源 + 状态 + 事件 + 参数 + stats”。它们不是文件索引，也不是 RTL 逐行翻译。

## 目录

1. 端口 token + busy bitmap 的 issue 模型
2. dependency wakeup + replay 回流模型
3. LSQ load pipeline 的 replay reason 压缩
4. completion index + dequeue quota 的 RAR/RAW 模型
5. store buffer merge / forwarding / eviction 模型
6. cache/MSHR readyTime queue 和 prefetch admission
7. memory controller burst split / write queue merge / early response
8. memory controller window / threshold / turnaround 调度
9. 高准确度模型的检查清单
10. 从 RTL 机制到行为级模型的转换模板

## 1. 端口 token + busy bitmap 的 issue 模型

建模模式：用 ready queue、dependency wakeup、port busy bitmap 和 TimeBuffer 表示发射选择与执行延迟。

保留的性能后果：

- 每个端口每周期最多选择有限指令。
- 非流水化或长 latency 操作会占用未来端口。
- canceled、arb failed、mem replay 会重新进入控制流。
- port busy、issue distribution、retryMem 可归因。

粗化的细节：

- 不展开 RTL 仲裁信号。
- 不逐拍模拟 FU 内部数据通路，只保留端口占用和完成延迟。

```cpp
struct PortState {
    uint64_t busy_bits;  // bit n means the port is occupied n cycles later
};

void select_ready_insts(Cycle now) {
    selected.clear();

    for (PortId p = 0; p < num_ports; ++p) {
        ReadyQueue& q = ready_by_port[p];

        for (auto it = selector.begin(q, p); it != q.end();
             it = selector.next(it, p)) {
            Inst* inst = *it;

            if (inst->canceled()) {
                q.erase(it);
                stats.canceled++;
                continue;
            }

            int lat = op_latency(inst);
            uint64_t token = lat >= 63 ? UINT64_MAX : (1ull << lat);

            if ((ports[p].busy_bits & token) == 0) {
                q.erase(it);
                selected.push_back({p, inst});
                break;
            }

            stats.port_busy[p]++;
        }
    }
}

void schedule_selected() {
    for (auto [p, inst] : selected) {
        if (inst->arb_failed()) {
            ready_by_port[p].push(inst);
            stats.arb_failed++;
            continue;
        }

        inst->mark_scheduled();
        issue_pipe.push(inst);
        stats.issued[p]++;

        if (!is_pipelined(inst)) {
            ports[p].busy_bits = UINT64_MAX;
        } else if (op_latency(inst) > 1) {
            ports[p].busy_bits |= 1ull << op_latency(inst);
        }

        speculative_wakeup(inst);
    }
}

void tick_issue_model() {
    schedule_selected();
    issue_pipe.advance();

    for (auto& port : ports) {
        port.busy_bits >>= 1;
    }
}
```

建模要点：

- `busy_bits` 是控制流模型，不是 RTL 信号。它表示未来端口 token 是否可用。
- selector 可替换成 oldest-first、priority、bank-aware 等策略，但接口仍是“从 ready queue 拿 token”。
- 高准确度来自保留端口带宽、年龄选择、长 latency 占用、arb fail 和 replay 回流。
- 高性能来自每端口只扫描到第一个可发射候选，复杂度受端口数和 ready queue 策略约束。

## 2. dependency wakeup + replay 回流模型

建模模式：用依赖图唤醒消费者；访存失败后通过有限 replay queue 回到 issue queue。

保留的性能后果：

- 生产者完成会唤醒等待源操作数的消费者。
- mem dependency 未解决的指令不能进入 ready queue。
- 快速 replay 和慢 replay 可以走不同入口，但最后统一回到 ready 选择。

```cpp
void wakeup_dependents(Inst* producer, bool speculative) {
    if (speculative && producer->canceled()) {
        return;
    }

    for (Reg dst : producer->dest_regs()) {
        if (dst.fixed_mapping()) {
            continue;
        }

        for (auto [src_idx, consumer] : dep_graph[dst.index]) {
            if (consumer->src_ready(src_idx)) {
                continue;
            }

            consumer->mark_src_ready(src_idx);
            add_if_ready(consumer);
        }

        if (!speculative) {
            dep_graph[dst.index].clear();
        }
    }
}

void add_if_ready(Inst* inst) {
    if (inst->issued() || !inst->all_src_ready()) {
        return;
    }

    if (inst->is_mem_ref() && !inst->mem_dep_solved()) {
        return;
    }

    if (!inst->in_ready_queue()) {
        inst->ready_tick = now();
        ready_by_port[port_class(inst)].push(inst);
    }
}

void retry_mem(Inst* inst, ReplayReason reason) {
    inst->set_replay_reason(reason);
    replay_q.push(inst);
    stats.retry_mem++;
    stats.replay_reason[reason]++;
}
```

建模要点：

- dependency graph 是控制依赖，不是数据网络；只回答“消费者何时可进入 ready”。
- replay reason 必须解释不同等待行为，例如 bank conflict、MSHR arbitration、cache miss、RAR/RAW full。
- replay 入口要统一，否则容易出现某类重试绕过端口/队列约束。

## 3. LSQ load pipeline 的 replay reason 压缩

建模模式：load pipeline 在 send/recv 阶段将 bank conflict、cache blocked、MSHR fail、write buffer hit、RAR/RAW queue full、nuke、cache miss 等压成有限 replay reason。

保留的性能后果：

- 请求是否成功发送。
- 请求失败后走快 replay、慢 replay、cache miss wait 还是 dependency wait。
- replay 原因进入 stats 和性能归因。
- RAR/RAW 队列满会造成结构性 replay，而不是被普通 cache miss 覆盖。

```cpp
enum class ReplayReason {
    None,
    BankConflict,
    CacheBlocked,
    MshrArbFail,
    MshrAliasFail,
    HitInWriteBuffer,
    CacheMiss,
    RARFull,
    RAWFull,
    Nuke,
    TlbMiss,
};

SendResult try_send_load(Request* req) {
    SendResult r;

    if (bank_conflict_check && bank_conflicts(req->vaddr)) {
        r.reason = ReplayReason::BankConflict;
        return r;
    }

    if (!cache_port_available()) {
        r.reason = ReplayReason::CacheBlocked;
        return r;
    }

    if (!dcache.send_timing(req->packet)) {
        if (req->packet->mshr_arb_failed()) {
            r.reason = ReplayReason::MshrArbFail;
        } else if (req->packet->mshr_alias_failed()) {
            r.reason = ReplayReason::MshrAliasFail;
        } else if (req->packet->hit_in_write_buffer()) {
            r.reason = ReplayReason::HitInWriteBuffer;
        } else {
            r.reason = ReplayReason::CacheBlocked;
        }
        return r;
    }

    mark_cache_port_busy();
    req->mark_sent();
    r.sent = true;
    return r;
}

void recv_load_data(Inst* load) {
    Request* req = load->request;

    bool cache_miss_replay =
        enable_load_miss_replay &&
        req->normal_load() &&
        !load->full_forwarded() &&
        !load->cache_hit();

    bool rar_full =
        load->is_normal_load() &&
        load->lq_index > load_completed_index + 1 &&
        rar_queue.size() >= max_rar_entries;

    bool raw_full =
        load->is_normal_load() &&
        load->sq_index > store_completed_index + 1 &&
        raw_queue.size() >= max_raw_entries;

    ReplayReason reason = first_reason({
        cache_miss_replay ? ReplayReason::CacheMiss : ReplayReason::None,
        rar_full ? ReplayReason::RARFull : ReplayReason::None,
        raw_full ? ReplayReason::RAWFull : ReplayReason::None,
        nuke_detected(load) ? ReplayReason::Nuke : ReplayReason::None,
    });

    if (reason != ReplayReason::None) {
        set_load_replay(load, reason);
        stats.load_replay[reason]++;
        enqueue_replay_destination(load, reason);
        cancel_load_pipeline(load);
        return;
    }

    complete_or_forward(load);
}
```

建模要点：

- reason 集合要按性能后果划分，而不是按 RTL 信号划分。
- `CacheBlocked`、`MshrArbFail`、`MshrAliasFail` 都是“没发出去”，但它们归因不同，必要时应分开。
- RAR/RAW full 是完成/释放速率问题，不能合并进 cache miss。
- replay 的重新进入点必须再次经过资源仲裁，否则会高估吞吐。

## 4. completion index + dequeue quota 的 RAR/RAW 模型

建模模式：维护 load/store completed index；RAR/RAW 队列按每周期 dequeue width 释放。

保留的性能后果：

- 完成宽度限制会拖慢依赖解除。
- RAR/RAW queue 满会触发 replay。
- replay latency 可以按 entry tick 统计。

```cpp
void update_completion_indices() {
    for (int i = 0; i < load_completion_width; ++i) {
        Entry* next = lq.entry(load_completed_index + 1);
        if (next && next->inst->executed()) {
            load_completed_index++;
        } else {
            break;
        }
    }

    for (int i = 0; i < store_completion_width; ++i) {
        Entry* next = sq.entry(store_completed_index + 1);
        if (next && (next->addr_ready() || next->can_writeback())) {
            store_completed_index++;
        } else {
            break;
        }
    }
}

void drain_rar_raw_queues() {
    int rar_budget = rar_dequeue_per_cycle;
    for (auto it = rar_queue.begin(); it != rar_queue.end() && rar_budget > 0;) {
        if ((*it)->lq_index <= load_completed_index + 1) {
            it = rar_queue.erase(it);
            rar_budget--;
        } else {
            ++it;
        }
    }

    int raw_budget = raw_dequeue_per_cycle;
    for (auto it = raw_queue.begin(); it != raw_queue.end() && raw_budget > 0;) {
        if ((*it)->sq_index <= store_completed_index + 1) {
            it = raw_queue.erase(it);
            raw_budget--;
        } else {
            ++it;
        }
    }
}

void process_structural_replay() {
    vector<Inst*> ready;

    collect_replay_if_unblocked(rar_replay_q, load_completed_index,
                                rar_dequeue_per_cycle, ready);
    collect_replay_if_unblocked(raw_replay_q, store_completed_index,
                                raw_dequeue_per_cycle, ready);

    for (Inst* inst : ready) {
        sample_replay_latency(inst);
        inst->clear_replay();
        retry_mem(inst, inst->last_replay_reason());
    }
}
```

建模要点：

- completion index 把“有序完成前缀”变成 O(width) 更新。
- dequeue quota 直接表达硬件吞吐上限。
- replay queue 不应无限立即释放；释放速度本身就是性能模型的一部分。
- stats 至少需要 full cycles、avg occupancy、replay count、latency sample。

## 5. store buffer merge / forwarding / eviction 模型

建模模式：store buffer 使用 block 地址 hash、free list、LRU、valid mask、vice entry 表示合并、转发和写回。

保留的性能后果：

- 同 cache line store 可 merge，减少写回请求。
- load 可从 store buffer full/partial forward。
- 新旧 store 覆盖关系影响返回数据和是否还要访问 cache。
- store buffer 满、SQ 将满、flush、timeout 会触发 eviction。
- 写回失败会 blocked 并稍后 retry。

粗化的细节：

- 不模拟 store 数据在每个 RTL stage 的搬运。
- 对性能无关的数据字节只作为 mask/coverage 的载体。

```cpp
struct StoreBufferEntry {
    ThreadId tid;
    Addr block_addr;
    ByteMask valid;
    Bytes data;
    bool sending;
    StoreBufferEntry* newer_same_line;
};

void insert_or_merge_store(Store* st) {
    Addr block = block_addr(st->paddr);
    int offset = line_offset(st->paddr);

    StoreBufferEntry* e = sbuf.find(st->tid, block);
    if (e && !e->sending) {
        e->merge(offset, st->data, st->byte_mask);
        sbuf.touch(e);
        stats.sbuffer_merge++;
        return;
    }

    if (e && e->sending) {
        StoreBufferEntry* vice = sbuf.allocate_vice(e);
        vice->reset(st->tid, block, offset, st->data, st->byte_mask);
        stats.sbuffer_vice++;
        return;
    }

    StoreBufferEntry* empty = sbuf.allocate_free();
    empty->reset(st->tid, block, offset, st->data, st->byte_mask);
    sbuf.insert(empty);
}

ForwardResult forward_from_sbuffer(Load* ld) {
    StoreBufferEntry* e = sbuf.find(ld->tid, block_addr(ld->paddr));
    if (!e) {
        return ForwardResult::Miss;
    }

    bool full = true;
    for (Byte b : ld->bytes()) {
        if (e->newer_same_line && e->newer_same_line->valid[b.offset]) {
            ld->record_forward_byte(b, e->newer_same_line->data[b.offset]);
        } else if (e->valid[b.offset]) {
            ld->record_forward_byte(b, e->data[b.offset]);
        } else {
            full = false;
        }
    }

    if (full) {
        stats.sbuffer_full_forward++;
        return ForwardResult::Full;
    }

    stats.sbuffer_partial_forward++;
    return ForwardResult::Partial;
}

void maybe_evict_sbuffer() {
    if (blocked_entry) {
        retry_blocked_sbuffer();
        return;
    }

    optional<EvictCause> cause;
    if (flushing) {
        cause = EvictCause::Flush;
    } else if (sbuf.unsent_size() > evict_threshold) {
        cause = EvictCause::Full;
    } else if (any_sq_will_full()) {
        cause = EvictCause::SQFull;
    } else if (inactive_cycles > inactive_threshold) {
        cause = EvictCause::Timeout;
    }

    if (!cause) {
        inactive_cycles++;
        return;
    }

    StoreBufferEntry* victim = sbuf.lru_victim();
    Request* req = build_writeback(victim);

    stats.sbuffer_evict[*cause]++;
    if (!send_to_cache(req)) {
        blocked_entry = victim;
        stats.sbuffer_dcache_blocked++;
    } else {
        victim->sending = true;
        reset_inactive_cycles();
        stats.sbuffer_dcache_fire++;
    }
}
```

建模要点：

- `valid` mask 是必要数据，因为它决定 full/partial forward 的控制结果。
- hash lookup 避免 load 每次扫描整个 store buffer。
- LRU/free list 给出固定容量和确定 eviction。
- eviction cause 是性能归因的一部分，应区分记录。

## 6. cache/MSHR readyTime queue 和 prefetch admission

建模模式：cache MSHR/write buffer 用 allocated/free/ready list；ready list 按 readyTime 排序；prefetch 只有在 demand reserve 之外有空间时才进入。

保留的性能后果：

- 请求未到 readyTime 不会发送。
- MSHR/write buffer full 会 backpressure。
- writeback/read miss 的地址冲突会影响仲裁顺序。
- prefetch 不得占用 demand reserve。
- occupancy 用 entry * tick 积分，而不是采样猜测。

```cpp
template <class Entry>
class ReadyQueue {
    List<Entry*> allocated;
    List<Entry*> ready;  // sorted by ready_time
    List<Entry*> free;
    int usable_entries;
    int reserve_entries;

    Entry* allocate(Request* req, Tick ready_time) {
        Entry* e = free.pop_front();
        e->allocate(req, ready_time);
        allocated.push_back(e);
        insert_by_ready_time(ready, e);
        return e;
    }

    Entry* get_next(Tick now) {
        if (ready.empty()) {
            return nullptr;
        }
        if (ready.front()->ready_time > now) {
            return nullptr;
        }
        return ready.front();
    }

    void delay(Entry* e, Tick delta) {
        e->ready_time += delta;
        ready.erase(e);
        insert_by_ready_time(ready, e);
    }
};

bool can_allocate_prefetch() {
    return allocated < total_entries - reserve_entries - demand_reserve - 1;
}

QueueEntry* get_next_cache_request() {
    MSHR* miss = mshr_queue.get_next(now());
    WriteEntry* write = write_buffer.get_next(now());

    if (write) {
        MSHR* older_conflict = mshr_queue.find_pending_conflict(write);
        return older_conflict && older_conflict->order < write->order
            ? older_conflict
            : write;
    }

    if (miss) {
        WriteEntry* write_conflict = write_buffer.find_pending_conflict(miss);
        return write_conflict ? write_conflict : miss;
    }

    if (prefetcher.has_packet() && can_allocate_prefetch() && !cache_blocked()) {
        Packet* pf = prefetcher.get_packet_if_tag_port_available();
        if (!pf) {
            stats.prefetch_tag_read_fail++;
            return nullptr;
        }

        if (cache_or_queue_already_has(pf->block_addr())) {
            stats.prefetch_dropped++;
            delete pf;
            return nullptr;
        }

        return allocate_mshr_for_prefetch(pf);
    }

    return nullptr;
}
```

建模要点：

- `readyTime` 是高性能时间抽象，避免每周期重算所有请求是否可发。
- demand reserve 是行为模型：保护 demand request，不是单纯容量数值。
- prefetch admission 应显式建模 drop/hit-in-cache/hit-in-MSHR/hit-in-WB，否则会误估队列压力。
- occupancy 积分适合解释长期 MSHR 压力。

## 7. memory controller burst split / write queue merge / early response

建模模式：memory controller 将系统请求拆成 burst；read 先查 write queue，若所有 burst 都由 write queue 满足则只付 frontend latency 并提前返回；write 可按 burst merge 并立即对上游响应。

保留的性能后果：

- 大请求变成有限 burst，占用多个 queue entry。
- write queue 命中可让 read 提前返回。
- partial serviced burst 仍需等待 memory。
- write merge 减少后端 burst 压力。

```cpp
bool add_read(Packet* pkt) {
    int burst_count = count_bursts(pkt->addr, pkt->size, bytes_per_burst);
    int serviced_by_writeq = 0;
    BurstHelper* helper = burst_count > 1 ? new BurstHelper(burst_count) : nullptr;

    for (Burst b : split_to_bursts(pkt)) {
        if (write_queue_covers(b.addr, b.size)) {
            serviced_by_writeq++;
            stats.read_serviced_by_writeq++;
            continue;
        }

        if (read_queue_full(1)) {
            stats.read_queue_full++;
            return false;
        }

        MemPacket* mp = decode_burst(pkt, b, Read);
        mp->ready_time = MaxTick;
        mp->helper = helper;
        read_queue[mp->qos].push_back(mp);
        stats.read_bursts++;
    }

    if (serviced_by_writeq == burst_count) {
        respond_after(pkt, frontend_latency);
        return true;
    }

    if (helper) {
        helper->bursts_serviced = serviced_by_writeq;
    }
    return false;
}

void add_write(Packet* pkt) {
    for (Burst b : split_to_bursts(pkt)) {
        Addr burst_addr = align_to_burst(b.addr);

        if (write_queue_index.contains(burst_addr)) {
            stats.merged_write_bursts++;
            continue;
        }

        if (write_queue_full(1)) {
            stats.write_queue_full++;
            mark_retry_write();
            return;
        }

        MemPacket* mp = decode_burst(pkt, b, Write);
        mp->ready_time = MaxTick;
        write_queue[mp->qos].push_back(mp);
        write_queue_index.insert(burst_addr);
        stats.write_bursts++;
    }

    respond_after(pkt, frontend_latency);
}
```

建模要点：

- split helper 是控制流状态：跟踪整个请求何时完整完成。
- read-by-write-queue 是高准确度关键点，直接改变延迟和 queue pressure。
- write merge 是性能模型，不只是功能优化。
- 队列 full 应阻止接收并触发 retry，而不是悄悄扩容。

## 8. memory controller window / threshold / turnaround 调度

建模模式：memory controller 是系统级性能模型，用 read/write 阈值、minimum bursts per switch、FR-FCFS/FCFS、command window 和 nextReqEvent 表示后端吞吐和读写切换。

保留的性能后果：

- read/write bus state 决定下一类请求。
- high/low threshold 和 min reads/writes per switch 提供 hysteresis。
- command window 限制同窗口命令数。
- FR-FCFS 保留 row hit 优先，FCFS 保留年龄优先。
- turnaround gap 影响切换后的第一个请求。

```cpp
enum class BusState { Read, Write };

void process_next_request() {
    bool switched = bus_state != next_bus_state;
    bus_state = next_bus_state;

    if (memory_busy()) {
        return;
    }

    if (bus_state == BusState::Read) {
        if (readq.empty()) {
            if (!writeq.empty() && writeq.size() > write_low_threshold) {
                next_bus_state = BusState::Write;
                schedule_next(now());
            }
            return;
        }

        MemPacket* rd = choose_next_read(
            switched ? write_to_read_gap() : 0);

        if (!rd) {
            return;
        }

        Tick cmd_at = issue_burst(rd);
        response_queue.insert_by_ready_time(rd);
        readq.erase(rd);
        reads_this_turn++;

        if (writeq.size() > write_high_threshold &&
            (reads_this_turn >= min_reads_per_switch || readq.empty())) {
            next_bus_state = BusState::Write;
        }
    } else {
        MemPacket* wr = choose_next_write(
            switched ? read_to_write_gap() : 0);

        if (!wr) {
            return;
        }

        Tick cmd_at = issue_burst(wr);
        write_queue_index.erase(wr->burst_addr);
        writeq.erase(wr);
        writes_this_turn++;

        bool below_low =
            writeq.size() + min_writes_per_switch < write_low_threshold;

        if (writeq.empty() ||
            below_low ||
            (!readq.empty() && writes_this_turn >= min_writes_per_switch)) {
            next_bus_state = BusState::Read;
        }
    }

    schedule_next(max(next_req_time, now()));
}

Tick reserve_command_window(Tick wanted, int commands_needed) {
    Tick window = align_down(wanted, command_window);

    while (!has_command_slots(window, commands_needed)) {
        window += command_window;
    }

    reserve_slots(window, commands_needed);
    return max(wanted, window);
}
```

建模要点：

- `bus_state` 和 `next_bus_state` 表示读写切换控制流，应作为独立状态建模。
- threshold/hysteresis 是性能准确度关键点，决定写压力何时反压读。
- command window 是带宽模型：只记录窗口内命令数量，不模拟完整 command bus RTL。
- `schedule_next` 让模型事件驱动，避免空转。

## 9. 高准确度模型的检查清单

新增模型完成后逐项检查：

1. **趋势**：增大队列、宽度、带宽、阈值、延迟时，stats 和 IPC 趋势是否符合预期。
2. **归因**：每类 replay/block/drop/merge/evict/turnaround 是否有独立或可解释的统计。
3. **回流**：失败请求是否回到同一条资源仲裁路径，而不是绕过瓶颈。
4. **背压**：queue full、cache blocked、port busy 是否会阻止上游继续前进。
5. **释放**：entry/token/busy bit 是否在完成、取消、squash、retry 成功后释放。
6. **顺序**：age/order、completion index、split helper 是否保留必要顺序。
7. **复杂度**：热路径扫描是否受参数上限约束；是否可用 hash、ready list、index 替代。
8. **数据最小化**：保留的数据是否都服务于 forwarding、ordering、异常、functional correctness 或 stats。

## 10. 从 RTL 机制到行为级模型的转换模板

```text
1. 列 RTL 机制的外部性能后果，而不是列信号。
2. 把后果归类为 progress、stall、retry、latency、bandwidth、fairness。
3. 找出最小控制状态：resource token、ready time、queue occupancy、reason enum、age/order。
4. 找出必须保留的数据：mask、block addr、split id、sequence、QoS。
5. 设计参数面：width/depth/latency/threshold/policy/switch。
6. 设计热路径算法和复杂度上界。
7. 设计 stats：每个状态转移、失败原因、队列压力、latency 都能观测。
8. 用 A/B 参数或最小 workload 验证趋势。
```

如果某个 RTL 信号无法映射到上述任一项，默认不进入性能模型；除非能证明它改变了可观测性能后果。
