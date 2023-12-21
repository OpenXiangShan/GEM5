import sqlite3
import collections
import heapq
from db_proc_args import args, db_path


print('Processing', db_path)

con = sqlite3.connect(db_path)
cur = con.cursor()
sms_trace = cur.execute('SELECT * FROM SMSTrainTraceTable0')

con2 = sqlite3.connect(db_path)
cur2 = con2.cursor()
pf_trace = cur2.execute('SELECT * FROM SMSPFTrace0')

con3 = sqlite3.connect(db_path)
cur3 = con3.cursor()
acc_trace = cur3.execute('SELECT * FROM LoadDebugTable0')

cycle = 333
seen_addr = {}
recent_lines = []
show = args.show
# outf = open(osp.join('arch_db_res', f'{show}_trace.txt'), 'w')
if show.endswith('of_pc'):
    outf = open(f'{args.pc}_{show}_xs_sms_trace.txt', 'w')
else:
    outf = open(f'{show}_xs_sms_trace.txt', 'w')
recent_len = 20
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
observe_pc = int(args.pc, 16)
observe_last_addr = 0
trigger_pcs = {}
miss_pcs = {}
pf_source = 'x'
completed = 0

trace_count = 0

for count, x in enumerate(heapq.merge(sms_trace, pf_trace, acc_trace, key=lambda a: a[-2])):
    site = x[-1]
    tick = int(x[-2])
    if tick < args.tick:
        continue


    if site == 'SMSTrainTraceTable':
        id_, miss, score, offset, cur_addr, old_addr, type_, tick, site = x
        print('Train', hex(old_addr), hex(cur_addr), file=outf)
    elif site == 'SMSPFTrace':
        id_, pf_source, pf_vaddr, trigger_addr, trigger_pc, tick, site = x
        if show.endswith('of_pc') and trigger_pc != observe_pc:
            continue
        trace_count += 1
        print('Prefe', hex(trigger_addr), hex(trigger_pc), hex(pf_vaddr), file=outf)
        trigger_pcs[trigger_pc] = trigger_pcs.get(trigger_pc, 0) + 1
    else:
        id_, exec_lat, trans_lat, is_miss, wb_tick, commit_tick, tranlated_tick, issued_tick, \
            paddr, vaddr, pc, is_load, tick, site = x
        lat = wb_tick - issued_tick
        aligned_vaddr = (vaddr >> 6) << 6
        if lat > 40:
            miss_pcs[pc] = miss_pcs.get(pc, 0) + (lat-30)
        print('Acces', hex(aligned_vaddr), hex(pc), tick, lat, file=outf)

    if trace_count >= args.max_trace:
        break

top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]
top_miss_pc = sorted(miss_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

print('Final tick:', tick, file=outf)

print('Top trigger PCs:', file=outf)
for x in top_trigger_pc:
    print(hex(x[0]), x[1], file=outf)

print('Top miss PCs:', file=outf)
for x in top_miss_pc:
    print(hex(x[0]), x[1], file=outf)

outf.close()