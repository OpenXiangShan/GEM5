# 构建 RTL trace binary

## RTL 编译

先固定 RTL checkout；显式路径优先，下面的个人 home 只作本机 fallback：

```bash
export XIANGSHAN_HOME="${XIANGSHAN_HOME:-/nfs/home/yanyue/workspace/xs-env/XiangShan}"
git -C "$XIANGSHAN_HOME" rev-parse HEAD
cd "$XIANGSHAN_HOME"
```

仅在现有 binary 缺失、版本不匹配或所需表未启用时构建。以下是配置示例，先核对目标 checkout 是否支持；按机器资源调整并发。

```bash
make emu \
  EMU_THREADS=8 \
  EMU_TRACE=fst \
  WITH_DRAMSIM3=1 \
  WITH_CONSTANTIN=1 \
  WITH_CHISELDB=1 \
  WITH_ROLLINGDB=1 \
  CONFIG=FrontendDebugConfig \
  -j64
```

编完先做两个 sanity check：

```bash
rg -n "FrontendDebugConfig" build/time.log
rg -n "CondTrace_0_write|BpuPredictionTrace_write|microTageTrace_write" build/chisel_db.cpp
```

如果 `CondTrace_0_write()` 仍然是空桩，就不要继续跑 trace。


优先增量构建；只有确认旧生成物或配置切换造成构建问题时才考虑 clean，并检查清理范围。
