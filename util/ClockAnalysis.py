from collections import Counter
from functools import lru_cache
import sqlite3 as sql
import argparse
import subprocess

try:
    from tqdm import tqdm
except ImportError:
    def tqdm(iterable, **_kwargs):
        return iterable

StageNameShort = ['f', 'd', 'r', 'D', 'i', 'a', 'g', 'e', 'b', 'w', 'c']
StageNameLong = ['fetch', 'decode', 'rename', 'dispatch', 'issue',
                 'arb', 'read', 'execute', 'bypass', 'writeback', 'commit']

@lru_cache(maxsize=None)
def DisAssemble(val):
    if isinstance(val, str):
        return val
    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
    return asm

def DiffAxis0(rows):
    return [[curr[idx] - prev[idx] for idx in range(len(curr))]
            for prev, curr in zip(rows, rows[1:])]


def DiffAxis1(rows):
    return [[row[idx + 1] - row[idx] for idx in range(len(row) - 1)]
            for row in rows]


def MeanRows(rows):
    if not rows:
        return []

    row_len = len(rows[0])
    sums = [0.0] * row_len
    for row in rows:
        for idx, value in enumerate(row):
            sums[idx] += value

    count = len(rows)
    return [value / count for value in sums]


def FormatCell(value):
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def RenderTable(columns, rows):
    formatted_rows = [[FormatCell(value) for value in row] for row in rows]
    widths = [len(str(column)) for column in columns]

    for row in formatted_rows:
        for idx, cell in enumerate(row):
            widths[idx] = max(widths[idx], len(cell))

    header = '  '.join(f"{str(column):<{widths[idx]}}" for idx, column in enumerate(columns))
    body = [
        '  '.join(f"{cell:<{widths[idx]}}" for idx, cell in enumerate(row))
        for row in formatted_rows
    ]
    return '\n'.join([header] + body)


def FormatPC(pc):
    if isinstance(pc, str):
        return pc
    return hex(pc)


def ParsePC(pc):
    if isinstance(pc, str):
        return int(pc, 16)
    return int(pc)


def BuildQuery(start_clock: int, end_clock: int, num_insts: int):
    clock_pick_cmd = f"and AtCommit >= {start_clock*period} "
    if end_clock >= start_clock:
        clock_pick_cmd += f"AND AtCommit <= {end_clock*period} "

    limit_cmd = f"LIMIT {num_insts}" if num_insts > 0 else ""
    return (
        "SELECT * FROM LifeTimeCommitTrace "
        f"where AtCommit != 0 {clock_pick_cmd} ORDER BY ID ASC {limit_cmd}"
    )


def IterInstMetric(sqldb, start_clock: int, end_clock: int, inter_gap: bool,
                   inner_gap: bool, num_insts: int):
    with sql.connect(sqldb) as con:
        cursor = con.execute(BuildQuery(start_clock, end_clock, num_insts))
        col_name = [item[0].lower() for item in cursor.description]

        pos_begin = col_name.index('atfetch')
        pos_end = col_name.index('atcommit') + 1
        disasm_idx = col_name.index('disasm')
        pc_idx = col_name.index('pc')
        prev_pos_clock = None

        for row in tqdm(cursor, desc='Reading DB'):
            pc = row[pc_idx]
            asm = DisAssemble(row[disasm_idx])
            pos_clock = list(row[pos_begin:pos_end])

            if inter_gap:
                if prev_pos_clock is None:
                    prev_pos_clock = pos_clock
                    continue
                metric = [pos_clock[idx] - prev_pos_clock[idx]
                          for idx in range(len(pos_clock))]
                prev_pos_clock = pos_clock
            elif inner_gap:
                metric = DiffAxis1([pos_clock])[0]
            else:
                metric = pos_clock

            yield pc, asm, metric


def ReadDB(sqldb, start_clock: int, end_clock: int, inter_gap: bool,
           inner_gap: bool, num_insts: int):
    inst_pos_clock = []
    inst_records = []

    inst_clock_info: dict = {}
    for pc, asm, metric in IterInstMetric(
            sqldb, start_clock, end_clock, inter_gap, inner_gap, num_insts):
        inst_records.append((pc, asm))
        inst_pos_clock.append(metric)

        if pc not in inst_clock_info:
            inst_clock_info[pc] = []

        inst_clock_info[pc].append(metric)

    inst_avg_clock_info: dict = {}
    for key in inst_clock_info.keys():
        inst_avg_clock_info[key] = MeanRows(inst_clock_info[key])
    return inst_records, inst_pos_clock, inst_avg_clock_info


def IsBranchInst(instr: str) -> bool:
    branch_instructions = ['beq', 'bne', 'blt', 'bge', 'bltu',
                           'bgeu', 'beqz', 'bnez', 'j', 'jr', 'jal', 'jalr', 'ret']
    return any(branch in instr for branch in branch_instructions)


def ExtractBasicBlocks(pc_inst_list) -> Counter:
    basic_blocks = Counter()
    current_block = []
    jmp_map = {} # target -> br's pc

    last_jmppc = 0
    for i, (pc, inst) in tqdm(enumerate(pc_inst_list), desc='Analyzing Traces'):

        if len(current_block) == 0:
            if pc not in jmp_map:
                jmp_map[pc] = set()
            jmp_map[pc].add(last_jmppc)

        current_block.append((pc, inst))
        if IsBranchInst(inst):
            basic_blocks[tuple(current_block)] += 1
            current_block = []
            last_jmppc = pc
    if current_block:
        basic_blocks[tuple(current_block)] += 1

    return basic_blocks, jmp_map

def AnalyzeBasicBlocks(sqldb, start_clock: int, end_clock: int, inter_gap: bool,
                       inner_gap: bool, num_insts: int):
    basic_blocks = Counter()
    current_block = []
    jmp_map = {}
    last_jmppc = 0
    inst_clock_sum = {}
    inst_clock_count = {}

    for pc, asm, metric in IterInstMetric(
            sqldb, start_clock, end_clock, inter_gap, inner_gap, num_insts):
        if pc not in inst_clock_sum:
            inst_clock_sum[pc] = [0.0] * len(metric)
            inst_clock_count[pc] = 0

        for idx, value in enumerate(metric):
            inst_clock_sum[pc][idx] += value
        inst_clock_count[pc] += 1

        if len(current_block) == 0:
            if pc not in jmp_map:
                jmp_map[pc] = set()
            jmp_map[pc].add(last_jmppc)

        current_block.append((pc, asm))
        if IsBranchInst(asm):
            basic_blocks[tuple(current_block)] += 1
            current_block = []
            last_jmppc = pc

    if current_block:
        basic_blocks[tuple(current_block)] += 1

    inst_avg_clock_info = {}
    for pc, sums in inst_clock_sum.items():
        count = inst_clock_count[pc]
        inst_avg_clock_info[pc] = [value / count for value in sums]

    return basic_blocks, jmp_map, inst_avg_clock_info


def bbl_main(basic_blocks, jmp_map, inst_avg_clock_info, inter_gap, inner_gap):

    mode_str = "inter-gap" if inter_gap else "inner-gap" if inner_gap else "normal"

    print(f"Most common basic blocks (mode:{mode_str}):")
    if inner_gap:
        global StageNameLong
        StageNameLong = StageNameLong[1:]
    for block, count in basic_blocks.most_common():
        df_col_name = ['PC', 'Instruction'] + StageNameLong
        start_pc = block[0][0]
        prev_block_pc = jmp_map.get(start_pc, [])
        if prev_block_pc:
            prev_block_pc = ', '.join([FormatPC(pc) for pc in prev_block_pc])
        else:
            prev_block_pc = 'N/A'

        df_data = [[FormatPC(pc), instr] + [value / period for value in inst_avg_clock_info[pc]]
                   for pc, instr in block]
        # 对每列数据的commit时间求和
        total_commit_time = sum([row[-1] for row in df_data])

        print()
        print(f"Count: {count}, Total commit time: {total_commit_time:.2f} cycles, jumped from: {prev_block_pc}")
        print("Instructions:")
        print(RenderTable(df_col_name, df_data))

def perfcct_main(inst_info, inst_pos_clock_info, start_pc, end_pc, attention_pc, only_attention: bool):
    start_pc_int = ParsePC(start_pc)
    end_pc_int = ParsePC(end_pc)
    attention_pc_int = {ParsePC(pc) for pc in attention_pc}

    for i, (pc, asm) in enumerate(inst_info):
        pos = inst_pos_clock_info[i]
        pc_int = ParsePC(pc)
        pc_str = FormatPC(pc)

        if not only_attention and \
                (pc_int < start_pc_int or pc_int > end_pc_int):
            continue

        if only_attention and (pc_int not in attention_pc_int):
            continue

        print(f"{pc_str:18} : {asm:30}", end=' : ')

        for j, pos_clock in enumerate(pos):
            print(f'{StageNameShort[j]} {int(pos_clock)}', end=' : ')

        if pc_int in attention_pc_int:
            print("<<====", end=' ')

        print()
        if pc_int == end_pc_int:
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

    period = args.period

    if args.tool == 'perfcct':
        inst_info, inst_pos_clock_info, inst_avg_clock_info = ReadDB(
            args.sqldb, args.start_clock, args.end_clock, args.inter_gap,
            args.inner_gap, args.num_insts)
        perfcct_main(inst_info, inst_pos_clock_info, args.start_pc,
                     args.end_pc, args.attention_pc, args.only_attention)
    elif args.tool == 'bbl':
        basic_blocks, jmp_map, inst_avg_clock_info = AnalyzeBasicBlocks(
            args.sqldb, args.start_clock, args.end_clock, args.inter_gap,
            args.inner_gap, args.num_insts)
        bbl_main(basic_blocks, jmp_map, inst_avg_clock_info,
                 args.inter_gap, args.inner_gap)
