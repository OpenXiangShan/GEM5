import sqlite3
import collections
import argparse

parser = argparse.ArgumentParser()
# add a positional argument
parser.add_argument('tick', type=int, help='roi start tick')
args = parser.parse_args()

con = sqlite3.connect('/local/zhouyaoyang/exec-storage/u-bop-analysis/mem_trace.db')
# con = sqlite3.connect('../../util/warmup_scripts/zeusmp-tune/zeusmp_694700000000/mem_trace.db')
cur = con.cursor()
res = cur.execute('SELECT * FROM MemTrace')
cycle = 500
seen_addr = {}
recent_lines = []
show = 'unseen'
outf = open(f'./{show}_trace.txt', 'w')
recent_len = 10
recent_cold_misses = collections.deque(recent_len*[0], recent_len)
miss_pcs = {}
for x in res:
    id_, tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, writenback = x
    if int(tick) < args.tick:
        continue
    tlb_lat = (translated - issued)//cycle
    cache_lat = (completed - translated)//cycle
    aligned_addr = (vaddr >> 6) << 6

    # if aligned_addr in seen_addr and (cache_lat > 10):
    #     print('Warning: miss on seen addr')
    #     print(hex(pc), hex(aligned_addr), tlb_lat, cache_lat)
    #     raise
    if show == 'unseen':
        if aligned_addr not in seen_addr:
            global_stride = []
            for i in range(recent_len):
                global_stride.append(aligned_addr - recent_cold_misses[-1-i])
            print(id_, tick, hex(pc), hex(aligned_addr), tlb_lat, cache_lat, global_stride, file=outf)
            recent_cold_misses.append(aligned_addr)
    elif show == 'seen':
        if aligned_addr in seen_addr and (cache_lat > 10) and aligned_addr not in recent_lines:
            print(id_, tick, hex(pc), hex(aligned_addr), tlb_lat, cache_lat, file=outf)
        
        recent_lines.append(aligned_addr)
        if len(recent_lines) > 100:
            recent_lines.pop(0)
    else:
        raise
    
    if (cache_lat > 10) and aligned_addr not in recent_lines:
        if pc not in miss_pcs:
            miss_pcs[pc] = 1
        else:
            miss_pcs[pc] += 1
        
    seen_addr[aligned_addr] = True

outf.close()

top10_miss_pc = sorted(miss_pcs.items(), key=lambda x: x[1], reverse=True)[:10]

for x in top10_miss_pc:
    print(hex(x[0]), x[1])
