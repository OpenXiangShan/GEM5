backend的范围包括decode,rename,issue,execute,writeback,commit

！！xs-gem5的后端经过大规模的重写，gem5官方教程已经不太适用！！

后端大致流程

# ![](images/1743390724629-d762a8fc-16bb-4a01-ac85-be76f1d5c217.png)decode
没啥好说

# Rename
主要是支持了move elimination,这个也没啥好说

# Scheduler ！！重点
### 逻辑功能
现在scheduler调度器已经几乎完全对齐RTL,如果熟悉RTL的话，应该也能很快上手

scheduler包含了dispatch一直到writeback这几个阶段

xs-gem5现在采用分布式IQ

dispatch阶段负责将指令派遣到不同的IQ中

```python
// 关dispatch和dispQue
iew.cc: tick() >> dispatchInsts() >> dispatchInstFromRename()
>> scheduler.addProducer() >> instQueue.insert() >> scheduler.insert()
// 开dispatch
dispatchInsts() >> dispatchInstFromDispQue() >> scheduler.addProducer
>> classifyInstToDispQue() >> instQueue.insert() >> scheduler.insert()
```

Scheduler和IssueQue相关的代码位于issue_queue.cc中

Scheduler比较重要的函数有

```python
Scheduler::addProducer()// 标记scoreboard
Scheduler::specWakeUpDependents()// 根据推测唤醒网络去唤醒指令
Scheduler::specWakeUpFromLoadPipe()// loadpipe的唤醒通道
Scheduler::useRegfilePort()// 寄存器堆读端口仲裁
Scheduler::loadCancel()// 指令cancel
```

IssueQue比较重要的函数有

```python
IssueQue::issueToFu()// 指令走完发射流程，走向执行单元
IssueQue::wakeUpDependents()// 唤醒本地指令
IssueQue::addIfReady()// 将指令添加到readyQ中
IssueQue::selectInst()// 从readyQ中挑选指令到selectQ中，在这里会完成读仲裁，写冲突等操作
IssueQue::scheduleInst()// 从selectQ中取出指令并过滤掉cancel的指令，将其放到发射流程当中
```

后端的指令调度相关的函数调用链如下（单个周期内）

![](images/1743405188693-30f8416c-49a5-4934-8578-834f61edfd6c.png)

### 参数配置
Scheduler还做了高度参数化的设计，下面是kmhv2的scheduler配置（configs/common/FUScheduler.py）

![](images/1743405430447-bde02ba8-ee76-4c4b-a471-353d60e6b651.png)

其中IssuePort用于配置一个IQ下能挂载的FU类型和读端口分配

每周期一个IssuePort只能接受一条指令，即使它支持多个FU

使用读端口从一条指令的源操作数编号来分配，比如：

add x1, x2, x3, 则x2会使用第一个整数寄存器堆读端口，x3会使用第2个整数寄存器读端口

若是fsd f1, 0(x1), 则f1会使用第一个浮点寄存器堆读端口，x1会使用第一个整数寄存器读端口(举例，实际并不是这样)

