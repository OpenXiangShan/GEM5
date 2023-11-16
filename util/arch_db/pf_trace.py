import sqlite3
import collections
import argparse
import os.path as osp

parser = argparse.ArgumentParser()
# add a positional argument
parser.add_argument('tick', type=int, help='roi start tick')
parser.add_argument('--pc', type=str, help='observe_pc', action='store')
parser.add_argument('--show', type=str, help='observe_pc', action='store', default='unseen')
args = parser.parse_args()

con = sqlite3.connect('../../util/warmup_scripts/Default-tag-2023-11-16-03-55-33/mem_trace.db')
cur = con.cursor()
res = cur.execute('SELECT * FROM L1PFTrace')
cycle = 333
seen_addr = {}
recent_lines = []
show = args.show
# outf = open(osp.join('arch_db_res', f'{show}_trace.txt'), 'w')
if show.endswith('of_pc'):
    outf = open(f'{args.pc}_{show}_trace.txt', 'w')
else:
    outf = open(f'{show}_trace.txt', 'w')
recent_len = 20
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
observe_pc = int(args.pc, 16)
observe_last_addr = 0
trigger_pcs = {}
pf_source = 'x'
for x in res:
    id_, tick, pc, vaddr, pf_addr, PFSrc = x
    aligned_vaddr = (vaddr >> 6) << 6
    if int(tick) < args.tick:
        continue
    trigger_pcs[pc] = trigger_pcs.get(pc, 0) + 1
    if show.endswith('of_pc') and pc != observe_pc:
        continue
    if PFSrc != 10:
        continue
    deltas = []
    for i in range(recent_len):
        deltas.append(vaddr - recent_cold_misses[-1-i])
    recent_cold_misses.append(vaddr)

    print(tick, hex(pc), hex(vaddr), hex(aligned_vaddr), hex(pf_addr), str(PFSrc).ljust(5),
          (pf_addr - vaddr)//64, deltas, file=outf)
outf.close()

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_trigger_pc:
    print(hex(x[0]), x[1])
