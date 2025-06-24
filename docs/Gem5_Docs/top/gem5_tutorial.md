视频讲解：Gem5 上手

1. Part 1  
录制：香山访存与缓存例会  
日期：2025-03-28 16:52:22  
录制文件：[https://meeting.tencent.com/crm/Nbw6E9g987](https://meeting.tencent.com/crm/Nbw6E9g987)
2. Part 2  
录制：香山访存与缓存例会  
日期：2025-03-28 17:09:49  
录制文件：[https://meeting.tencent.com/crm/N8XYYELz8b](https://meeting.tencent.com/crm/N8XYYELz8b)

## 1.如何使用 gem5
gem5 仓库 [https://github.com/OpenXiangShan/GEM5](https://github.com/OpenXiangShan/GEM5) 的 README 包含了编译和运行的方法。

```shell
git clone https://github.com/OpenXiangShan/GEM5.git
cd GEM5
export GEM5_HOME=$(pwd)
# 构建 DRAMSIM3
bash ./init.sh
make
# 构建 GEM5
cd $GEM5_HOME
scons build/RISCV/gem5.opt --gold-linker -j48
scons build/RISCV/gem5.debug --gold-linker -j48
```

```shell
# 设置 diff 的 nemu
export GCBV_REF_SO="/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
# 设置 GCB_RESTORE 为空
export GCB_RESTORER=""
# 跑一个 bin (非 ckpt)
./build/RISCV/gem5.opt configs/example/xiangshan.py --generic-rv-cpt /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin --raw-cpt
# 跑一个 ckpt
./build/RISCV/gem5.opt configs/example/xiangshan.py --generic-rv-cpt /nfs/home/share/jiaxiaoyu/simpoint_checkpoint_archive/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/lbm/12578/_12578_0.231430_.zstd
# log 和性能计数器在 m5out/ 下，可以指定./build/RISCV/gem5.opt --outdir=xxx configs/example/xiangshan.py 来更换
# 加上 --ideal-kmhv3 参数来使用理想化设置的模型运行
./build/RISCV/gem5.opt configs/example/xiangshan.py --ideal-kmhv3 --generic-rv-cpt /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin --raw-cpt
```

```shell
# 小机房 ckpt 路径
export CHECKPOINT_ROOT="/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_archive/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0"
# 或者使用大机房 ckpt 路径
# export CHECKPOINT_ROOT="/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
export GCBV_REF_SO="/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
export GCB_RESTORER=""
export GEM5_HOME=$(pwd)
mkdir -p $GEM5_HOME/util/xs_scripts/test
cd $GEM5_HOME/util/xs_scripts/test
# 批量跑 ckpt
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` \
      /nfs/home/share/gem5_ci/spec06_cpts/spec_0.8c_int.lst \ # 跑 ckpt 的 list，类似于 json
      $CHECKPOINT_ROOT \
      spec_all # 输出的 log 所在文件夹名
```

```shell
git clone https://github.com/shinezyy/gem5_data_proc.git
cd gem5_data_proc
export PYTHONPATH=`pwd`
ulimit -n 4096
bash example-scripts/gem5-score-ci.sh \
      $GEM5_HOME/util/xs_scripts/test/spec_all \ # 原始 log 路径
      /nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json
```

<br/>warning
关于编译和运行 yanyue 记录了一些更详细的内容在：[https://bosc.yuque.com/yny0gi/sggyey/he9nnr02lttsiihf](https://bosc.yuque.com/yny0gi/sggyey/he9nnr02lttsiihf)

<br/>

<br/>warning
**关于开发和调试环境 yanyue 总结了如何使用 vscode + clangd 配置代码跳转 & vscode 使用 gdb 调试（强烈建议配一下）：**[https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6](https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6)

<br/>

一些辅助资料：

1. [https://www.gem5.org/documentation/learning_gem5/part1/gem5_stats/](https://www.gem5.org/documentation/learning_gem5/part1/gem5_stats/)（理解gem5的输出）

## 2.如何理解 gem5
通过静态看代码可以大概理解 gem5，但存在以下几个问题

1. gem5 有一些代码是用模板生成的，想看具体的内容需要在 build 下的文件里面找
2. gem5 使用了大量的重载和多态特性，即使配置了代码跳转只能跳转到基类的实现，对应到具体的子类还需要费点劲

因此比较推荐使用 gdb 动态地来看代码，通过观察一条指令的执行情况来理解 gem5 的运行机制：

1. 配置 vscode 使用 gdb 调试 [https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6](https://bosc.yuque.com/yny0gi/sggyey/xuobc516y876q5n6)
2. 使用以下的 launch.json 放到 GEM5 项目的 .vscode 目录下

```json
{
    "version": "0.2.0",
    "configurations": [
      {
        "name": "(gdb) 启动",
        "type": "cppdbg",
        "request": "launch",
        // "program": "${workspaceFolder}/build/RISCV/gem5.opt",
        "program": "${workspaceFolder}/build/RISCV/gem5.debug",
        "args": [
          // "--debug-break=569430",
          // "--debug-start=569430",
          "--debug-flags=LSQ,LSQUnit",
          //   "--debug-file=debug.log",
          "${workspaceFolder}/configs/example/xiangshan.py",
          //   "--ideal-kmhv3",
          "--raw-cpt",
          //   "--generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/dummy-riscv64-xs.bin",
          "--generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin",
          //   "--generic-rv-cpt=/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/xalancbmk/35819/_35819_0.027347_.zstd"
        ],
        "stopAtEntry": false,
        "cwd": "${workspaceFolder}",
        "environment": [
            {
                "name": "GCBV_REF_SO",
                "value": "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
            },
            {
                "name": "GCB_RESTORER",
                "value": ""
            },
            {
                "name": "GEM5_HOME",
                "value": "${workspaceFolder}"
            }
        ],
        "externalConsole": false,
        "MIMode": "gdb",
        // "miDebuggerArgs": "--nh -iex \"set auto-load safe-path /\"",
        "logging": {
          "engineLogging": true,
          "trace": true
        },
        "setupCommands": [
          {
            "description": "为 gdb 启用整齐打印",
            "text": "-enable-pretty-printing",
            "ignoreFailures": true
          },
          {
            "description": "将反汇编风格设置为 Intel",
            "text": "-gdb-set disassembly-flavor intel",
            "ignoreFailures": true
          },
          {	// 默认输出为16进制，可以不用
            "description": "设置输出为16进制",
            "text": "set output-radix 16",
            "ignoreFailures": true
          },
          {
            "description": "加载外部断点文件",
            "text": "source ${workspaceFolder}/.gdbinit_bp",
            "ignoreFailures": false
          }
        ]
      }
    ]
  }
```

3. 使用以下的 .gdbinit_bp 断点列表 放到 GEM5 项目的根目录下

```python
set breakpoint pending on
python

import gdb
import os
import re

print("making breakpoints in gdb!")

def get_target_seq():
    return os.getenv('TARGET_SEQ_NUM', '1222')

def get_func_info(func_name):
    func_info = gdb.execute(f'info line {func_name}', to_string=True)
    func_line_start = 0
    file_path = ''
    print(func_info)
    matched = re.search(r'Line (\d+) of "([^"]+)"', func_info)
    if matched:
        func_line_start = int(matched.group(1))
        file_path = matched.group(2)
    else:
        raise Exception(f"can not find {func_name}")
    return [file_path, func_line_start]

def find_pattern_in_func(func_name, pattern):
    file_path, func_line_start = get_func_info(func_name)
    with open(os.path.join(os.getcwd(), file_path), 'r') as fp:
        lines = fp.readlines()
        lines = lines[func_line_start - 2:]
        for i, line in enumerate(lines):
            # print(line.strip())
            if pattern in line:
                return f'{file_path}:{i + func_line_start}'

        raise Exception(f"Error: Pattern '{pattern}' not found in {func_name}")

# 查找包含特定模式的代码行
def set_dynamic_breakpoint(func_name, pattern, condition):
    gdb.execute(f"break {find_pattern_in_func(func_name, pattern)} {condition}")

def set_fe_breakpoint():
    # TODO: add frontend breakpoint
    pass

def set_ooo_breakpoint():
    target_seq = get_target_seq()
    # TODO:
    # rename
    # dsipatch
    # schedule
    
    # get an instruction to execute(issue into FU)
    set_dynamic_breakpoint('IEW::executeInsts',
                            f'DynInstPtr inst = instQueue.getInstToExecute();',
                            f'if inst->seqNum == {target_seq}')
    # bypass writeback an instruction
    set_dynamic_breakpoint('IEW::instToCommit',
                            f'IEW::instToCommit(const DynInstPtr& inst)',
                            f'if inst->seqNum == {target_seq}')
    # writeback an instrution to commit
    set_dynamic_breakpoint('IEW::writebackInsts',
                            f'DynInstPtr inst = execWB->insts[inst_num];',
                            f'if inst->seqNum == {target_seq}')
    # try commit an instruction
    set_dynamic_breakpoint('Commit::commitInsts',
                            f'head_inst = rob->readHeadInst(commit_thread);',
                            f'if head_inst->seqNum == {target_seq}')
    # commit rob head instruction
    set_dynamic_breakpoint('Commit::commitHead',
                            f'Commit::commitHead(const DynInstPtr &head_inst, unsigned inst_num)',
                            f'if head_inst->seqNum == {target_seq}')
    # instruction is been squashed
    set_dynamic_breakpoint('ROB::doSquash',
                            f'(*squashIt[tid])->setSquashed();',
                            f'if (*squashIt[tid])->seqNum == {target_seq}')
    # clear the entry in rob
    set_dynamic_breakpoint('ROB::retireHead',
                            f'DynInstPtr head_inst = std::move(*head_it);',
                            f'if head_inst->seqNum == {target_seq}')

def set_load_breakpoint():
    target_seq = get_target_seq()
    # dispatch insert a load instruction to LQ
    set_dynamic_breakpoint('LSQ::insertLoad',
                            f'LSQ::insertLoad(const DynInstPtr &load_inst)',
                            f'if load_inst->seqNum == {target_seq}')
    # Iq issue to Load pipe
    set_dynamic_breakpoint('LSQ::issueToLoadPipe',
                            f'LSQ::issueToLoadPipe(const DynInstPtr &inst)',
                            f'if inst->seqNum == {target_seq}')
    # load instruction executes through its pipe
    set_dynamic_breakpoint('LSQUnit::executeLoadPipeSx',
                            f'auto& inst = stage->insts[j];',
                            f'if inst->seqNum == {target_seq}')

def set_store_breakpoint():
    target_seq = get_target_seq()
    # dispatch insert a store/amo instruction to SQ
    set_dynamic_breakpoint('LSQ::insertStore',
                            f'LSQ::insertStore(const DynInstPtr &store_inst)',
                            f'if store_inst->seqNum == {target_seq}')
    # execute store data pipeline
    set_dynamic_breakpoint('IEW::executeInsts',
                            f'if (inst->getFault() == NoFault)',
                            f'if inst->seqNum == {target_seq}')
    # Iq issue to Store pipe
    set_dynamic_breakpoint('LSQ::issueToStorePipe',
                            f'LSQ::issueToStorePipe(const DynInstPtr &inst)',
                            f'if inst->seqNum == {target_seq}')
    # store instruction executes through its pipe
    set_dynamic_breakpoint('LSQUnit::executeStorePipeSx',
                            f'auto& inst = stage->insts[j];',
                            f'if inst->seqNum == {target_seq}')
    # store instruction committed by rob
    set_dynamic_breakpoint('LSQUnit::commitStores',
                            f'assert(x.valid());',
                            f'if x.instruction()->seqNum == {target_seq}')
    # store instruction writes to sbuffer
    set_dynamic_breakpoint('LSQUnit::offloadToStoreBuffer',
                            f'DynInstPtr inst = storeWBIt->instruction();',
                            f'if inst->seqNum == {target_seq}')
    # mmio store/amo instruction directly writes to dcache
    set_dynamic_breakpoint('LSQUnit::directStoreToCache',
                            f'DynInstPtr inst = storeWBIt->instruction()',
                            f'if inst->seqNum == {target_seq}')
    # store completes and deallocate from SQ
    set_dynamic_breakpoint('LSQUnit::completeStore',
                            f'DynInstPtr store_inst = store_idx->instruction();',
                            f'if store_inst->seqNum == {target_seq}')

def set_mem_breakpoint():
    set_load_breakpoint()
    set_store_breakpoint()

def set_breakpoint():
    set_mem_breakpoint()
    set_ooo_breakpoint()

class ConditionalBreakpointsLoader(gdb.Breakpoint):
    def __init__(self, func_name, pattern, condition):
        """在 file:line 处设置特殊断点，触发时加载外部断点"""
        super().__init__(find_pattern_in_func(func_name, pattern), internal=False)
        self.silent = True  # 使该断点静默，不打断程序执行
        self.condition = condition
        self.external_bp_file = get_func_info(func_name)[0]

    def stop(self):
        if gdb.parse_and_eval(self.condition):
            """断点触发时执行，加载其他断点，并删除本断点"""
            print(f"[GDB] trigger {self.location}, load real break points\n")
            set_breakpoint()
            gdb.post_event(self.delete)
        else:
            gdb.execute("continue")
        return False  # 返回 False 继续执行，不停下

def create_first_cond_break_point():
    target_seq = get_target_seq()
    ConditionalBreakpointsLoader('Fetch::buildInst',
                                 f'InstSeqNum seq = cpu->getAndIncrementInstSeq();',
                                 f'seq == {target_seq}')

def on_file_loaded(event):
    create_first_cond_break_point()
    gdb.events.stop.disconnect(on_file_loaded)

if gdb.execute("info files", to_string=True).strip():
    print("file has been loaded set bp now...")
    create_first_cond_break_point()
else:
    print("file has not been loaded, wait until stop event")
    gdb.events.stop.connect(on_file_loaded)

end
```

> .gdbinit_bp 文件主要是给某一条指令执行的关键路径上设置断点，由于过多的条件断点会大量降低运行速度，为了提高效率，其主要逻辑是先只设置一个条件断点，检测取指是否取到特定的 seqNum(GEM5 会给每一个 Dynamic Inst 分配一个 seqNum)，满足条件再设置其他真正关心的关键断点
>
> 使用 export TARGET_SEQ_NUM=xxx 来指定观察 seqNum 为 xxx 的指令执行过程
>

4. 按 F5，或者在 vscode 的运行与调试界面点开始调试

### 2.1 预取
相对比较独立，接口较为规范，主要代码位于 src/mem/cache/prefetch 下，大部分的预取器继承了 Queued 类 (queued.cc)，使用 calculatePrefetch 训练预取器，使用 sendPFWithFilter 发送预取请求

建议学习一下 bop.cc 中的实现，照着 bop 的样子添加一个简单的预取器

### 2.2 LSU
这个 pr 描述了 LSU 重构前后的区别和当前的流程 [https://github.com/OpenXiangShan/GEM5/pull/265](https://github.com/OpenXiangShan/GEM5/pull/265)

建议使用 gdb 动态地观察一条 load/store 指令在 gem5 中是如何执行的

> 例如在 .gdbinit_bp 的例子中，使用：
>
> + export TARGET_SEQ_NUM=193 来观察一条 load
> + export TARGET_SEQ_NUM=300 来观察一条 store
>

### 2.3 Cache


一些辅助资料：

1. [https://www.gem5.org/documentation/learning_gem5/introduction/](https://www.gem5.org/documentation/learning_gem5/introduction/)（gem5 tutorial）
2. [https://www.gem5.org/documentation/](https://www.gem5.org/documentation/)（gem5 官方文档）
3. [https://www.gem5.org/documentation/general_docs/cpu_models/O3CPU](https://www.gem5.org/documentation/general_docs/cpu_models/O3CPU)（gem5 o3 cpu 模型介绍）
4. [https://www.gem5.org/documentation/learning_gem5/part2/events/](https://www.gem5.org/documentation/learning_gem5/part2/events/)（gem5 的事件驱动介绍）
5. [https://www.gem5.org/documentation/general_docs/memory_system/](https://www.gem5.org/documentation/general_docs/memory_system/)（gem5 的 memory 系统介绍）

## 3.如何改 gem5
一些辅助资料：

1. [https://www.gem5.org/documentation/learning_gem5/part2/helloobject/](https://www.gem5.org/documentation/learning_gem5/part2/helloobject/)（如何在gem5中增加新的部件如预取器等）
2. [https://www.gem5.org/documentation/learning_gem5/part2/parameters/](https://www.gem5.org/documentation/learning_gem5/part2/parameters/)（如何给gem5的部件增加参数）
3. [https://www.gem5.org/documentation/learning_gem5/part2/debugging/](https://www.gem5.org/documentation/learning_gem5/part2/debugging/)（如何使用debug-flags打印调试信息）
4. [https://www.gem5.org/documentation/general_docs/debugging_and_testing/debugging/trace_based_debugging](https://www.gem5.org/documentation/general_docs/debugging_and_testing/debugging/trace_based_debugging)（gem5的调试选项用法）
5. [https://www.gem5.org/documentation/general_docs/debugging_and_testing/debugging/debugger_based_debugging](https://www.gem5.org/documentation/general_docs/debugging_and_testing/debugging/debugger_based_debugging)（使用gdb或valgrind来调试gem5）

## 4.使用gem5平台的工具






### <font style="color:rgb(38, 38, 38);">Compute Instructions</font>
<font style="color:rgb(33, 37, 41);">Compute instructions are simpler as they do not access memory and do not interact with the LSQ. Included below is a high-level calling chain (only important functions) with a description about the functionality of each.</font>

```plain
Rename::tick()->Rename::RenameInsts()
IEW::tick()->IEW::dispatchInsts()
IEW::tick()->InstructionQueue::scheduleReadyInsts()
IEW::tick()->IEW::executeInsts()
IEW::tick()->IEW::writebackInsts()
Commit::tick()->Commit::commitInsts()->Commit::commitHead()
```

### <font style="color:rgb(38, 38, 38);">Load Instruction</font>
<font style="color:rgb(33, 37, 41);">Load instructions share the same path as compute instructions until execution.</font>

```plain
Rename::tick()->Rename::RenameInsts()
IEW::tick()->IEW::dispatchInsts()
IEW::tick()->InstructionQueue::scheduleReadyInsts()
IEW::tick()->IEW::executeInsts()
  ->LSQUnit::issueToLoadPipe()
IEW::tick()->LSQUnit::executeLoadPipeS0()
  ->StaticInst::initiateAcc()
    ->LSQ::pushRequest()
      ->LSQRequest::initiateTranslation()
IEW::tick()->LSQUnit::executeLoadPipeS1()
  ->LSQUnit::read()
    ->LSQRequest::buildPackets()
      ->LSQRequest::sendPacketToCache()
  ->LSQUnit::checkViolation()
IEW::tick()->LSQUnit::executeLoadPipeS2()
IEW::tick()->LSQUnit::executeLoadPipeS3()

DcachePort::recvTimingResp()->LSQRequest::recvTimingResp()
  ->LSQUnit::completeDataAccess()
    ->LSQUnit::writeback()
      ->StaticInst::completeAcc()
      ->IEW::instToCommit()
IEW::tick()->IEW::writebackInsts()
```

