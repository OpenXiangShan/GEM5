import argparse
import sqlite3
import subprocess
from functools import lru_cache
import pandas as pd
import tqdm

parser = argparse.ArgumentParser()

parser.add_argument('sqldb')
parser.add_argument('-p', '--period', action='store', default=1)
args = parser.parse_args()

period = int(args.period)

replayReason = {
    'C' : 'Cache Miss',
    'T' : 'TLB Miss',
    'B' : 'Bank Conflict',
    'N' : 'Nuke',
    'S' : 'Cache Stall',
    'R' : 'RAR replay',
    'W' : 'RAW replay',
    'O' : 'Other Reason',
}


def getReplayReason(code):
    reasons = {
        'C' : 0,
        'T' : 0,
        'B' : 0,
        'N' : 0,
        'S' : 0,
        'R' : 0,
        'W' : 0,
        'O' : 0,
    }
    for c in code:
        if c in reasons:
            reasons[c] += 1
    return reasons

stages = ['d','r','D','i','a','g','e','b','w','c']

@lru_cache(maxsize=None)
def DisAssemble(val):
    if type(val) is str:
        return val

    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    asm = subprocess.run(command, shell=True, capture_output=True,
                         text=True, check=True).stdout.strip()
    return asm


with sqlite3.connect(args.sqldb) as db:
    cur = db.cursor()
    cur.execute("SELECT LifeTimeCommitTrace.*, LoadLifeTimeCommitTrace.* from LifeTimeCommitTrace INNER JOIN LoadLifeTimeCommitTrace ON LifeTimeCommitTrace.ID = LoadLifeTimeCommitTrace.ID WHERE LifeTimeCommitTrace.AtCommit != 0;")
    col_name = [i[0] for i in cur.description]
    pc_idx = col_name.index('PC')
    asm_idx = col_name.index('DisAsm')
    atfetch_idx = col_name.index('AtFetch')
    atcommit_idx = col_name.index('AtCommit')
    replaystr_idx = col_name.index('ReplayStr')
    rows = cur.fetchall()
    load_map = {}

    def statistics(data, total):
        pos = data[atfetch_idx: atcommit_idx + 1]
        disp_idx = 3
        if pos[disp_idx] == 0:
            pos[disp_idx] = pos[disp_idx - 1]
        pos_diff = [j - i for i, j in zip(pos[:-1], pos[1:])]
        replays = getReplayReason(data[replaystr_idx])
        # [count, pos_diff, replay reason, disasm, pc]
        if total:
            total[0] += 1
            total[1] = [x + y for x, y in zip(total[1], pos_diff)]
            total[2] = {k: total[2].get(k, 0) + v for k, v in replays.items()}
            return total
        else:
            return [1, pos_diff, replays, DisAssemble(data[asm_idx]), data[pc_idx]]

    for row in tqdm.tqdm(rows):
        row = list(row)
        pc = row[pc_idx]
        if pc not in load_map:
            load_map[pc] = statistics(row, None)
        else:
            load_map[pc] = statistics(row, load_map[pc])

    # sort by total count from high to low
    sorted_loads = sorted(load_map.items(), key=lambda x: x[1][0], reverse=True)
    # get top 20
    sorted_loads = sorted_loads[:20]
    # flatten the result
    for i, data in enumerate(sorted_loads):
        count, pos_diff, replays, disasm, pc = data[1]
        replays = [v for k, v in replays.items()]
        sorted_loads[i] = [count, hex(pc), disasm] + [x / count / period for x in pos_diff]+ replays

    col_name = ['Count', 'PC', 'DisAsm'] + stages + list(replayReason.values())

    df = pd.DataFrame(sorted_loads, columns=col_name)
    pd.set_option('expand_frame_repr', False)
    pd.set_option('display.max_columns', None)
    print(df)

