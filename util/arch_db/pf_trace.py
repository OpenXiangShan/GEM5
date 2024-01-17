import sqlite3
import collections
from db_proc_args import args, db_path

print('Processing', db_path)
con = sqlite3.connect(db_path)
cur = con.cursor()
res = cur.execute('SELECT * FROM L1PFTrace')
cycle = 333
seen_addr = {}
recent_lines = []
show = args.show
if show.endswith('of_pc'):
    outf = open(f'{args.pc}_{show}_pf_trace.txt', 'w')
else:
    outf = open(f'{show}_pf_trace.txt', 'w')
recent_len = 20
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
observe_pc = int(args.pc, 16)
observe_last_addr = 0
trigger_pcs = {}
pf_source = 'x'
trace_count = 0

for x in res:
    id_, tick, pc, vaddr, pf_addr, PFSrc, site = x
    aligned_vaddr = (vaddr >> 6) << 6
    if int(tick) < args.tick:
        continue
    trigger_pcs[pc] = trigger_pcs.get(pc, 0) + 1

    if show.endswith('of_pc') and pc != observe_pc:
        continue

    # we can filter prefetcher type here
    # if PFSrc != 10:
    #     continue

    trace_count += 1
    deltas = []
    if args.show_global_delta:
        for i in range(recent_len):
            deltas.append(vaddr - recent_cold_misses[-1-i])
        recent_cold_misses.append(vaddr)
        deltas_str = str(deltas)
    else:
        deltas_str = ''
    print(tick, hex(pc), hex(vaddr), hex(aligned_vaddr), hex(pf_addr), str(PFSrc).ljust(5),
          (pf_addr - vaddr)//64, deltas_str, file=outf)

    if trace_count >= args.max_trace:
        break
outf.close()

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_trigger_pc:
    print(hex(x[0]), x[1])
