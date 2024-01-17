# Arch DB process script

This directory contains example scripts to analyze Arch DB traces.

## Performance results used as example

Example data can be downloaded from [here](https://smailnjueducn-my.sharepoint.com/personal/shinezyy_smail_nju_edu_cn/_layouts/15/onedrive.aspx?id=%2Fpersonal%2Fshinezyy%5Fsmail%5Fnju%5Fedu%5Fcn%2FDocuments%2Fperf%2Danalysis%2Dtutor%2D20231227&ga=1)

`New-gcc-12-o3-jemalloc-bwaves-max-weighted-point`, `bop-trace` and `sms-trace` are generated with [simple_gem5.sh](util/warmup_scripts/simple_gem5.sh),
with turning arch db enabled.
`sms-trace-rtl` is generated from XS' RTL simulation with arch DB enabled.

To generate `New-gcc-12-o3-jemalloc-bwaves-max-weighted-point`, set following flags to true in [fs.py](configs/example/fs.py)
and simulate with the max-weighted checkpoint of bwaves.

``` Python
        test_sys.arch_db.dump_l1_pf_trace = True
        test_sys.arch_db.dump_mem_trace = True
```

To generate `sms-trace`, set following flags to true in [fs.py](configs/example/fs.py)
and simulate with a GemsFDTD checkpoint (most of GemsFDTD checkpoints show trigger many SMS prefetches).

``` Python
        test_sys.arch_db.dump_l1_pf_trace = True
        test_sys.arch_db.dump_mem_trace = True
        test_sys.arch_db.dump_sms_train_trace = True
```

To generate `bop-trace`, set following flags to true in [fs.py](configs/example/fs.py)
and simulate with a mcf checkpoint (one of mcf's top-3 weighted checkpoint will trigger this case).

``` Python
        test_sys.arch_db.dump_l1_pf_trace = True
        test_sys.arch_db.dump_mem_trace = True
        test_sys.arch_db.dump_bop_train_trace = True
        test_sys.arch_db.dump_l2_evict_trace = True
        test_sys.arch_db.dump_l3_evict_trace = True
```


## Memory trace analysis

Show recently unseen trace for 10000 accesses
``` Bash
python3 mem_trace.py --db $tutor_top/New-gcc-12-o3-jemalloc-bwaves-max-weighted-point/mem_trace.db -M 10000
```
It prints trace to unseen_trace.txt. It also shows top missed PCs at the end of the txt.

To verify whether PC-based local prefetchers function well, we can pick top miss PC to show their unseen traces.
Ususally, we investigate unseen traces and full traces for spatical prefetchers and seen traces for temporal prefetchers.

Show full traces for a specific PC
``` Bash
python3 mem_trace.py --db $tutor_top/New-gcc-12-o3-jemalloc-bwaves-max-weighted-point/mem_trace.db --show all_of_pc --pc 0x10836 -M 1000
```
Its trace is quite regular, so we can use stream or stride prefetcher to prefetch it easily.
```
  10201 7643682 0x10836  0xae5350       0xc8f9e340     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  10594 7817841 0x10836  0xae5378       0xc8f9e340     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  10987 7927398 0x10836  0xae53a0       0xc8f9e380     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  11380 8038953 0x10836  0xae53c8       0xc8f9e3c0     3   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  11773 8531460 0x10836  0xae53f0       0xc8f9e3c0     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  12166 8702622 0x10836  0xae5418       0xc8f9e400     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  12559 8808516 0x10836  0xae5440       0xc8f9e440     3   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  12952 8917074 0x10836  0xae5468       0xc8f9e440     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  13368 9180144 0x10836  0xae5490       0xc8f9e480     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  13761 9291033 0x10836  0xae54b8       0xc8f9e480     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  14154 9495162 0x10836  0xae54e0       0xc8f9e4c0     0   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800]                                                                                                                            
  14547 9641349 0x10836  0xae5508       0xc8f9e500     3   1    1    40           [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800] 
```

## Prefetch triggerting trace

When a easy-to-prefetch pattern is not well prefetched, we can check whether prefetcher is triggered expectedly.
``` Bash
python3 pf_trace.py --db $tutor_top/New-gcc-12-o3-jemalloc-bwaves-max-weighted-point/mem_trace.db --show all_of_pc --pc 0x10836 -M 100
```

From the trace, we can see that 0x10836 is prefetched well.
At very beginning, it was prefetched by prefetch 3 (SMS's pattern prefetcher) and then switched to prefetch 1 (stream prefetcher).

```
10082241 0x10836 0xae5580 0xae5580 0xae5a80 3     20 
10082241 0x10836 0xae5580 0xae5580 0xae5ac0 3     21 
10082241 0x10836 0xae5580 0xae5580 0xae5b00 3     22 
11374947 0x10836 0xae56c0 0xae56c0 0xae5700 3     1  
11374947 0x10836 0xae56c0 0xae56c0 0xae5740 3     2  
11374947 0x10836 0xae56c0 0xae56c0 0xae5780 3     3  
11374947 0x10836 0xae56c0 0xae56c0 0xae57c0 3     4  
11374947 0x10836 0xae56c0 0xae56c0 0xae5800 3     5  
11374947 0x10836 0xae56c0 0xae56c0 0xae5840 3     6  
11374947 0x10836 0xae56c0 0xae56c0 0xae5880 3     7  
11374947 0x10836 0xae56c0 0xae56c0 0xae58c0 3     8  
11374947 0x10836 0xae56c0 0xae56c0 0xae5900 3     9  
11374947 0x10836 0xae56c0 0xae56c0 0xae5940 3     10 
12352635 0x10836 0xae5800 0xae5800 0xae5c00 1     16  
12352635 0x10836 0xae5800 0xae5800 0xae5c40 1     17  
12352635 0x10836 0xae5800 0xae5800 0xae5c80 1     18  
12352635 0x10836 0xae5800 0xae5800 0xae5cc0 1     19  
12352635 0x10836 0xae5800 0xae5800 0xae5d00 1     20  
12352635 0x10836 0xae5800 0xae5800 0xae5d40 1     21  
12352635 0x10836 0xae5800 0xae5800 0xae5d80 1     22  
12352635 0x10836 0xae5800 0xae5800 0xae5dc0 1     23  
12352635 0x10836 0xae5800 0xae5800 0xae5e00 1     24  
12352635 0x10836 0xae5800 0xae5800 0xae5e40 1     25  
12352635 0x10836 0xae5800 0xae5800 0xae5e80 1     26  
12352635 0x10836 0xae5800 0xae5800 0xae5ec0 1     27  
12352635 0x10836 0xae5800 0xae5800 0xae5f00 1     28  
12352635 0x10836 0xae5800 0xae5800 0xae5f40 1     29  
12352635 0x10836 0xae5800 0xae5800 0xae5f80 1     30  
```

## Mixed trace analysis for BOP prefetcher

Background of this trace: we found this checkpoint potentially trigger excessive BOP prefetches on RTL.
We show the show all traces after warmup:
``` Bash
# output file: unseen_bop_mix_trace.txt
python3 mix_trace.py --db $tutor_top/bop-trace/mem_trace.db --tick 11334320334
# This will take several minutes because we didn't limit the count of trace
```

From the stats file, we found the most popular offset is 24, by searching "24 Train" in the trace file.
We can found why 24 is scored as the best offset.
To our supprise, the case that add up 24'score is mostly false positive hit.
For example `11334955032 0x20183e7c40 0x2020ee8240 8 1 24 Train`.
This is because the tag bits is too short and index and tag are overlapped and too short.

## Mixed trace analysis for SMS prefetcher

Background of this trace: we found this checkpoint potentially trigger excessive SMS prefetches on RTL.
We show the show 300000 traces after warmup:
``` Bash
# for GEM5's trace, output file: unseen_sms_trace.txt
python3 sms_trace.py --db $tutor_top/sms-trace/mem_trace.db --tick 2401099830 -M 300000
# for Xiangshan's trace, output file: unseen_xs_sms_trace.txt
python3 xs_sms_trace.py --db $tutor_top/sms-trace-rtl/2023-12-19@13:38:09.db --tick 16487810 -M 300000
# This will take several minutes because XS's trace is large
```

We can see that 0x2d92a triggered 107786 prefetches in RTL.
To show the trace of 0x2d92a, we can use following command:
``` Bash
# output file: 0x2d92a_all_of_pc_xs_sms_trace.txt
python3 xs_sms_trace.py --db $tutor_top/sms-trace-rtl/2023-12-19@13:38:09.db --tick 16487810 -M 107786 --pc 0x2d92a --show all_of_pc
```
Following trace suggests that the same PC triggers multiple SMS pattern prefetches which contains redundant prefetches
and are possibly uselss less prefetches.
```
  1417 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b380
  1418 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b100
  1419 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b180
  1420 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b200
  1421 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b240
  1422 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b2c0
  1423 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b340
  1424 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b000
  1425 Prefe 0x2006b9b0f0 0x2d92a 0x2006b9b040
```

```
  1450 Prefe 0x2006b9b158 0x2d92a 0x2006b9b3c0
  1451 Prefe 0x2006b9b158 0x2d92a 0x2006b9b180
  1452 Prefe 0x2006b9b158 0x2d92a 0x2006b9b200
  1453 Prefe 0x2006b9b158 0x2d92a 0x2006b9b280
  1454 Prefe 0x2006b9b158 0x2d92a 0x2006b9b2c0
  1455 Prefe 0x2006b9b158 0x2d92a 0x2006b9b340
  1459 Prefe 0x2006b9b158 0x2d92a 0x2006b9b000
  1460 Prefe 0x2006b9b158 0x2d92a 0x2006b9b080
  1461 Prefe 0x2006b9b158 0x2d92a 0x2006b9b0c0
```