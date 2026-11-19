# SMT LR 重放回归

`ordered-lr` 用两个 hart 的握手与 `fence rw,rw` 保证写入完成后才读取。
当 LR 在 load pipeline 结束后仍等待缓存响应，另一线程的重叠 store 可见
可能丢弃它的请求。回归要求 LR 被重新排入队列，正常结束且每轮读值正确。
`ordered-lw` 是普通 load 对照；`sc-invalidation` 验证握手强制的跨线程
store 使 SC 失败；`race` 检查同地址单调写入下的 LR 值范围和同地址读顺序。

这些是有独立 guest 判定的定向测试，不是完整 RVWMO 验证。
SC 测试不要求无争用的 SC 必然成功。

设置 `AM_HOME` 并预先构建 nexus-am 的 `riscv64-xs-dual` AM/klib 库：

```sh
make -C tests/test-progs/smt-lr-replay BUILD_DIR=/tmp/smt-lr-images
python3 tests/test-progs/smt-lr-replay/run.py \
  --images /tmp/smt-lr-images --out /tmp/smt-lr-no-diff
```

加 `--ref-so <compatible-multi16g-reference>` 可在相同测试中开启 difftest。
如 gem5 动态链接 DRAMsim3，需要在 `LD_LIBRARY_PATH` 中提供它的库路径。
可用 `--param 'system.cpu[0].commitWidth=1'` 改变提交交错。

只有进程退出码 0、guest 输出 `errors=0` 和正常 `m5_exit` 同时满足才通过。
指令/tick 限制退出、watchdog panic 和墙钟超时均计为失败。

默认握手测试执行一轮，足以复现 LR 重放丢失。压力测试可在新的
`BUILD_DIR` 下用 `HANDSHAKE_ROUNDS=500` 构建；反复轮询还可能触发
独立的跨线程 bank 仲裁饥饿，需要另行接入 PR #1084 的仲裁修复。
