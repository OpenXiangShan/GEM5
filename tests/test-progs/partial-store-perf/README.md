# Partial Store Performance Test

This AM bare-metal workload writes one byte in each cold 64-byte cacheline.
The odd line stride visits every line while avoiding a simple sequential
prefetch pattern. The timed region ends with a fence so its CSR cycle count
includes StoreBuffer drain and permission/data-read completion.

Run the automated baseline/partial comparison from the gem5 root:

```bash
python3 util/partial_store_perf.py \
  --am-home ../nexus-am \
  --gem5 build/RISCV/gem5.opt
```

The default 65,536 lines create a 4 MiB cold-store workload. Use
`--lines 524288 --rounds 2` for a 32 MiB working set larger than the default
LLC. `--lines` must be a power of two and `--stride` must be odd.

The report compares the workload's CSR-measured cycles, `StorePermReq`,
`ReadExReq`, partial-store DDR requests, and CPU-data bytes read from memory.
Both configurations execute the same binary; only the L1D partial-store
configuration flag differs.
