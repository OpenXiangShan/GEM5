# gem5 学习

参考[https://github.com/shinezyy/micro-arch-training](https://github.com/shinezyy/micro-arch-training)有一些练手项目

## 统计数据
添加decode 每拍 译码数目

```cpp
system.cpu.fetch.nisnDist::samples             133418                       # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::mean              0.867109                       # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::stdev             1.805963                       # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::underflows               0      0.00%      0.00% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::0                    96731     72.50%     72.50% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::1                    16395     12.29%     84.79% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::2                     1424      1.07%     85.86% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::3                     1411      1.06%     86.92% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::4                      306      0.23%     87.14% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::5                    15157     11.36%     98.51% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::6                      231      0.17%     98.68% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::7                      287      0.22%     98.89% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::8                     1476      1.11%    100.00% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::overflows                0      0.00%    100.00% # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::min_value                0                       # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::max_value                8                       # Number of instructions fetched each cycle (Total) (Count)
system.cpu.fetch.nisnDist::total               133418  
```



统计数据

```bash
example_stats_dir=/nfs/home/yanyue/workspace/GEM5/util/xs_scripts/backend_test/test0

batch.py -s $example_stats_dir --cache -t --topdown-raw -o results/example.csv

# 统计得分
python3 simpoint_cpt/compute_weighted.py \
-r results/example.csv \
-j /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/checkpoint-0-0-0/cluster-0-0.json \
--score results/example-score.csv

# 统计topdown
python3 simpoint_cpt/compute_weighted.py \
-r results/example.csv \
-j /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/checkpoint-0-0-0/cluster-0-0.json \
-o results/weighted.csv

```

![](images/1724901163046-50a4b858-b7d7-4216-9ae3-e80d857a498b.png)

[SimObjects - gem5 --- SimObjects - gem5](https://old.gem5.org/SimObjects.html)

[Python 与 C++ 交互 --- Python Interaction with C++ (narkive.com)](https://gem5-users.gem5.narkive.com/TyNxYz3Z/python-interaction-with-c)

python 主要配置系统，把每个SimObject 的参数设置好，用xiangshan.py来配置

然后对每个SimObject 依次调用m5.instantiate() 来调用C++对象的构造函数

最后m5.simulate() 来运行整个系统





## ProbePoint 探测点
<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">ProbePoint（探测点）是一种用于监控和调试系统行为的机制。它允许开发者在代码的特定位置插入"探测点"，以便在模拟运行时收集信息或触发特定行为。这种机制对于理解系统内部工作、性能分析和调试非常有用。</font>

就是运行过程中插装，获取统计数据或者修改行为

例如监控rename行为，每次指令重命名完成打印信息

探测点可以绑定多个监听器，每次notify 会监听所有信息

```cpp
class RenameProbeListener : public ProbeListenerArgBase<DynInstPtr>
{
  public:
    void notify(const DynInstPtr &inst) override
    {
        std::cout << "Instruction " << inst->seqNum 
                  << " renamed at cycle " << curTick() << std::endl;
    }
};

// 在Rename类的构造函数中
Rename::Rename(...)
{
    // ...其他初始化代码...

    ppRename = new ProbePointArg<DynInstPtr>(this, "Rename");	// 一个探测点
    RenameProbeListener *listener = new RenameProbeListener();  // 一个监听器
    ppRename->addListener(listener);  // 监听器绑定到探测点
}

// 在重命名完成时触发探测点
void Rename::renameInsts(ThreadID tid)
{
    // ...重命名代码...

    for (auto &inst : renamed_instructions) {
        ppRename->notify(inst);    // 探测点触发所有监听器来输出一些信息
    }
}
```



## IEW 阶段
```bash
/**
 *          insert into queue
 *                 |
 *                 V
 *         speculative schedule <-------+
 *                 |                    |
 *                 V                    |
 *      schedule (issueStage 0)   ------+
 *                 |                    |
 *                 V                    |
 *       delay (issueStage n)     wake or cancel
 *                 |                    |
 *                 V                    |
 *      issue success bypass datas      |
 *                 |                    |
 *                 V                    |
 *              execute ----------------+
*/
指令队列(issue queue)中指令的处理流程。我来解释一下这个过程:
1. 插入队列 (insert into queue):
指令首先被插入到发射队列中。
2. 推测性调度 (speculative schedule):
在这个阶段,指令被推测性地调度。这意味着即使所有操作数可能还没准备好,也会尝试调度指令。
3. 调度 (schedule - issueStage 0):
这是正式调度阶段的开始。在这里,指令被选择进行发射。
4. 延迟 (delay - issueStage n):
这表示可能存在多个发射阶段。在某些处理器设计中,从调度到实际执行可能需要几个周期。
5. 发射成功并旁路数据 (issue success bypass datas):
指令成功发射后,其结果可能会通过旁路网络立即提供给依赖的指令,而不需要等待写回阶段。
6. 执行 (execute):
指令在功能单元中执行。
7. 唤醒或取消 (wake or cancel):
唤醒: 执行完成后,依赖于该指令的其他指令会被唤醒,使它们可以准备发射。
取消: 如果发生异常或预测错误,指令可能会被取消。

```

```bash
 /* IEW (发射/执行/写回) 模块同时处理单线程和SMT (同时多线程) 的IEW过程。
  * 作为发射阶段的一部分,它负责将指令分派到LSQ (加载/存储队列) 和IQ (指令队列)。（Dispatch阶段）
  * 每个周期,IQ都会尝试发射指令。
  * 执行延迟实际上与发射延迟相关联,这允许IQ能够进行背靠背调度,而无需推测性地调度指令。
  * 这是通过让IQ访问功能单元来实现的,IQ在发射指令时从功能单元获取执行延迟。
  * 指令在其执行的最后一个周期到达执行阶段,此时IQ知道要唤醒任何依赖的指令,从而允许背靠背调度。
  * IEW的执行部分将内存指令与非内存指令分开,要么告诉LSQ执行指令,要么直接执行指令。
  * IEW的写回部分通过唤醒任何依赖项并在记分板上标记寄存器为就绪来完成指令执行。 
  */
```

```bash
OpDesc 描述操作类延迟
FUDesc 
```

