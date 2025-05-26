# GEM5 LSQ 学习
（2025.04.28：LSQ预计需要重构pipe代码，下面的代码不一定仍然对得上最新版本）

## lsq接收来自cache的回复流程
在GEM5中，LSQ类为整个LSQ的外层接口，内部包括多个LSQUnit，每个LSQUnit处理具体的Packet。GEM5将LSQRequest划分为了3种，分别是SingleDataRequest、SplitDataRequest、SbufferRequest。

函数调用层次顺序为：LSQ::DcachePort::recvTimingResp ----- LSQ::recvTimingResp   ----- LSQUnit::recvTimingResp    ----- LSQRequest::recvTimingResp

### **1. 单一数据请求的处理**
+ **函数**: `SingleDataRequest::recvTimingResp`
    - 检查`cache`是否命中（`pkt->cacheSatisfied`）。
    - 如果启用了`enableLdMissReplay`并且是`cache miss`：
        * 将`load`指令标记为等待数据回填（`waitingCacheRefill`）。
        * 调用`discard()`丢弃当前请求。
    - 如果`cache hit`：
        * 调用`assemblePackets()`将数据写回到指令目标寄存器。
    - 清除`LSQRequest`的`pendingCacheReq`状态。

---

### **2. 分割数据请求的处理**
+ **函数**: `SplitDataRequest::recvTimingResp`
    - 检查当前响应的`Packet`是否属于分割请求的某一部分。
    - 更新已接收的分割包计数（`numReceivedPackets`）。
    - 如果所有分割包都接收完成：
        * 调用`assemblePackets()`将数据组装并写回。
        * 清除`pendingCacheReq`状态。

---

### **3. Store Buffer 请求的处理**
+ **函数**: `SbufferRequest::recvTimingResp`
    - 确认响应的`Packet`与请求匹配。
    - 调用`completeSbufferEvict()`完成数据写回。

---

### **4. 数据访问完成**
+ **函数**: `LSQ::completeDataAccess`
    - 调用`LSQUnit::completeDataAccess`完成数据访问。
    - 如果是`load`指令，检查数据是否与golden mem一致（如果启用了数据一致性检查）。
    - 如果需要写回寄存器（如`load`或`atomic`指令），调用`writeback()`完成写回。

---

### 5.writeback处理
1. 唤醒 CPU 并确保流水线活动。
2. 检查指令是否被丢弃，若是则直接返回。
3. 如果指令正常完成，将数据写回目标寄存器。
4. 如果指令发生异常，处理异常并记录调试信息。
5. 将指令插入提交队列，等待提交。
6. 检查是否发生分支预测错误

## lsq向cache发送读请求流程
### **1. 指令被插入 Load Queue**
+ **函数**: `LSQ::insertLoad`
    - `load`指令被插入到`LSQ`的`Load Queue`中。
    - 每个线程有独立的`Load Queue`，用于管理该线程的`load`指令。
    - 调用`LSQUnit::insertLoad`将`load`指令插入到`LSQUnit`的`Load Queue`。

---

### **2. 产生LSQRequest内存请求**
+ **函数**: `LSQ::pushRequest`
    - 创建`LSQRequest`对象（如`SingleDataRequest`或`SplitDataRequest`）。
    - 如果地址跨越了`cache line`边界，则创建`SplitDataRequest`，否则创建`SingleDataRequest`。
    - 调用`LSQRequest::initiateTranslation`发起地址翻译。

---

### **3. 地址翻译**
+ **函数**: `LSQRequest::initiateTranslation`
    - 调用`LSQRequest::sendFragmentToTranslation`将请求发送到`MMU`进行地址翻译。
    - 翻译完成后，`LSQRequest::finish`被回调，更新请求状态。
    - 如果翻译成功，设置`load`指令的物理地址。

---

### **4. 进入 Load Pipe**
`load`指令进入`LSQUnit`的`load pipe`，按照流水线阶段依次执行：

#### **4.1 Load Pipe S0: 地址计算与对齐检查**
+ **函数**: `LSQUnit::loadPipeS0`
    - 检查`load`指令的地址是否对齐。
    - 如果地址未对齐，设置`fault`并终止执行。
    - 如果地址对齐，进入下一阶段。

---

#### **4.2 Load Pipe S1: 地址翻译完成检查**
+ **函数**: `LSQUnit::loadPipeS1`
    - 检查地址翻译是否完成：
        * 如果翻译未完成，指令被阻塞，等待翻译完成。
        * 如果翻译完成，检查是否需要访问内存。
    - 如果需要访问内存，调用`LSQUnit::read`发起内存访问。
    - read()期间先检查SQ、sbuffer、总线是否包含需要的数据，有就会触发数据前递，没有就会向cache发送请求，使用**函数**: `LSQRequest::sendPacketToCache`
        * 调用`LSQUnit::trySendPacket`尝试将`Packet`发送到`cache`。
        * 如果发生`bank conflict`或`tag read fail`，调用`bankConflictReplaySchedule`或`tagReadFailReplaySchedule`重新调度。



---

#### **4.3 Load Pipe S2: 数据访问与转发**
+ **函数**: `LSQUnit::loadPipeS2`
    - 检查是否可以从`store queue`或`store buffer`中转发数据：
        * 调用`LSQUnit::forwardFrmBus`尝试从总线转发数据。
        * 如果无法转发，调用`LSQRequest::sendPacketToCache`发送请求到`cache`。
    - 如果发生`cache miss`，可能触发`load miss replay`。

---

#### **4.4 Load Pipe S3: 数据写回**
+ **函数**: `LSQUnit::loadPipeS3`
    - 检查`cache`响应是否到达：
        * 如果响应到达，调用`LSQUnit::completeDataAccess`完成数据写回。
        * 如果响应未到达，指令被阻塞，等待响应。
    - 数据写回后，指令标记为完成。



---

### **5. 数据访问完成**
+ **函数**: `LSQUnit::completeDataAccess`
    - 调用`LSQRequest::assemblePackets`将数据写回到指令的目标寄存器或内存。
    - 更新`Load Queue`状态，标记指令为完成。

### 
