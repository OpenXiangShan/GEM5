import sqlite3
import collections
from db_proc_args import args, db_path

print('Processing', db_path)

con = sqlite3.connect(db_path)
cur = con.cursor()
res = cur.execute('SELECT * FROM MemTrace')
cycle = 333
seen_addr = {}
recent_lines = []
show = args.show

if args.output is not None:
    outfile_name = args.output
elif  show.endswith('of_pc'):
    outfile_name = f'{args.pc}_{show}_trace.txt'
else:
    outfile_name = f'{show}_trace.txt'

outf = open(outfile_name, 'w')

recent_len = 20
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
observe_pc = int(args.pc, 16)
observe_last_addr = 0
miss_pcs = {}
pf_source = 'x'
completed = 0
printed_header = False
trace_count = 0

for x in res:
    last_completed = completed
    id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, writenback, pf_source, site = x
    complete_delta = int(round((completed - last_completed)/cycle, 0))
    if int(tick) < args.tick:
        continue
    if show.endswith('of_pc') and pc != observe_pc:
        continue

    trace_count += 1
    tlb_lat = (translated - issued)//cycle
    cache_lat = (completed - translated)//cycle
    aligned_vaddr = (vaddr >> 6) << 6
    aligned_paddr = (paddr >> 6) << 6

    if show.startswith('unseen'):
        if not printed_header:
            print('id', 'issued', 'pc'.ljust(8), 'vaddr'.ljust(14), 'paddr'.ljust(14),
                  'pf_source'.ljust(3), 'tlb_lat'.ljust(4), 'cache_lat'.ljust(4), 'local_stride'.ljust(12),
                  'complete_delta', file=outf)
            printed_header = True

        if aligned_vaddr not in recent_lines:
            global_stride = []
            for i in range(recent_len):
                global_stride.append(vaddr - recent_cold_misses[-1-i])
            if pc == observe_pc:
                local_stride = vaddr - observe_last_addr
                observe_last_addr = vaddr
            else:
                local_stride = 0
            print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                  str(pf_source).ljust(3), str(tlb_lat).ljust(4),
                  str(cache_lat).ljust(4), str(local_stride).ljust(12), complete_delta, file=outf)

            recent_cold_misses.append(vaddr)
    elif show.startswith('seen'):
        if not printed_header:
            print('id', 'issued', 'pc'.ljust(8), 'vaddr'.ljust(14), 'paddr'.ljust(14),
                    'pf_source'.ljust(3), 'tlb_lat'.ljust(4), 'cache_lat'.ljust(4), file=outf)
            printed_header = True

        if aligned_vaddr in seen_addr and (cache_lat > 10) and aligned_vaddr not in recent_lines:
            print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                    str(pf_source).ljust(3), str(tlb_lat).ljust(4),
                    str(cache_lat).ljust(4), file=outf)

    elif show.startswith('all'):
        if not printed_header:
            print('id', 'issued', 'pc'.ljust(8), 'vaddr'.ljust(14), 'paddr'.ljust(14),
                  'pf_source'.ljust(3), 'tlb_lat'.ljust(4), 'cache_lat'.ljust(4), 'local_stride'.ljust(12),
                  'global_stride', file=outf)
            printed_header = True

        global_stride = []
        for i in range(recent_len):
            global_stride.append(vaddr - recent_cold_misses[-1-i])
        if pc == observe_pc:
            local_stride = vaddr - observe_last_addr
            observe_last_addr = vaddr
        else:
            local_stride = 0
        print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                str(pf_source).ljust(3), str(tlb_lat).ljust(4),
                str(cache_lat).ljust(4), str(local_stride).ljust(12), global_stride, file=outf)
        recent_cold_misses.append(vaddr)
    else:
        raise

    if (cache_lat > 10) and aligned_vaddr not in recent_lines:
        if pc not in miss_pcs:
            miss_pcs[pc] = max(cache_lat - 30, 0)
        else:
            miss_pcs[pc] += max(cache_lat - 30, 0)

    recent_lines.append(aligned_vaddr)
    if len(recent_lines) > 1024:
        recent_lines.pop(0)

    seen_addr[aligned_vaddr] = True

    if trace_count >= args.max_trace:
        break

top_miss_pc = sorted(miss_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

for x in top_miss_pc:
    print(hex(x[0]), x[1], file=outf)

outf.close()
