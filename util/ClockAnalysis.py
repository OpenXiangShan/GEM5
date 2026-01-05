from collections import Counter
from functools import lru_cache
import sqlite3 as sql
import argparse
import numpy as np
import subprocess
import pandas as pd
from tqdm import tqdm

pd.set_option('display.width', 1000) # 设置显示宽度
pd.set_option('display.max_columns', None) # 显示所有列
pd.set_option('display.max_rows', None) # 显示所有行
pd.set_option('display.float_format', '{:.2f}'.format)

StageNameShort = ['f', 'd', 'r', 'D', 'i', 'a', 'g', 'e', 'b', 'w', 'c']
StageNameLong = ['fetch', 'decode', 'rename', 'dispatch', 'issue',
                 'arb', 'read', 'execute', 'bypass', 'writeback', 'commit']

@lru_cache(maxsize=None)
def DisAssemble(val):
    if type(val) == str:
        return val
    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
    return asm

def ReadDB(sqldb, start_clock: int, end_clock: int, inter_gap: bool, inner_gap: bool):
    inst_pos_clock = []
    inst_records = []

    with sql.connect(sqldb) as con:
        clock_pick_cmd = f"and AtCommit >= {start_clock*period} "
        if end_clock >= start_clock:
            clock_pick_cmd += f"AND AtCommit <= {end_clock*period} "

        df = pd.read_sql_query(f"SELECT * FROM LifeTimeCommitTrace where AtCommit != 0 {clock_pick_cmd} ORDER BY ID ASC", con)
        col_name = [i.lower() for i in df.columns]
        rows = df.to_numpy()

    pos_begin = col_name.index('atfetch')
    pos_end = col_name.index('atcommit') + 1
    disasm_idx = col_name.index('disasm')
    pc_idx = col_name.index('pc')

    for row in tqdm(rows, desc='Reading DB'):
        pc = row[pc_idx]
        asm = DisAssemble(row[disasm_idx])
        inst_pos_clock.append(row[pos_begin:pos_end])
        inst_records.append(tuple([pc, asm]))

    if inter_gap:
        inst_pos_clock = np.diff(inst_pos_clock, axis=0)
    elif inner_gap:
        inst_pos_clock = np.diff(inst_pos_clock, axis=1)

    inst_records = inst_records[len(inst_records) - len(inst_pos_clock):]

    inst_clock_info: dict = {}
    for inst_index in range(len(inst_records)):
        pc = inst_records[inst_index][0]
        if pc not in inst_clock_info:
            inst_clock_info[pc] = []

        inst_clock_info[pc].append(
            inst_pos_clock[inst_index])

    inst_avg_clock_info: dict = {}
    for key in inst_clock_info.keys():
        inst_avg_clock_info[key] = np.mean(inst_clock_info[key], axis=0)
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

def bbl_main(inst_info, inst_avg_clock_info, inter_gap, inner_gap):
    basic_blocks, jmp_map = ExtractBasicBlocks(inst_info)

    mode_str = "inter-gap" if inter_gap else "inner-gap" if inner_gap else "normal"

    print(f"Top 10 most common basic blocks (mode:{mode_str}):")
    if inner_gap:
        global StageNameLong
        StageNameLong = StageNameLong[1:]
    for block, count in basic_blocks.most_common():
        df_col_name = ['PC', 'Instruction'] + StageNameLong
        start_pc = block[0][0]
        prev_block_pc = jmp_map.get(start_pc, [])
        if prev_block_pc:
            prev_block_pc = ', '.join([hex(pc) for pc in prev_block_pc])
        else:
            prev_block_pc = 'N/A'

        df_data = [[hex(pc), instr] + (inst_avg_clock_info[pc] / period).tolist() for pc, instr in block]
        df = pd.DataFrame(df_data, columns=df_col_name)
        # 对每列数据的commit时间求和
        total_commit_time = sum([row[-1] for row in df_data])

        print()
        print(f"Count: {count}, Total commit time: {total_commit_time:.2f} cycles, jumped from: {prev_block_pc}")
        print("Instructions:")
        print(df)

def perfcct_main(inst_info, inst_pos_clock_info, start_pc, end_pc, attention_pc, only_attention: bool):

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

    period = args.period

    inst_info, inst_pos_clock_info, inst_avg_clock_info = ReadDB(
        args.sqldb, args.start_clock, args.end_clock, args.inter_gap, args.inner_gap)

    if args.tool == 'perfcct':
        perfcct_main(inst_info, inst_pos_clock_info, args.start_pc,
                     args.end_pc, args.attention_pc, args.only_attention)
    elif args.tool == 'bbl':
        bbl_main(inst_info, inst_avg_clock_info,
                 args.inter_gap, args.inner_gap)
