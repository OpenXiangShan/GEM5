# Flush 后 vset 的 IBuffer 旁路规则

## 背景

发生流水线 flush 后，Fetch 会丢弃错误路径上的指令和取指状态，并从新的正确地址重新开始取指。当前模型中的 `fetchQueue[tid]` 承担 IBuffer 的作用：Fetch 将指令放入该队列，然后 `sendInstructionsToDecode()` 在同一个 Fetch 周期内把队列内容写入 `toDecode`，形成 Fetch 到 Decode 的直接旁路。

对于 flush 后重新取到的第一批指令，如果其中包含 `vsetvli`、`vsetivli` 或 `vsetvl`，这批指令不能沿用直接旁路路径。它们必须先完整地写入 IBuffer，下一周期再由 IBuffer 送往 Decode。

## 时序

不含 vset 的 flush 恢复路径保持原有行为：

```text
第 N 周期     Fetch
第 N+1 周期   Decode
```

如果 flush 后的首批指令包含 vset，则整个批次共享这一额外的前端传递周期：

```text
第 N 周期     Fetch -> IBuffer
第 N+1 周期   IBuffer
第 N+2 周期   Decode
```

这里“Fetch 到 Decode 延长 1 个周期”描述的是前端传递延迟增加，并不表示 vset 指令自身的执行延迟增加。由于 Decode 接收的是一个取指批次，同一批次中的其他指令也会一起受到这一个周期的影响。

## 实现

- `Fetch::doSquash()` 清空对应线程的 `fetchQueue` 后，设置一次性的 `deferVsetvlDecode[tid]` 标志。
- Fetch 在 flush 后首次形成非空 fetch queue 时记录该批次的长度；`Fetch::sendInstructionsToDecode()` 只扫描这段首批指令是否包含 `staticInst->isVectorConfig()`，避免后续批次被误计入。
- 如果不含 vset，标志立即清除，指令继续使用正常旁路。
- 如果包含 vset，标志立即清除，但本周期不填充 `toDecode`，整批指令留在 IBuffer 中；下一周期恢复正常发送。
- 标志按线程维护，并在启动、状态清理和重新初始化时复位，避免一个线程的 flush 恢复延迟影响其他线程。

该规则只改变 flush 后首批指令的 Fetch→Decode 传递路径，不改变 vset 的解码、执行、提交或 `waitForVsetvl` 语义。

## 验证要点

使用 `Fetch` 调试输出或流水线 trace 检查以下两种情况：

1. flush 后首批不含 vset：指令仍按原有旁路时序到达 Decode。
2. flush 后首批含 vset：日志出现 `Deferring post-squash vector-config batch`，该批次在一个周期内只停留于 IBuffer，随后整体到达 Decode。
