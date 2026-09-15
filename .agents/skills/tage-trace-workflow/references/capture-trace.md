# 采集 TAGE trace

先核对已有 binary 的版本和表支持。RTL 命令在选定的 XiangShan checkout 执行，gem5 命令在 GEM5 根目录执行。
`--no-diff` 是下面 RTL trace 示例的设置，不代表功能正确性验证。

## RTL 运行

关键点：

- `--dump-select-db` 必须是空格分隔的精确表名
- 优先只打 `CondTrace_0..7`，需要更细再加 `microTageTrace`
- 尽量先用 `-I` 把窗口限制住

示例：

```bash
./build/emu \
  --no-diff \
  -I 200000 \
  -i /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
  --dump-db \
  --dump-select-db "CondTrace_0 CondTrace_1 CondTrace_2 CondTrace_3 CondTrace_4 CondTrace_5 CondTrace_6 CondTrace_7 microTageTrace" \
  > /tmp/coremark_tage.out 2>&1
```

对 `.zstd` GCPT slice，直接把路径传给 `-i` 即可。

## gem5 运行

`kmhv3.py` 需要 diff 环境变量。保留调用者已有设置，否则使用 CI 常见默认值：

```bash
export GCBV_REF_SO="${GCBV_REF_SO:-/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so}"
test -f "$GCBV_REF_SO"
```

示例：

```bash
./build/RISCV/gem5.opt \
  --outdir /tmp/debug/coremark_200k_basic \
  ./configs/example/kmhv3.py \
  -I 200000 \
  --generic-rv-cpt /nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
  --raw-cpt \
  --enable-bp-db tage basic
```

说明：

- `tage` 会生成 `TAGEMISSTRACE`
- `basic` 会额外生成 `BPTRACE`
- gem5 和 RTL 使用相同输入及指令上限，并核对 restore、warmup 和统计起止位置；相同 `-I` 本身不能证明窗口一致。
- `.zstd` slice 不加 `--raw-cpt`；正常单核 GCPT 使用嵌入式 restorer。
