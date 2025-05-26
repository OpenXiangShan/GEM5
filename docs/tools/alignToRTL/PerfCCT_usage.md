原理：

perfCCT框架主要用来收集指令执行过程中的各种信息

比如指令的完整生命周期跟踪，指令的执行结果、访存地址等信息

perfCCT是RTL仿真友好的，它将大规模性能数据存放在c++端的结构体内而不是用reg存放

作为代价，每条指令都需要在取指阶段分配一个sequence number作为唯一标识符，几乎不会影响仿真性能

## Gem5端使用方法
以下是gem5端 perfCCT相关接口

createMeta在fetch阶段给每条dyninst分配一个计数器表项，直接复用dyninst本身的seqNum

updateInstPos则会用来记录指令的在何时到达了什么位置，需要在正确的指令流水线各个阶段调用updateInstPos来更新指令的位置时间

updateInstMeta则会用来记录指令除了位置信息以外的性能计数器，比如在写回阶段记录指令的结果，在lsu里记录指令的访存地址等

commitMeta则是在指令退休时将指令的信息dump到archdb中，便于分析

以上接口调用均需要指令的seqNum来查找指令对应的性能计数器表项，在不使用perfCCT时，计数器表项不占任何内存

```cpp
void createMeta(const DynInstPtr inst);

void updateInstPos(InstSeqNum sn, const PerfRecord pos);

void updateInstMeta(InstSeqNum sn, const PerfRecord meta, uint64_t val);

void commitMeta(InstSeqNum sn);
```

### 开启perfCCT
首先需要开启gem5的archdb，再在configs/example/xiangshan.py里做如下修改

将dump_lifetime改为True即可

```python
test_sys.arch_db = ArchDBer(arch_db_file=args.arch_db_file)
test_sys.arch_db.dump_from_start = args.arch_db_fromstart
test_sys.arch_db.enable_rolling = args.enable_rolling
test_sys.arch_db.dump_l1_pf_trace = False
test_sys.arch_db.dump_mem_trace = False
test_sys.arch_db.dump_l1_evict_trace = False
test_sys.arch_db.dump_l2_evict_trace = False
test_sys.arch_db.dump_l3_evict_trace = False
test_sys.arch_db.dump_l1_miss_trace = False
test_sys.arch_db.dump_bop_train_trace = False
test_sys.arch_db.dump_sms_train_trace = False
test_sys.arch_db.dump_lifetime = False -> True
```

此外还需要同时打开--enable-arch-db, 指定db-file

```python
./build/RISCV/gem5.debug ./configs/example/xiangshan.py --raw-cpt --generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/ifuwidth-riscv64-xs.bin --enable-arch-db --arch-db-file=m5out/test.db

python3 util/perfcct.py m5out/test.db --zoom 1.5 -p 333 --visual | less
```

### 分析脚本
gem5的perfCCT分析脚本位于util/perfcct.py中

gem5生成出数据库后，使用以下命令来dump出可读数据

`python3 util/perfcct.py test.db`

其他选项

--zoom (-z) 尺度缩放

--visual (-v) 文本可视化

--period (-p) tick per cycle, RTL上是1, GEM5上3GHz设置为333, （若是2Ghz设置为500）

举例：

python3 util/perfcct.py test.db --z 1.5 -p 333

![](images/1732854749964-15d18da2-8ccb-43d0-9989-bf5a1959de1f.png)

若添加“--v”参数，则如下

![](images/1732848778754-12280a5e-a824-4d02-809e-1b44b94a2912.png)

其中每个点和每一个字母代表一个周期

f代表指令处于fetch或者ibuffer中

d代表指令处于decode

r代表处于rename

D代表处于DispQue

i代表处于issueQue

a代表处于issue0（og0/arb）

g代表处于issue1（og1/read regfile）或者issue2/og2（如果有的话）

e代表处于FU

b代表处于bypassing,写回的前一个周期，对于单周期指令，e和b会重合，此时字母只会输出b

w代表指令已写回，等待提交，可能会出现一连串的‘w’， 代表指令已完成但还未提交

c代表指令已提交

## RTL端使用方法
目前 perfcct ~~还处于 PR 状态 (~~[~~https://github.com/OpenXiangShan/XiangShan/pull/4007~~](https://github.com/OpenXiangShan/XiangShan/pull/4007)~~)，处于 PerfCCT 分支，需要在最新的香山中~~

+ ~~先在 difftest 下，merge PerfCCT 分支中的 difftest~~
+ ~~在 utility 下，merge PerfCCT 分支中的 utility~~
+ ~~最后在 xiangshan 下，merge PerfCCT 分支~~

perfcct已经合入主线

编译香山，带上 WITH_CHISELDB=1

执行 emu 时，带上 --dump-db --dump-select-db "lifetime"，生成一个 db 文件

**在大机房上：**

```python
import sqlite3 as sql
import argparse
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument('sqldb')
parser.add_argument('-v', '--visual', action='store_true', default=False)
parser.add_argument('-z', '--zoom', action='store', type=float, default=1)
parser.add_argument('-p', '--period', action='store', default=333)

args = parser.parse_args()

sqldb = args.sqldb

tick_per_cycle = int(args.period)
cycle_per_line = int(100 * args.zoom)

stages = ['f','d','r','D','i','a','g','e','b','w','c']

inst_translate_map = {}

def DisAssemble(val):
    # print(val)
    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
    # print(val, hex_val, asm)
    return asm

def non_stage():
    return '.'

def stage(x):
    return stages[x]

def dump_visual(pos, records):
    pos_start = pos[0] % cycle_per_line
    line = ''
    line += '[' + non_stage() * pos_start
    pos_next = pos_start
    last_index = 0
    for i in range(1, len(pos)):
        if (pos[i] == pos[last_index]) or pos[i] == 0:
            continue
        if pos[i] - pos[last_index] >= cycle_per_line - pos_next:
            diff = cycle_per_line - pos_next
            line += f'{stage(last_index)}' * diff + ']\n'
            diff_line = ((pos[i] - pos[last_index]) - diff - 1) // cycle_per_line
            if diff_line > 0:
                line += '[' + f'{stage(last_index)}' * cycle_per_line + ']\n'

            pos_next = pos[i] % cycle_per_line
            line += '[' + f'{stage(last_index)}' * pos_next
        else:
            diff = pos[i] - pos[last_index]
            pos_next = pos[i] % cycle_per_line
            line += f'{stage(last_index)}' * diff
        last_index = i
    if cycle_per_line - pos_next == 0:
        line += ']\n'
        line += f'[{stage(i)}{non_stage() * (cycle_per_line - 1)}]\n'
    else:
        line += f'{stage(i)}' + non_stage() * (cycle_per_line - pos_next - 1) + ']'
    line += str(records)
    print(line)


def dump_txt(pos, records):
    for i in range(len(pos)):
        print(f'{stage(i)}{pos[i]}', end=' ')
    print(records)


dump = dump_txt
if args.visual:
    dump = dump_visual

with sql.connect(sqldb) as con:
    cur = con.cursor()
    cur.execute("SELECT * FROM LifeTimeCommitTrace")
    col_name = [i[0] for i in cur.description]
    col_name = col_name[1:]
    col_name = [i.lower() for i in col_name]
    rows = cur.fetchall()
    for row in rows:
        row = row[1:]
        pos = []
        records = []
        i = 0
        for val in row:
            if col_name[i].startswith('at'):
                pos.append(val//tick_per_cycle)
            elif col_name[i].startswith('pc'):
                if val < 0:
                    val = val + 1 << 64
                records.append(hex(val))
            else:
                if val not in inst_translate_map:
                    inst_translate_map[val] = DisAssemble(val)
                asm = inst_translate_map[val]
                records.append(asm)
            i += 1
        dump(pos, records)
```

```python
import re
from collections import Counter
import sys
import gzip

def parse_instruction(line):
    # something like [c1000 'c_addw a4 a5', '0x800000']
    match = re.search(r"c(\d+) \['([^']+)',\s*'([^']+)'\]", line)
    if match:
        return match.groups()   # commit time + instruction + pc
    return None

def is_branch_instruction(instr):
    branch_instructions = ['beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'beqz', 'bnez', 'j', 'jal', 'jalr', 'ret', 'mret', 'sret']
    branch_instructions += ['c_beqz', 'c_bnez', 'c_j', 'c_jal', 'c_jr', 'c_jalr']
    return any(instr.split()[0].startswith(branch) for branch in branch_instructions)

def analyze_commit_trace(file_path):
    basic_blocks = Counter()
    basic_blocks_commit_time = Counter()
    current_block = []
    block_begin_commit_time = 0

    open_func = gzip.open if file_path.endswith('.gz') else open
    mode = 'rt' if file_path.endswith('.gz') else 'r'

    with open_func(file_path, mode) as file:
        commit_time = 0
        for line in file:
            parsed = parse_instruction(line) # commit time[0] + instruction[1] + pc[2]
            if parsed:
                if len(current_block) == 0:
                    block_begin_commit_time = int(parsed[0])
                commit_time = int(parsed[0])
                current_block.append((parsed[1], parsed[2]))
                if is_branch_instruction(parsed[1]):
                    if current_block:
                        # print(current_block)
                        basic_blocks[tuple(current_block)] += 1
                        basic_blocks_commit_time[tuple(current_block)] += int(parsed[0]) - block_begin_commit_time
                        current_block = []

        if current_block:
            basic_blocks[tuple(current_block)] += 1
            basic_blocks_commit_time[tuple(current_block)] += commit_time - block_begin_commit_time
    return basic_blocks, basic_blocks_commit_time

def main(file_path):
    basic_blocks, basic_blocks_commit_time = analyze_commit_trace(file_path)

    print("Top 10 most common basic blocks:")
    total_count = sum(basic_blocks.values())
    total_cycle = sum(basic_blocks_commit_time.values())
    print(f"Total cycle: {total_cycle}")
    for block, count in basic_blocks.most_common(10):
        percentage = (count / total_count) * 100
        print(f"Count: {count} ({percentage:.2f}%) cycle: {basic_blocks_commit_time[block]}", end='')
        print(f" ratio ({basic_blocks_commit_time[block] / total_cycle * 100:.2f}%)")
        print("Instructions:")
        for instr, pc in block:
            print(f"  {pc}: {instr}")
        print()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Please provide the file path as a command line argument.")
        sys.exit(1)
    file_path = sys.argv[1]
    main(file_path)
```

```python
from collections import Counter
import sqlite3 as sql
import argparse
import numpy as np
import subprocess
from tqdm import tqdm


StageNameShort = ['f', 'd', 'r', 'D', 'i', 'a', 'g', 'e', 'b', 'w', 'c']
StageNameLong = ['fetch', 'decode', 'rename', 'dispatch', 'issue',
                 'arb', 'read', 'execute', 'bypass', 'writeback', 'commit']


def DisAssemble(val):
    # print(val)
    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
    # print(val, hex_val, asm)
    return asm


def ReadDB(sqldb, start_clock: int, end_clock: int, period: int, inter_gap: bool, inner_gap: bool):
    inst_pos_clock = []
    inst_records = []
    inst_translate_map = {}

    with sql.connect(sqldb) as con:
        cur = con.cursor()

        clock_pick_cmd = f"WHERE AtCommit >= {start_clock*period} "
        if end_clock >= start_clock:
            clock_pick_cmd += f"AND AtCommit <= {end_clock*period} "

        cur.execute(
            f"SELECT * FROM LifeTimeCommitTrace {clock_pick_cmd} ORDER BY ID ASC")
        col_name = [i[0].lower() for i in cur.description[1:]]
        rows = cur.fetchall()

    for row in tqdm(rows, desc='Reading DB'):
        row = row[1:]
        pos_clock_cycles = []
        pos_index = 0
        pc = None
        asm = None

        for val in row:
            # print(f"{col_name[pos_index]}, {val}")
            if col_name[pos_index].startswith('at'):
                pos_clock_cycles.append(float(val//period))
            elif col_name[pos_index].startswith('pc'):
                if val < 0:
                    # pc is unsigned, but sqlite3 only supports signed integer [-2^63, 2^63-1]
                    # if real pc > 2^63-1, it will be stored as negative number (real pc - 2^64)
                    # when read a negtive pc, real pc = negtive pc + 2^64
                    val = val + (1 << 64)
                pc = hex(val)
            elif col_name[pos_index].startswith('disasm'):
                if args.platform == 'rtl':
                    if val not in inst_translate_map:
                        inst_translate_map[val] = DisAssemble(val)
                    asm = inst_translate_map[val]
                else:
                    asm = val
            pos_index += 1

        inst_pos_clock.append(pos_clock_cycles)
        inst_records.append(tuple([pc, asm]))

    if inter_gap:
        inst_pos_clock = np.diff(inst_pos_clock, axis=0)
    elif inner_gap:
        inst_pos_clock = np.diff(inst_pos_clock, axis=1)

    inst_records = inst_records[len(inst_records) - len(inst_pos_clock):]

    inst_clock_info: dict = {}
    for inst_index in range(len(inst_records)):
        if inst_records[inst_index] not in inst_clock_info:
            inst_clock_info[inst_records[inst_index]] = []
        inst_clock_info[inst_records[inst_index]].append(
            inst_pos_clock[inst_index])

    inst_avg_clock_info: dict = {}
    for key in inst_clock_info.keys():
        inst_avg_clock_info[key] = np.mean(inst_clock_info[key], axis=0)

    return inst_records, inst_pos_clock, inst_avg_clock_info


def IsBranchInst(instr: str) -> bool:
    branch_instructions = ['beq', 'bne', 'blt', 'bge', 'bltu',
                           'bgeu', 'beqz', 'bnez', 'j', 'jal', 'jalr', 'ret']
    branch_instructions += ['c_beqz', 'c_bnez',
                            'c_j', 'c_jal', 'c_jr', 'c_jalr']
    return any(instr.split()[0].startswith(branch) for branch in branch_instructions)


def ExtractBasicBlocks(pc_inst_list: tuple[str, str]) -> Counter:
    basic_blocks = Counter()
    current_block = []

    for i, (pc, inst) in tqdm(enumerate(pc_inst_list), desc='Analyzing Traces'):
        current_block.append((pc, inst))
        if IsBranchInst(inst) and current_block:
            basic_blocks[tuple(current_block)] += 1
            current_block = []
    if current_block:
        basic_blocks[tuple(current_block)] += 1

    return basic_blocks


def bbl_main(inst_info, inst_avg_clock_info, inter_gap, inner_gap):
    basic_blocks = ExtractBasicBlocks(inst_info)

    mode_str = "inter-gap" if inter_gap else "inner-gap" if inner_gap else "normal"

    print(f"Top 10 most common basic blocks (mode:{mode_str}):")
    for block, count in basic_blocks.most_common(10):
        print()
        print(f"Count: {count}")
        print("Instructions:")
        pc_header = f"{'PC':18}"
        instr_header = f"{'Instruction':30}"
        clock_head = ":".join([f"{stage_name:>9}" for stage_name in StageNameLong])

        if inter_gap or inner_gap:
            header = f"  {pc_header} : {instr_header} : {clock_head}"
        else:
            header = f"  {pc_header} : {instr_header} "

        print(header)

        for pc, instr in block:
            if inter_gap or inner_gap:
                formatted_clock = [
                    f"{clock:9.2f}" for clock in inst_avg_clock_info[(pc, instr)]]
                formatted_clock = ":".join(formatted_clock)
                print(f"  {pc:18} : {instr:30} : {formatted_clock}")
            else:
                print(f"  {pc:18} : {instr:30} ")


def perfcct_main(inst_info, inst_pos_clock_info, start_pc, end_pc, attention_pc: list[str], only_attention: bool):

    for i, (pc, asm) in enumerate(inst_info):
        pos = inst_pos_clock_info[i]

        if not only_attention and \
                (int(pc, 16) < int(start_pc, 16) or int(pc, 16) > int(end_pc, 16)):
            continue

        if only_attention and (pc not in attention_pc):
            continue

        print(f"{pc:18} : {asm:30}", end=' : ')

        for j, pos_clock in enumerate(pos):
            print(f'{StageNameShort[j]} {int(pos_clock)}', end=' : ')

        if pc in attention_pc:
            print("<<====", end=' ')

        print()
        if pc == end_pc:
            print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('sqldb', action='store',
                        help='Path to the SQLite database')
    parser.add_argument('-p', '--period', action='store',
                        type=int,
                        default=333,
                        help='Number of ticks per clock cycle')
    parser.add_argument('-P', '--platform', action='store',
                        choices=['gem5', 'rtl'],
                        default='gem5',
                        help='Platform to analyze')
    parser.add_argument('-s', '--start-clock', action='store',
                        type=int,
                        default=0,
                        help='Start clock to analyze')
    parser.add_argument('-e', '--end-clock', action='store',
                        type=int,
                        default=-1,
                        help='End clock to analyze')
    parser.add_argument('-n', '--num-insts', action='store',
                        type=int,
                        default=-1,
                        help='MAX Number of instructions to analyze')
    parser.add_argument('--inter-gap', action='store_true',
                        default=False,
                        help='Analyze inter-gap')
    parser.add_argument('--inner-gap', action='store_true',
                        default=False,
                        help='Analyze inner-gap')
    parser.add_argument('--tool', action='store',
                        choices=['perfcct', 'bbl'],
                        default='bbl',
                        help='Mode to analyze')
    parser.add_argument('--attention-pc', action='store',
                        nargs='+',
                        default=[],
                        help='Attention PC')
    parser.add_argument('--start-pc', action='store',
                        type=str,
                        default="0x0",
                        help='Start PC to analyze, a hex value start with 0x')
    parser.add_argument('--end-pc', action='store',
                        type=str,
                        default="0xffffffffffffffff", help='End PC to analyze, a hex value start with 0x')
    parser.add_argument('--only-attention', action='store_true',
                        default=False,
                        help='Only print attention PC')

    args = parser.parse_args()

    if args.platform.lower() == 'rtl':
        args.period = 1

    if args.inter_gap and args.inner_gap:
        raise ValueError("Cannot set both inter-gap and inner-gap to True")

    inst_info, inst_pos_clock_info, inst_avg_clock_info = ReadDB(
        args.sqldb, args.start_clock, args.end_clock, args.period, args.inter_gap, args.inner_gap)

    if args.tool == 'perfcct':
        perfcct_main(inst_info, inst_pos_clock_info, args.start_pc,
                     args.end_pc, args.attention_pc, args.only_attention)
    elif args.tool == 'bbl':
        bbl_main(inst_info, inst_avg_clock_info,
                 args.inter_gap, args.inner_gap)

```

```shell
# 使用文本格式输出
python3 perfcct.py <db path> --zoom 2 -p 1 | gzip > trace.gz
# 得到 hotloop 信息
python3 hotloop.py trace.gz > hotloop.txt
# 分析指令之间的 inter-gap
python3 ClockAnalysis.py <db path> -p 1 -P rtl --tool bbl --inter-gap > inter_gap.txt
# 分析指令内的 inner-gap
python3 ClockAnalysis.py <db path> -p 1 -P rtl --tool bbl --inner-gap > inner_gap.txt
# 可视化指令执行的时空图
python3 perfcct.py <db path> --zoom 2 -p 1 -v | less
```

**遇到问题请及时联系**[@Lurker](undefined/lurker-jhyqa)[@邱泽源](undefined/u26707279)[@常庆宇](undefined/changqingyu-sqxjx)[@李昕](undefined/brucelee-7hknq)



## RTL端添加自定义计数器
在utility/src/ChiselPerfCCT.scala中，能找到如下接口

其中tick，createInstMetaAtFetch，updateInstPos，commitInstMeta这几个接口是固定接口外

只有updateInstMeta是用户可自定义的接口，

其传参规则如下：

sn：指令的seqNum

meta：一个枚举类id，表示指令的meta标记

data：表示需要写入的meta的值

![](images/1743156324995-a76a7e80-09e0-4a84-8f9d-0e17a9ffc0a5.png)

### 样例
若我们想记录一条指令的写回的值，首先我们需要在InstRecord里添加一个meta,这里记为Result

![](images/1743157134155-1a114354-7691-4fb4-af9f-cc46e4da29cd.png)

![](images/1743157301024-13ac6a41-1aac-4167-bbb4-9f2ef034edf7.png)

修改对应的c++代码，添加数据库

![](images/1743157151659-eb5043ee-4082-4872-af91-ab55eb4c21e6.png)

![](images/1743157185974-e6558ce7-03a8-4398-a7b6-dc8d6157f3fd.png)

修改对应的c++函数

![](images/1743157351895-d1f4bbd3-d388-4ee3-a8fa-793c168bd9cb.png)

![](images/1743157432788-fc89f038-ce67-4900-8e62-3496a82364ff.png)

然后在RTL chisel代码的正确位置调用updateInstMeta（只是举例，不代表真实RTL代码）

![](images/1743157725346-0dfb7209-6a47-4953-ace7-3eeebad09d62.png)

最后开启perfcct即可在数据库中dump 指令的写回值



！！另外注意，为了削减数据库的大小，对于“访存地址”这类不是所有指令都有的meta,建议单独开一个数据库！！

