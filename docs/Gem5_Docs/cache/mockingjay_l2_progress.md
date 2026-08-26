# Mockingjay L2 Progress

## 2026-08-25: Setup and Design

* Created clean worktree `/tmp/gem5-mockingjay-5361c12` on branch
  `codex/mockingjay-l2-5361c12`, based exactly on
  `5361c1248804755d285313f41dd73b7a299f7b48`.
* Preserved the original workspace and its unrelated untracked files.
* Read `shah2022.pdf` and the authors' public ChampSim reference implementation.
* Confirmed the integration point: one independent policy SimObject per
  `system.l2_wrappers[i].slices[j].inner_cache` in `kmhv3.py`.
* Confirmed the target checkpoint exists:
  `/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint/omnetpp/6881/_6881_0.962556_memory_.zstd`.
* Confirmed the requested baseline run 32391965338 completed on the same base
  SHA and includes `performance-score-gcc15-spec06-1.0c/score.txt`.

## 2026-08-26: Final Local Validation

* Review checkpoint is available on local branch `codex/mockingjay-l2-5361c12`
  at `c2dbe9837b`; the implementation, bookkeeping fix, and admitted-fill
  regression coverage are separate commits for review.

* Corrected the RDP index to use the low bits of the PC/state CRC hash.
  The previous top-bit extraction collapsed ordinary RV64 PCs into entry zero,
  so all performance observations from that version are invalidated.
* Restricted direct policy bypass to a clean, single-target `ReadSharedReq`
  MSHR with no pending downgrade or invalidation. This preserves the normal
  fill path's coherence-state normalization for dirty lower responders and
  concurrent read snoops.
* Rebuilt the optimized binary and both focused test targets serially. This
  checkout has unrelated generated-header races under concurrent SCons
  actions.
* The policy GTest passed all 10 tests:

  `build/RISCV/mem/cache/replacement_policies/mockingjay_l2_rp.test.opt`

* The cache timing regression passed all five cases:

  `ReadCleanReqDirtyResponderFills`,
  `ReadSharedReqBypassesWithoutAllocating`,
  `ReadSharedReqAdmittedCleanFillAllocates`,
  `ReadSharedReqDirtyResponderFills`, and
  `ReadSharedReqPendingDowngradeFills`.

  The last case injects a concurrent `ReadSharedReq` snoop between issue and
  response, proves that the MSHR enters `postDowngrade`, and verifies that the
  response takes the normal allocating path.
  The admitted-fill case verifies the complementary path: a real victim is
  selected, the response allocates, and the line remains resident.
* The final-binary checkpoint smoke used the checkpoint-compatible reference
  and a local DDR4 fallback:

  `GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so ./build/RISCV/gem5.opt -d /tmp/mockingjay-l2-omnetpp-6881-final-b53bhi ./configs/example/kmhv3.py --mem-type=DDR4_2400_8x8 -I 1000000 --generic-rv-cpt=/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260604/checkpoint/omnetpp/6881/_6881_0.962556_memory_.zstd`

  It restored the embedded checkpoint restorer, entered real simulation, and
  completed at `simInsts=1000007` with
  `system.cpu.committedInsts=1000007`.
* Its generated `config.ini` verified four independent L2 policies, each with
  `type=MockingjayL2RP`, `num_sets=1024`, `num_ways=8`, `block_bits=6`,
  `slice_bits=2`, `sampled_sets=8`, `sampled_tag_bits=12`, and
  `rdp_entries=512`. Every slice recorded sampled-history, RDP, promotion,
  insertion, and aging activity. This short final-code smoke recorded zero
  bypasses, so it is functional/configuration evidence only.
* The default interpreter reference
  (`riscv64-nemu-interpreter-so`) fails at the checkpoint's initial
  `mstateen0` CSR instruction (`sn:163`) before useful execution. The smoke
  therefore uses `riscv64-nemu-notama-tvalref-so`. Local DDR4 timing is not
  comparable performance evidence for the CI DRAMsim3 configuration.
* `git diff --check` and `python3 -m py_compile configs/example/kmhv3.py` pass
  on the final branch.

## Controlled CI A/B Contract (Not Dispatched)

* Baseline run: `32391965338`, valid job `96499960567`, base SHA
  `5361c1248804755d285313f41dd73b7a299f7b48`.
* Baseline archive:
  `/nfs/home/share/gem5_ci/performance_data/gcc15-spec06-1.0c/20260821_003117_5361c12488_kmhv3_run102`
  with score `20.612401866596542`.
* Proposed run must use this branch's full 40-character SHA, `kmhv3.py`,
  `gcc15-spec06-1.0c`, the complete integer slice set, blank
  `distributed_servers` (CI parallel path), and CI DRAMsim3. Archive
  `config.ini`, `score.txt`, and the manifest before making any performance
  claim.
* No remote dispatch has been performed in this turn.
