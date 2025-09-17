# GEM5 cache 学习

因为后面需要做L3和多核的建模，所以需要先了解目前gem5的cache结构，随手整理一下里面的一些子模块的思路。首先是一些现成的技术博客，可以辅助着看一下。

GEM5 RubyCache CHI协议 [https://zhuanlan.zhihu.com/p/684413555](https://zhuanlan.zhihu.com/p/684413555)

GEM5内存系统概述[https://blog.csdn.net/zhenz0729/article/details/136092984](https://blog.csdn.net/zhenz0729/article/details/136092984)

## gem5的缓存系统
gem5具有两种缓存系统：“Classic”和“Ruby”，这是因为gem5的前身m5使用Classic缓存模型，GEMS使用Ruby缓存模型。

Classic的特点是能够简单快速地搭建起缓存系统，但其牺牲了一定的真实性与可配置性，例如其缓存一致性为MOESI的监听协议，与缓存代码紧密耦合，难以单独替换与修改缓存一致性。

Ruby为内存子系统实现了详细的仿真模型，它可以建模包含或独占的缓存层次结构、各类缓存替换策略、缓存一致性协议实现、互连网络、DMA和内存控制器、定序器等。这些模型是模块化的、灵活的和高度可配置的。这样我们可以单独地对某个存储层次系统的部件进行定制化的研究。例如可以对许多不同类型的缓存一致性实现进行建模，包括广播、目录、令牌、区域一致性等，并且很容易扩展到新的一致性模型。

xs-dev目前使用的是Classic模型cache，基于gem5原代码向xs-RTL进行对齐。cache的代码主要集中在src/mem/cache中，其中base.cc实现的是cache的基本增删改查功能和一致性协议的状态变化逻辑（类BaseCache)，然后cache.cc在BaseCache的基础上覆写了接口，将一致性协议的处理逻辑放在了这一层中。

## Cache的snoop设计
探测请求是一致性协议实现中重要的通信类型，在 `gem5` 的 `Cache` 类中，以下函数可以创建 snoop 请求：

---

### 1. `Cache::isCachedAbove`
+ **功能**：检查某个块是否存在于上层缓存中。
+ **创建方式**：创建一个 `Packet` 对象（`snoop_pkt`），并通过 `cpuSidePort.sendTimingSnoopReq` 或 `cpuSidePort.sendAtomicSnoop` 发送 snoop 请求。
+ **代码片段**：

```cpp
bool
Cache::isCachedAbove(PacketPtr pkt, bool is_timing)
{
    if (!forwardSnoops)
        return false;

    if (is_timing) {
        Packet snoop_pkt(pkt, true, false);
        snoop_pkt.setExpressSnoop();
        cpuSidePort.sendTimingSnoopReq(&snoop_pkt);
        return snoop_pkt.isBlockCached();
    } else {
        cpuSidePort.sendAtomicSnoop(pkt);
        return pkt->isBlockCached();
    }
}
```

---

### 2. `Cache::recvTimingReq`
+ **功能**：处理来自 CPU 的请求。
+ **创建方式**：当上层缓存响应请求时，创建一个 express snoop 请求（`Packet` 对象），并通过 `memSidePort.sendTimingReq` 发送。
+ **代码片段**：

```cpp
void
Cache::recvTimingReq(PacketPtr pkt)
{
    if (pkt->cacheResponding()) {
        Packet *snoop_pkt = new Packet(pkt, true, false);
        snoop_pkt->setExpressSnoop();
        snoop_pkt->setCacheResponding();
        memSidePort.sendTimingReq(snoop_pkt);
        pendingDelete.reset(pkt);
        return;
    }

    BaseCache::recvTimingReq(pkt);
}
```

---

### 3. `Cache::recvTimingSnoopReq`
+ **功能**：处理来自下层缓存的 snoop 请求。
+ **创建方式**：在处理 snoop 请求时，如果需要向上层缓存转发 snoop 请求，会创建一个新的 `Packet` 对象（`snoopPkt`），并通过 `cpuSidePort.sendTimingSnoopReq` 发送。
+ **代码片段**：

```cpp
void
Cache::recvTimingSnoopReq(PacketPtr pkt)
{
    if (!inRange(pkt->getAddr())) {
        return;
    }

    if (mshr && mshr->handleSnoop(pkt, order++)) {
        return;
    }

    uint32_t snoop_delay = handleSnoop(pkt, blk, true, false, false, nullptr);
    pkt->snoopDelay = std::max<uint32_t>(pkt->snoopDelay, snoop_delay + lookupLatency * clockPeriod());
}
```

---

### 4. `Cache::handleSnoop`
+ **功能**：处理 snoop 请求。
+ **创建方式**：如果 `forwardSnoops` 为 `true`，会创建一个新的 `Packet` 对象（`snoopPkt`），并通过 `cpuSidePort.sendTimingSnoopReq` 或 `cpuSidePort.sendAtomicSnoop` 发送。
+ **代码片段**：

```cpp
uint32_t
Cache::handleSnoop(PacketPtr pkt, CacheBlk *blk, bool is_timing,
                   bool is_deferred, bool pending_inval, MSHR *deferring_mshr)
{
    if (forwardSnoops) {
        if (is_timing) {
            Packet snoopPkt(pkt, true, true);
            snoopPkt.setExpressSnoop();
            cpuSidePort.sendTimingSnoopReq(&snoopPkt);
        } else {
            cpuSidePort.sendAtomicSnoop(pkt);
        }
    }
}
```

---

### 5. `Cache::sendMSHRQueuePacket`
+ **功能**：从 MSHR 队列中发送请求。
+ **创建方式**：当处理硬件预取请求（`HardPFReq`）时，创建一个 snoop 请求（`Packet` 对象），并通过 `cpuSidePort.sendTimingSnoopReq` 发送。
+ **代码片段**：

```cpp
bool
Cache::sendMSHRQueuePacket(MSHR* mshr)
{
    Packet snoop_pkt(tgt_pkt, true, false);
    snoop_pkt.setExpressSnoop();
    cpuSidePort.sendTimingSnoopReq(&snoop_pkt);
}
```

---

### 6. `Cache::doWritebacks` 
+ **功能**：处理写回操作。
+ **创建方式**：在写回操作中调用 `isCachedAbove`，间接创建 snoop 请求。
+ **代码片段**：

```cpp
void
Cache::doWritebacks(PacketList& writebacks, Tick forward_time)
{
    if (isCachedAbove(wbPkt)) {
        wbPkt->setBlockCached();
    }
}
```

---

以下是所有能创建 snoop 请求的函数：

1. `Cache::isCachedAbove`
2. `Cache::recvTimingReq`
3. `Cache::recvTimingSnoopReq`
4. `Cache::handleSnoop`
5. `Cache::sendMSHRQueuePacket`
6. `Cache::doWritebacks`

这些函数通过创建 `Packet` 对象并调用 `cpuSidePort` 或 `memSidePort` 的方法，将 snoop 请求发送到缓存层次结构的其他部分。下图展示了Cache类中实现向上向下snoop的通路和调用关系。

![](C:\Users\60962\AppData\Roaming\Typora\typora-user-images\image-20250424165228352.png)![](images/1745909284866-785065d3-f466-4576-9194-fb86d65133df.png)

### express snoops设计
GEM5的classic cache系统设计时为了简化时序，规避各种潜在的争用问题，设计了“express snoops”。首先看GEM5官网的解释：

> Requests from upper-level caches (those closer to the CPUs) propagate toward memory in the expected fashion: an L1 miss is broadcast on the local L1/L2 bus, where it is snooped by the other L1s on that bus and (if none respond) serviced by the L2. If the request misses in the L2, then after some delay (currently set equal to the L2 hit latency), the L2 will issue the request on its memory-side bus, where it will possibly be snooped by other L2s and then be issued to an L3 or memory.
>
> Unfortunately, propagating snoop requests incrementally back up the hierarchy in a similar fashion is a source of myriad nearly intractable race conditions. Real systems don’t typically do this anyway; in general you want a single snoop operation at the L2 bus to tell you the state of the block in the whole L1/L2 hierarchy. There are a handful of methods for this:
>
> 1. just snoop the L2, but enforce inclusion so that the L2 has all the info you need about the L1s as well—an idea we’ve already rejected above
> 2. keep an extra set of tags for all the L1s at the L2 so those can be snooped at the same time (see the Compaq Piranha)—reasonable, if you’re hierarchy’s not too deep, but now you’ve got to size the tags in the lower-level caches based on the number, size, and configuration of the upper-level caches, which is a configuration pain
> 3. snoop the L1s in parallel with the L2, something that’s not hard if they’re all on the same die (I believe Intel started doing this with the Pentium Pro; not sure if they still do with the Core2 chips or not, or if AMD does this as well, but I suspect so)—also reasonable, but adding explicit paths for these snoops would also make for a very cumbersome configuration process
>
> We solve this dilemma by introducing “express snoops”, which are special snoop requests that get propagated up the hierarchy instantaneously and atomically (much like the atomic-mode accesses described on the [Memory System](https://www.gem5.org/documentation/general_docs/memory_system) page), even when the system is running in timing mode. Functionally this behaves very much like options 2 or 3 above, but because the snoops propagate along the regular bus interconnects, there’s no additional configuration overhead. There is some timing inaccuracy introduced, but if we assume that there are dedicated paths in the real hardware for these snoops (or for maintaining the additional copies of the upper-level tags at the lower-level caches) then the differences are probably minor.
>
> 不幸的是，以类似方式逐步将snoop请求传播回cache 结构会导致一种几乎无法解决的竞争条件。 实际系统通常不会这样做； 一般来说，您希望在L2总线上进行一次snoop操作，以告知整个L1/L2层次结构中块的状态。 有几种方法可以实现这一目标：
>
> 1.仅仅snoop L2，但强制执行包含的cache策略，以便L2拥有所有关于L1的信息——这个想法我们已经在上面拒绝过了（效率太低）。
>
> 2. 在L2处为所有的L1保留额外的一组标签，这样可以同时进行snoop（参见康柏Piranha）——如果你的cache结构不是太深，这是合理的，但现在你必须根据上层缓存的数量、大小和配置来确定下层缓存中的标签大小，这会带来配置上的困扰。
> 3. 与L2并行窥探L1，如果它们都在同一块DIE上，这一点并不难实现（我相信英特尔从Pentium Pro开始这样做；不确定他们是否仍然在Core2芯片上这样做，或者AMD是否也这样做，但我怀疑如此）——这也是合理的，但为这些窥探添加额外的路径也会使配置过程变得非常繁琐。
>
> 我们通过引入“快速侦听”来解决这个两难问题，这是一种特殊的snoop请求，在系统以时序模式运行时，它们仍能像原子模式访问（如内存系统页面中描述的那样）一样即时且原子地在层次结构上传播。从功能上来说，这与上面的选项2或3非常相似，但由于侦听沿着常规的总线互连传播，因此没有额外的配置开销。这样做确实会引入一些时序不准确性，但如果我们假设实际硬件中存在用于这些snoop的专用路径（或者用于在低级缓存中维护高级标签额外副本的路径），那么差异可能很小。
>

express snoop在被发送和处理时都是即时地调用函数，通过维护一个snoopDelay变量，在经过每一级部件时向snoopDelay加上预设的delay值，来模拟一个snoop的延迟。

也就是说express snoop没有模拟一个片上网络上链路的路由算法和转发端口的队列，这些会对访存的延迟存在多少影响还有待考量。

## Cache一致性状态
### 一致性状态定义
CacheBlk 类中的一致性状态通过以下位标志表示：

+ WritableBit：表示缓存块是否具有写权限。
+ ReadableBit：表示缓存块是否具有读权限。
+ DirtyBit：表示缓存块是否被修改（脏状态）。
+ Valid（隐含状态）：表示缓存块是否有效。

这些状态可以组合形成缓存一致性协议中的状态，例如：

+ Modified (M)：WritableBit 和 DirtyBit 被设置。
+ Exclusive (E)：WritableBit 被设置，但 DirtyBit 未设置。
+ Shared (S)：ReadableBit 被设置，但 WritableBit 和 DirtyBit 未设置。
+ Invalid (I)：缓存块无效。

```cpp
        *  state       M   O   E   S   I
         *  writable    1   0   1   0   0
         *  dirty       1   1   0   0   0
         *  valid       1   1   1   1   0
         *
         *  state   writable    dirty   valid
         *  M       1           1       1
         *  O       0           1       1
         *  E       1           0       1
         *  S       0           0       1
         *  I       0           0       0
         *
         * Note that only one cache ever has a block in Modified or
         * Owned state, i.e., only one cache owns the block, or
         * equivalently has the DirtyBit bit set. However, multiple
         * caches on the same path to memory can have a block in the
         * Exclusive state (despite the name). Exclusive means this
         * cache has the only copy at this level of the hierarchy,
         * i.e., there may be copies in caches above this cache (in
         * various states), but there are no peers that have copies on
         * this branch of the hierarchy, and no caches at or above
         * this level on any other branch have copies either.
```

Cache_blk类设置了readable这个状态位，但在组合表示MOESI状态时没有用到这个位，而且在实际逻辑中，用到的判断条件还是直接看状态位而不是MOESI状态。

