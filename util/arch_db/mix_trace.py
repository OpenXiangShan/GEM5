import sqlite3
import collections
import heapq
from db_proc_args import args, db_path

print('Processing', db_path)

con = sqlite3.connect(db_path)
cur = con.cursor()
bop_train_trace = cur.execute('SELECT * FROM BOPTrainTrace')

con2 = sqlite3.connect(db_path)
cur2 = con2.cursor()
pf_trace = cur2.execute('SELECT * FROM L1PFTrace')

con3 = sqlite3.connect(db_path)
cur3 = con3.cursor()
acc_trace = cur3.execute('SELECT * FROM MemTrace')

con4 = sqlite3.connect(db_path)
cur4 = con4.cursor()
evict_trace = cur4.execute('SELECT * FROM CacheEvictTrace')

cycle = 333
show = args.show

def gen_trace():
    trigger_pcs = {}
    completed = 0
    if show.endswith('of_pc'):
        outf = open(f'{args.pc}_{show}_bop_mix_trace.txt', 'w')
    else:
        outf = open(f'{show}_bop_mix_trace.txt', 'w')

    for count, x in enumerate(heapq.merge(bop_train_trace, pf_trace, acc_trace, evict_trace, key=lambda a: a[1])):
        if int(x[1]) < args.tick:
            continue
        if x[-1] == 'L1PFTrace':  # pf trace
            id_, tick, pc, vaddr, pf_addr, PFSrc, site = x
            aligned_vaddr = (vaddr >> 6) << 6
            aligned_pf_addr = (pf_addr >> 6) << 6
            offset = int(aligned_pf_addr - aligned_vaddr) // 64
            if PFSrc != 4:
                continue
            print(tick, hex(aligned_vaddr), hex(aligned_pf_addr), PFSrc, offset, "Prefe", file=outf)

        elif x[-1] == 'system.l2' or x[-1] == 'system.l3':  # l1 evict trace
            id_, tick, paddr, curCycle, level, site = x
            print(tick, hex(paddr), f'L{level}', "Evict", file=outf)

        elif x[-1] == 'CommitMemTrace':  # memory access trace
            last_completed = completed
            id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, wb, pf_source, site = x
            complete_delta = round((completed - last_completed)/cycle, 0)
            aligned_vaddr = (vaddr >> 6) << 6
            aligned_paddr = (paddr >> 6) << 6
            latency = int(completed - translated)
            if latency > 25:
                print(id_, issued, hex(pc).ljust(8), hex(vaddr).ljust(14), hex(aligned_paddr).ljust(14),
                        str(pf_source).ljust(3), complete_delta, latency, "Acces", file=outf)

        elif x[-1] == 'BOPTrain':
            id_, tick, old_addr, vaddr, offset, score, miss, site = x
            aligned_vaddr = (vaddr >> 6) << 6
            aligned_old_addr = (old_addr >> 6) << 6
            if int(tick) < args.tick:
                continue

            print(tick, hex(aligned_old_addr), hex(aligned_vaddr), score, miss, offset, "Train", file=outf)

        else:
            print(x)
            raise

    outf.close()

    top_trigger_pc = sorted(trigger_pcs.items(), key=lambda x: x[1], reverse=True)[:20]

    for x in top_trigger_pc:
        print(hex(x[0]), x[1])

class PfInfo:
    prefetched_addr = {}
    total_used = 0
    def __init__(self, blk_addr):
        self.pf_blk = blk_addr
        self.evict_to_l2 = False
        self.evict_to_l3 = False
        self.used = False
        PfInfo.prefetched_addr[blk_addr] = self

def analyze_pf_hit():
    prefetched_vaddrs = set()
    # Because there is not paddr info when pf triggers, we manually construction a tlb here
    tlb = {}
    count = 0
    global pf_trace
    global acc_trace
    for _, x in enumerate(heapq.merge(pf_trace, acc_trace, key=lambda a: a[1])):
        if int(x[1]) < args.tick:
            continue
        count += 1
        if x[-1] == 'L1PFTrace':  # pf trace
            id_, tick, pc, vaddr, pf_addr, PFSrc, site = x
            aligned_addr = (pf_addr >> 6) << 6
            prefetched_vaddrs.add(aligned_addr)

        elif x[-1] == 'CommitMemTrace':  # memory access trace
            id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, wb, pf_source, site = x
            aligned_addr = (vaddr >> 6) << 6
            aligned_paddr = (paddr >> 6) << 6
            if aligned_addr in prefetched_vaddrs:
                tlb[aligned_addr] = aligned_paddr

        if count % 1000000 == 0:
            print(count)

    count = 0

    # reset cursor
    pf_trace = cur2.execute('SELECT * FROM L1PFTrace')
    acc_trace = cur3.execute('SELECT * FROM MemTrace')
    evict_trace = cur4.execute('SELECT * FROM CacheEvictTrace')

    # go through the trace
    for _, x in enumerate(heapq.merge(evict_trace, acc_trace, pf_trace, key=lambda a: a[1])):
        if int(x[1]) < args.tick:
            continue
        count += 1
        if x[-1] == 'L1PFTrace':  # pf trace
            id_, tick, pc, vaddr, pf_addr, PFSrc, site = x
            aligned_addr = (pf_addr >> 6) << 6
            if PFSrc == 4:
                if aligned_addr in tlb:
                    aligned_paddr = tlb[aligned_addr]
                    if aligned_paddr not in PfInfo.prefetched_addr:
                        PfInfo(aligned_paddr)
                else:
                    PfInfo(aligned_addr)

        elif x[-1] == 'system.l2' or x[-1] == 'system.l3':  # l1 evict trace
            id_, tick, paddr, curCycle, level, site = x
            aligned_addr = (paddr >> 6) << 6
            if aligned_addr in PfInfo.prefetched_addr and not PfInfo.prefetched_addr[aligned_addr].used:
                if level == 2:
                    PfInfo.prefetched_addr[aligned_addr].evict_to_l2 = True
                elif level == 3:
                    PfInfo.prefetched_addr[aligned_addr].evict_to_l3 = True

        elif x[-1] == 'CommitMemTrace':  # memory access trace
            id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, wb, pf_source, site = x
            aligned_addr = (vaddr >> 6) << 6
            aligned_paddr = (paddr >> 6) << 6
            if aligned_paddr in PfInfo.prefetched_addr and not PfInfo.prefetched_addr[aligned_paddr].used:
                PfInfo.prefetched_addr[aligned_paddr].used = True
                PfInfo.total_used += 1

        if count % 1000000 == 0:
            print(len(PfInfo.prefetched_addr), PfInfo.total_used)

    # count ratio of evict to l2 and l3
    evict_to_l2_before_used = 0
    evict_to_l3_before_used = 0
    for x in PfInfo.prefetched_addr.values():
        if x.evict_to_l2:
            evict_to_l2_before_used += 1
        if x.evict_to_l3:
            evict_to_l3_before_used += 1
    total = len(PfInfo.prefetched_addr)
    print(f'Total: {total}, total used: {PfInfo.total_used},'
          f' evict to l2: {evict_to_l2_before_used},'
          f' evict to l3: {evict_to_l3_before_used}')
    print(f'Total used: {PfInfo.total_used / total * 100:.2f}%,'
          f' evict to l2: {evict_to_l2_before_used / total * 100:.2f}%,'
          f' evict to l3: {evict_to_l3_before_used / total * 100:.2f}%')

if args.dont_gen_trace:
    analyze_pf_hit()
else:
    gen_trace()
