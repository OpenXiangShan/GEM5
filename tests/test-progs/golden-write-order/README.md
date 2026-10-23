# Golden memory 写入顺序验证

`double-add` 验证两次减一后的值和 AMO 返回值之和；`add-swap`
覆盖不交换的 AMO；`add-store` 覆盖 AMO 与普通 store 混合。
后两者接受两种合法串行顺序，但返回值和最终 LR 必须对应同一种顺序。
邻接字节的 guard 用于检查 word 写入没有破坏其他字节。
`concurrent-add` 两个 hart 各执行 1000 次减一，校验最终值和所有返回值之和。
`zero-vl-store` 检查 VL=0 的向量 store 正常完成且不修改内存。

```sh
make -C tests/test-progs/golden-write-order \
  AM_HOME=/path/to/nexus-am BUILD_DIR=/tmp/golden-images
python3 tests/test-progs/golden-write-order/run.py \
  --images /tmp/golden-images --out /tmp/golden-diff \
  --ref-so /path/to/compatible-multi16g-reference \
  --param 'system.cpu[0].BankConflictCheck=False'
```

握手测试反复轮询可能触发独立的 bank 仲裁饥饿，验证 golden 功能时
可显式关闭该检查以隔离两个问题，或另行应用 PR #1084。
`concurrent-add` 已在保持 bank 检查开启时运行。
用独立 BUILD_DIR 和 `DELAY=<n>` 可调整两个 hart 的相对发出时机。

这些测试有 guest 的独立结果判定，但不是历史响应倒序的稳定最小复现，
也不是完整 RVWMO 验证。历史问题的精确复现使用 SPEC17
`mcf_rate_refrate_0`、版本 `0e05d634`、16GB 和兼容 multi16g REF：
PC `0xffffffff80152788`，地址 `0x47e951534`，tick `678519468`。
应同时比较修复前后的实际执行轨迹，并检查缓存原子更新时 golden
立即变成 8、7，迟到的响应没有再把它覆盖成 8。

当前写入观察接口覆盖 classic cache 和 AbstractMemory 的普通写入及 AMO。
失败 SC 不更新 golden；其他未触发该接口的内存模型保留原响应路径。
该修改只维护校验状态，不改变 cache 请求、响应调度或 AMO 的实际操作。
