import sqlite3
import collections
import heapq
from db_proc_args import args, db_path


print('Processing', db_path)

# con = sqlite3.connect(db_path)
# cur = con.cursor()
# bop_trace = cur.execute('SELECT * FROM SMSTrainTrace')

con2 = sqlite3.connect(db_path)
cur2 = con2.cursor()
pf_trace = cur2.execute('SELECT * FROM L1PFTrace')

con3 = sqlite3.connect(db_path)
cur3 = con3.cursor()
acc_trace = cur3.execute('SELECT * FROM MemTrace')

cycle = 333
seen_addr = {}
recent_lines = []
show = args.show
# outf = open(osp.join('arch_db_res', f'{show}_trace.txt'), 'w')
if show.endswith('of_pc'):
    outf = open(f'{args.pc}_{show}_sms_trace.txt', 'w')
else:
    outf = open(f'{show}_sms_trace.txt', 'w')
recent_len = 20
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
observe_pc = int(args.pc, 16)
observe_last_addr = 0
trigger_pcs = {}
pf_source = 'x'
completed = 0

trace_count = 0

for count, x in enumerate(heapq.merge(pf_trace, acc_trace, key=lambda a: a[1])):
    if args.tick is not None and int(x[1]) < args.tick:
        continue

    if x[-1] == 'L1PFTrace':  # pf trace
        id_, tick, pc, vaddr, pf_addr, PFSrc, site = x
        if show.endswith('of_pc') and pc != observe_pc:
            continue
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_pf_addr = (pf_addr >> 6) << 6
        offset = int(aligned_pf_addr - aligned_vaddr) // 64
        trigger_pcs[pc] = trigger_pcs.get(pc, 0) + 1
        trigger_offset = int((aligned_vaddr // 64) % 16)
        print(tick, hex(vaddr), hex(aligned_pf_addr), PFSrc, trigger_offset, hex(pc), "Prefe", file=outf)

    elif x[-1] == 'CommitMemTrace':
        last_completed = completed
        id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, writenback, pf_source, site = x
        if show.endswith('of_pc') and pc != observe_pc:
            continue
        complete_delta = int(round((completed - last_completed)/cycle, 0))
        aligned_vaddr = (vaddr >> 6) << 6
        aligned_paddr = (paddr >> 6) << 6
        print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                str(pf_source).ljust(3), complete_delta, "Acces", file=outf)

    # else:
    #     assert len(x) == 8  # sms trace
    #     id_, tick, site_str, old_addr, vaddr, addr_distance, confi, miss = x
    #     aligned_vaddr = (vaddr >> 6) << 6
    #     aligned_old_addr = (old_addr >> 6) << 6
    #     trigger_offset = (aligned_old_addr / 64) % 16

    #     print(tick, hex(aligned_old_addr), hex(aligned_vaddr), confi, miss, trigger_offset, "Train", file=outf)

    trace_count += 1
    if trace_count >= args.max_trace:
        break

outf.close()

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_trigger_pc:
    print(hex(x[0]), x[1])
