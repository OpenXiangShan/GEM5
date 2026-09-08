import sqlite3 as sql
import argparse
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument('sqldb')
parser.add_argument('-v', '--visual', action='store_true', default=False)
parser.add_argument('-z', '--zoom', action='store', type=float, default=1)
parser.add_argument('-p', '--period', action='store', default=333)
parser.add_argument('-r', '--rtl_dasm', action='store_true', default=False,
                    help='used for rtl log analysis, use dasm to disassemble the instructions')
parser.add_argument('--tid', type=int, default=None,
                    help='only show instructions from this hardware thread')

args = parser.parse_args()

sqldb = args.sqldb

tick_per_cycle = int(args.period)
cycle_per_line = int(100 * args.zoom)

stages = ['f','d','r','D','i','a','g','e','b','w','c']

inst_translate_map = {}

def DisAssemble(val):
    if not args.rtl_dasm:
        return val
    # print(val)
    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
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
    table_columns = [row[1].lower() for row in
                     cur.execute("PRAGMA table_info(LifeTimeCommitTrace)")]
    if args.tid is not None:
        if 'tid' not in table_columns:
            parser.error("the trace database has no TID column")
        if args.tid < 0:
            parser.error("--tid must be non-negative")
        cur.execute("SELECT * FROM LifeTimeCommitTrace WHERE TID = ?",
                    (args.tid,))
    else:
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
            elif col_name[i] == 'tid':
                records.append(f'tid={val}')
            else:
                if val not in inst_translate_map:
                    inst_translate_map[val] = DisAssemble(val)
                asm = inst_translate_map[val]
                records.append(asm)
            i += 1
        dump(pos, records)
