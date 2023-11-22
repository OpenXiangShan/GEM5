import sqlite3
import collections
import argparse
import os.path as osp
import heapq

parser = argparse.ArgumentParser()
# add a positional argument
parser.add_argument('tick', type=int, help='roi start tick')
parser.add_argument('--pc', type=str, help='observe_pc', action='store')
parser.add_argument('--show', type=str, help='observe_pc', action='store', default='unseen')
args = parser.parse_args()

db = '../warmup_scripts/Default-tag-2023-11-23-09-27-15/mem_trace.db'

con = sqlite3.connect(db)
cur = con.cursor()
bop_trace = cur.execute('SELECT * FROM BOPTrainTrace')

con2 = sqlite3.connect(db)
cur2 = con2.cursor()
pf_trace = cur2.execute('SELECT * FROM L1PFTrace')

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
# observe_pc = int(args.pc, 16)
observe_last_addr = 0
trigger_pcs = {}
pf_source = 'x'

for count, x in enumerate(heapq.merge(bop_trace, pf_trace, key=lambda a: a[1])):
    if len(x) == 8:  # bop trace
        id_, tick, type_str, old_addr, vaddr, offset, score, miss = x
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_old_addr = (old_addr >> 6) << 6
        if int(tick) < args.tick:
            continue

        print(tick, hex(aligned_old_addr), hex(aligned_vaddr), score, miss, offset, "Train", file=outf)

    elif len(x) == 6:  # pf trace
        id_, tick, pc, vaddr, pf_addr, PFSrc = x
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_pf_addr = (pf_addr >> 6) << 6
        offset = int(aligned_pf_addr - aligned_vaddr) // 64
        if PFSrc != 4:
            continue
        if int(tick) < args.tick:
            continue
        print(tick, hex(aligned_vaddr), hex(aligned_pf_addr), offset, "Prefe", file=outf)
    else:
        print(x)
        raise


outf.close()

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_trigger_pc:
    print(hex(x[0]), x[1])
