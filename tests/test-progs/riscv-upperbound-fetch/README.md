# Upper-bound instruction supply regression

`fetch.S` installs Sv39 mappings (or Sv48 with `--defsym SV48=1`) and checks:

- repeated calls/returns through an execute-only mapping;
- a noncanonical address whose low VPN aliases executable code;
- a readable, nonexecutable target;
- reuse of the trained indirect branch with a valid target, so a speculative
  NX fault must not survive its older branch redirect;
- a compressed return in the final halfword before an unmapped page;
- a 32-bit instruction whose second halfword is on an unmapped page.

The M-mode handler checks instruction page-fault cause, original instruction
PC, and failing-byte trap value. Failure loops until maxinsts; only success
executes the XiangShan m5-exit instruction. Keep difftest enabled.

Build from the repository root:

```sh
riscv64-linux-gnu-as -march=rv64gc -o /tmp/upper-fetch.o \
  tests/test-progs/riscv-upperbound-fetch/fetch.S
riscv64-linux-gnu-ld -Ttext=0x80000000 -o /tmp/upper-fetch.elf /tmp/upper-fetch.o
riscv64-linux-gnu-objcopy -O binary /tmp/upper-fetch.elf /tmp/upper-fetch.bin
```

Run using the CI-pinned normal-dedup REF, `--enable-mem-dedup`, `--raw-cpt`,
`--generic-rv-cpt=/tmp/upper-fetch.bin`, and `--maxinsts=20000`. Enable
`system.cpu[0].idealInstructionFetch=True` and `enableTwoFetch=True` through
`--param`; optionally add serial double prediction and the backend probe.
Check for normal exit and `because m5_exit instruction encountered` in the log.

The historical timing baseline eagerly translates its entire 66-byte fetch
request and can fault on an unneeded next page. Build with
`--defsym SKIP_BOUNDARY=1` when comparing only canonical-address and permission
checks against that baseline. This regression does not cover H-extension or
establish a performance score.
