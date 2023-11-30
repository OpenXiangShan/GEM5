import sqlite3
import collections
import argparse
import os
import os.path as osp
import heapq

parser = argparse.ArgumentParser()
# add a positional argument
parser.add_argument('tick', type=int, help='roi start tick')
parser.add_argument('--pc', type=str, help='observe_pc', action='store')
parser.add_argument('--show', type=str, help='observe_pc', action='store', default='unseen')
args = parser.parse_args()

# get newest dir in ../warmup_scripts
all_subdirs = [d for d in os.listdir('../warmup_scripts') if osp.isdir(osp.join('../warmup_scripts', d))]
lastest_subdir = max(all_subdirs, key=lambda x: osp.getmtime(osp.join('../warmup_scripts', x)))
db = osp.join('../warmup_scripts', lastest_subdir, 'mem_trace.db')
print('Processing', db)

con = sqlite3.connect(db)
cur = con.cursor()
bop_trace = cur.execute('SELECT * FROM BOPTrainTrace')

con2 = sqlite3.connect(db)
cur2 = con2.cursor()
pf_trace = cur2.execute('SELECT * FROM L1PFTrace')

con3 = sqlite3.connect(db)
cur3 = con3.cursor()
acc_trace = cur3.execute('SELECT * FROM MemTrace')

con4 = sqlite3.connect(db)
cur4 = con4.cursor()
l1_evict_trace = cur4.execute('SELECT * FROM CacheEvictTrace')

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
completed = 0

for count, x in enumerate(heapq.merge(bop_trace, pf_trace, acc_trace, l1_evict_trace, key=lambda a: a[1])):
    if len(x) == 6 and type(x[-1]) == str:  # l1 evict trace
        # curTick(), paddr, curCycle, this->name().c_str());
        id_, tick, paddr, curCycle, level, site = x
        print(tick, hex(paddr), f'L{level}', "Evict", file=outf)

    elif len(x) == 6:  # pf trace
        id_, tick, pc, vaddr, pf_addr, PFSrc = x
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_pf_addr = (pf_addr >> 6) << 6
        offset = int(aligned_pf_addr - aligned_vaddr) // 64
        if PFSrc != 4 and PFSrc != 10:
            continue
        if int(tick) < args.tick:
            continue
        print(tick, hex(aligned_vaddr), hex(aligned_pf_addr), PFSrc, offset, "Prefe", file=outf)

    elif len(x) == 8:  # bop trace
        id_, tick, type_str, old_addr, vaddr, offset, score, miss = x
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_old_addr = (old_addr >> 6) << 6
        if int(tick) < args.tick:
            continue

        print(tick, hex(aligned_old_addr), hex(aligned_vaddr), score, miss, offset, "Train", file=outf)

    elif len(x) == 12:  # memory access trace
        last_completed = completed
        id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, writenback, pf_source = x
        complete_delta = round((completed - last_completed)/cycle, 0)
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_paddr = (paddr >> 6) << 6
        print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                str(pf_source).ljust(3), complete_delta, "Acces", file=outf)

    else:
        print(x)
        raise


outf.close()

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_trigger_pc:
    print(hex(x[0]), x[1])
