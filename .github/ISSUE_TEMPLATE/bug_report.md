---
name: Bug report
about: Create a report to help us improve
title: ''
labels: ''
assignees: ''

---

**Describe the bug**

- What workload are you running? Baremetal workload, bbl+Linux+application or checkpoints?
- What is the expected behavior? What is the actual behavior?
- Can this issue be consistently reproduced? If so, what are the steps to reproduce? Can this issue be reproduced in latest xs-dev branch?
- Can you upload your image or checkpoint to somewhere to help us to reproduce the issue?

**To Reproduce**
Steps to reproduce the behavior:
- Build command: typically `scons build/RISCV/gem5.opt`
- Running command: typically
```
mkdir -p util/xs_scripts/test
cd util/xs_scripts/test
bash ../kmh_6wide.sh /path/to/checkpoint
```
- The link to your image or checkpoint

**Expected behavior**
A clear and concise description of what you expected to happen.

**Error log or Screenshots**

If applicable, plz provide you error log.

If applicable, add screenshots to help explain your problem.

**Necessary information on versions**
- XS-GEM5 version: [e.g. xs-dev branch or commit: deadbeaf]
- NEMU version used as reference design: [e.g. master branch or commid: deadbeaf]
- Checkpoint restorer version used: [e.g. main branch of OpenXiangShan/LibCheckpointAlpha, or commit: deadbeaf of xxx]

**Additional information**
If you generate checkpoints your self, you can optionally provide version information of following components:
- riscv-linux
- riscv-rootfs
    - initramfs.txt
- riscv-pk or OpenSBI
- GCPT restorer (checkpoint restorer), it might be NEMU in older versions or OpenXiangShan/LibCheckpointAlpha or OpenXiangShan/LibCheckpoint in newer versions

**Additional context**
Add any other context about the problem here.
