

```cpp
Rename::tick()->Rename::RenameInsts()
IEW::tick()->IEW::dispatchInsts()
IEW::tick()->IssueQueue::scheduleReadyInsts()
IEW::tick()->IEW::executeInsts()
IEW::tick()->IEW::writebackInsts()
Commit::tick()->Commit::commitInsts()->Commit::commitHead()

重命名，
dispatch，发送到issue queue, LSQ
发射准备好的指令，src准备好的指令放入ready list, 调度到可用FU
执行，insts->execute(),并写入寄存器
写回，wakeDepends(),依赖指令放入ready list等待调度
提交，指令到达ROB头部，提交并从ROB释放

区别Instruction Queue
    Issue Queue
```



## 香山配置
1. 预取器

```cpp
common/Options.py

L1 i cache 无prefetcher  64K
L1d  XSCompositePrefetcher   64k
L2 L2CompositeWithWorkerPrefetcher  1M
L3 WorkerPrefetcher			16MB

ation='store' store_true?
```

2. 注意参数变化

```bash
parser: --enable-arch-db  命令行用中横线
python:  if args.enable_arch_db   用下划线， C++中也是一样
sys.gcpt_file  == p.gcpt_file
```

2. difftest

```cpp
difftest.cc/.hh

和nemu对比
new NemuProxy()
打开nemu动态库，调用其中很多函数，其中关键在regcpy
本质是把nemu 中cpu regs memcpy出来！
```

3. 分支预测器 BP

```cpp
DecoupledBPUWithFTB(
                                            bpDBSwitches=bp_db_switches,
                                            enableLoopBuffer=args.enable_loop_buffer,
                                            enableLoopPredictor=args.enable_loop_predictor,
                                            enableJumpAheadPredictor=args.enable_jump_ahead_predictor
                                            )

```

4. 前端参数

```bash
BaseO3CPU.py
decode width 6 -> 8
rename width 6 -> 8
dispatch 	6 -> 8
issue 	  8 -> 12
writeback 8 -> 12
commit	  6 -> 12
squash    6 -> 12
LQ entry 	80  128
SQ entry	64	96
storebuffer 16	24
num phy regs 192 256
num float/vec  
dispatch queue  16	32
ROB entry 	 256 	320

```

新的scheduler

```python
class XiangshanCore(RiscvO3CPU):
    fuPool = XSCoreFUPool()
    # 参数用默认配置的

class XiangshanECore(XiangshanCore):
    fuPool = XSECoreFUPool()
    # 参数重新覆盖了！
    fetchWidth = 8
    decodeWidth = 4

class XSCoreFUPool(FUPool):
没有用了，都用scheduler来替代了！

FUPool   一堆Func type (FUDesc) 具体单元

class KunminghuScheduler(Scheduler):
    IQs = [
    # 分布式发射对立
    # inoutPorts 净出口， size = 宽度， 对应的发射到不同功能单元去计算延迟
        IssueQue(name='IQ_misc' , inoutPorts=1, size=1*24, fuType=[IntDiv()]),
        IssueQue(name='IQ_br', inoutPorts=2, size=2*24, fuType=[IntBRU()]),
        IssueQue(name='IQ_si', inoutPorts=4, size=4*24, fuType=[IntALU()]),
        IssueQue(name='IQ_ci', inoutPorts=2, size=2*24, fuType=[IntALU(), IntMult()]),
        IssueQue(name='IQ_stu', inoutPorts=2, size=2*24, fuType=[WritePort()]),
        IssueQue(name='IQ_ldu', inoutPorts=2, size=2*24, fuType=[ReadPort()]),
        IssueQue(name='IQ_cplx',inoutPorts=3, size=3*24,
            scheduleToExecDelay=3, fuType=[FP_MISC(), FP_SLOW(), FP_MAM(), FP_MAA(), SIMD_Unit()])
    ]
    slotNum = 12
    xbarWakeup = True
```

### 参数传递
```python
./build/RISCV/gem5.opt configs/example/xiangshan.py --generic-rv-cpt /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/libquantum/98457/_98457_0.048839_.zstd
```

xiangshan.py 接受--generic-rv-cpt

```python
# Add args
parser = argparse.ArgumentParser()
Options.addCommonOptions(parser, configure_xiangshan=True)
# configs/common/Options.py  常用参数
    addNoISAOptions(parser,configure_xiangshan=configure_xiangshan)
    # 配置cache 等输入
Options.addXiangshanFSOptions(parser)	# 切片相关


test_sys = build_test_system(args.num_cpus)
    XSConfig.config_xiangshan_inputs(args, test_sys)    # 配置输入
    # 配置分支预测等，
    


```



scons build/RISCV/gem5.opt --gold-linker -j100



