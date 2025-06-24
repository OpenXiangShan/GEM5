# 香山对齐测试配置手册

## checkpoint路径
小机房：

/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_archive/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/

大机房：

/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/

## RTL 平台配置
### 起始提交点
XiangShan: d78a17c1d883132bf47d00d463dc9817c6a2dd0b

### Prefetch Off & No L3 配置
#### 打 patch 关闭 L1 prefetcher，使用 fake L3
```diff
diff --git a/src/main/scala/system/SoC.scala b/src/main/scala/system/SoC.scala
index 68cd14377..ab63d6074 100644
--- a/src/main/scala/system/SoC.scala
+++ b/src/main/scala/system/SoC.scala
@@ -32,7 +32,7 @@ import huancun._
 import top.BusPerfMonitor
 import utility.{ReqSourceKey, TLClientsMerger, TLEdgeBuffer, TLLogger}
 import xiangshan.backend.fu.{MemoryRange, PMAConfigEntry, PMAConst}
-import xiangshan.{DebugOptionsKey, PMParameKey, XSTileKey}
+import xiangshan.{DebugOptionsKey, FakeL3, PMParameKey, XSTileKey}
 import coupledL2.{EnableCHI, L2Param}
 import coupledL2.tl2chi.CHIIssue
 import openLLC.OpenLLCParam
@@ -381,7 +381,7 @@ class MemMisc()(implicit p: Parameters) extends BaseSoC
   }
 
   if(soc.L3CacheParamsOpt.isEmpty){
-    l3_out :*= l3_in
+    l3_out := FakeL3() :*= l3_in
   }
 
   if (!enableCHI) {
diff --git a/src/main/scala/top/Configs.scala b/src/main/scala/top/Configs.scala
index 22aefc26e..525a4b241 100644
--- a/src/main/scala/top/Configs.scala
+++ b/src/main/scala/top/Configs.scala
@@ -347,8 +347,8 @@ case class L3CacheConfig(size: String, ways: Int = 8, inclusive: Boolean = true,
       t.L2NBanks * t.L2CacheParamsOpt.map(_.toCacheParams.capacity).getOrElse(0)
     }.sum
     up(SoCParamsKey).copy(
-      L3NBanks = banks,
-      L3CacheParamsOpt = Option.when(!up(EnableCHI))(HCCacheParameters(
+      L3NBanks = 1/*banks*/,
+      L3CacheParamsOpt = None /* Option.when(!up(EnableCHI))(HCCacheParameters(
         name = "L3",
         level = 3,
         ways = ways,
@@ -371,7 +371,7 @@ case class L3CacheConfig(size: String, ways: Int = 8, inclusive: Boolean = true,
         simulation = !site(DebugOptionsKey).FPGAPlatform,
         prefetch = Some(huancun.prefetch.L3PrefetchReceiverParams()),
         tpmeta = Some(huancun.prefetch.DefaultTPmetaParameters())
-      )),
+      )) */,
       OpenLLCParamsOpt = Option.when(up(EnableCHI))(OpenLLCParam(
         name = "LLC",
         ways = ways,
@@ -442,7 +442,7 @@ class FuzzConfig(dummy: Int = 0) extends Config(
 
 class DefaultConfig(n: Int = 1) extends Config(
   L3CacheConfig("16MB", inclusive = false, banks = 4, ways = 16)
-    ++ L2CacheConfig("1MB", inclusive = true, banks = 4)
+    ++ L2CacheConfig("1MB", inclusive = true, banks = 4, tp = false)
     ++ WithNKBL1D(64, ways = 4)
     ++ new BaseConfig(n)
 )
diff --git a/src/main/scala/xiangshan/FakeL3.scala b/src/main/scala/xiangshan/FakeL3.scala
new file mode 100644
index 000000000..35be7b90d
--- /dev/null
+++ b/src/main/scala/xiangshan/FakeL3.scala
@@ -0,0 +1,69 @@
+/***************************************************************************************
+ * Copyright (c) 2020-2021 Institute of Computing Technology, Chinese Academy of Sciences
+ * Copyright (c) 2020-2021 Peng Cheng Laboratory
+ *
+ * XiangShan is licensed under Mulan PSL v2.
+ * You can use this software according to the terms and conditions of the Mulan PSL v2.
+ * You may obtain a copy of Mulan PSL v2 at:
+ *          http://license.coscl.org.cn/MulanPSL2
+ *
+ * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
+ * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
+ * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
+ *
+ * See the Mulan PSL v2 for more details.
+ ***************************************************************************************/
+
+package xiangshan
+
+import chisel3._
+import chisel3.util._
+import org.chipsalliance.cde.config._
+import chisel3.util.{Valid, ValidIO}
+import freechips.rocketchip.diplomacy._
+import freechips.rocketchip.interrupts._
+import freechips.rocketchip.tile.{BusErrorUnit, BusErrorUnitParams, BusErrors, MaxHartIdBits}
+import freechips.rocketchip.tilelink._
+import coupledL2.{L2ParamKey, EnableCHI}
+import coupledL2.tl2tl.TL2TLCoupledL2
+import coupledL2.tl2chi.{TL2CHICoupledL2, PortIO, CHIIssue}
+import huancun.BankBitsKey
+import system.HasSoCParameter
+import top.BusPerfMonitor
+import utility._
+
+
+class FakeL3()(implicit p: Parameters) extends TLClientsMerger(debug = true) {
+  override val node = TLAdapterNode(
+    clientFn = s => {
+
+      println("TLClientsMerger: Merging clients:")
+      for (c <- s.masters) {
+        println(c)
+      }
+
+      val sourceIds = s.masters.map(_.sourceId)
+      val minId = sourceIds.map(_.start).min
+      val maxId = sourceIds.map(_.end).max
+      val merged = s.v1copy(
+        clients = Seq(s.masters.find(_.name == "L2").get.v1copy(
+          sourceId = IdRange(minId, maxId),
+          visibility = Seq(AddressSet(0x0, ~0x0))
+        ))
+      )
+      println("Merged params:")
+      println(merged.masters)
+
+      merged
+    }
+  )
+
+
+}
+
+object FakeL3 {
+  def apply()(implicit p: Parameters) = {
+    val fakeL3 = LazyModule(new FakeL3)
+    fakeL3.node
+  }
+}
\ No newline at end of file
diff --git a/src/main/scala/xiangshan/mem/MemBlock.scala b/src/main/scala/xiangshan/mem/MemBlock.scala
index 2784c2ba7..320053a2e 100644
--- a/src/main/scala/xiangshan/mem/MemBlock.scala
+++ b/src/main/scala/xiangshan/mem/MemBlock.scala
@@ -426,7 +426,8 @@ class MemBlockInlinedImp(outer: MemBlockInlined) extends LazyModuleImp(outer)
   val l1PrefetcherOpt: Option[BasePrefecher] = coreParams.prefetcher.map {
     case _ =>
       val l1Prefetcher = Module(new L1Prefetcher())
-      l1Prefetcher.io.enable := Constantin.createRecord(s"enableL1StreamPrefetcher$hartId", initValue = true)
+      // l1Prefetcher.io.enable := Constantin.createRecord(s"enableL1StreamPrefetcher$hartId", initValue = true)
+      l1Prefetcher.io.enable := Constantin.createRecord(s"enableL1StreamPrefetcher$hartId", initValue = false)
       l1Prefetcher.pf_ctrl <> dcache.io.pf_ctrl
       l1Prefetcher.l2PfqBusy := io.l2PfqBusy
 
```

#### 打 patch 关闭 L2 prefetcher
```diff
diff --git a/src/main/scala/coupledL2/prefetch/Prefetcher.scala b/src/main/scala/coupledL2/prefetch/Prefetcher.scala
index 2aa3a53..5a83dc2 100644
--- a/src/main/scala/coupledL2/prefetch/Prefetcher.scala
+++ b/src/main/scala/coupledL2/prefetch/Prefetcher.scala
@@ -344,11 +344,10 @@ class Prefetcher(implicit p: Parameters) extends PrefetchModule {
 
   val pftQueue = Module(new PrefetchQueue)
   val pipe = Module(new Pipeline(io.req.bits.cloneType, 1))
-
-  pftQueue.io.enq.valid :=
-    (if (hasReceiver)     pfRcv.get.io.req.valid                         else false.B) ||
-    (if (hasBOP)          vbop.get.io.req.valid || pbop.get.io.req.valid else false.B) ||
-    (if (hasTPPrefetcher) tp.get.io.req.valid                            else false.B)
+  pftQueue.io.enq.valid := false.B
+  //   (if (hasReceiver)     pfRcv.get.io.req.valid                         else false.B) ||^M
+  //   (if (hasBOP)          vbop.get.io.req.valid || pbop.get.io.req.valid else false.B) ||^M
+  //   (if (hasTPPrefetcher) tp.get.io.req.valid                            else false.B)^M
   pftQueue.io.enq.bits := ParallelPriorityMux(Seq(
     if (hasReceiver)     pfRcv.get.io.req.valid -> pfRcv.get.io.req.bits else false.B -> 0.U.asTypeOf(io.req.bits),
     if (hasBOP)          vbop.get.io.req.valid -> vbop.get.io.req.bits   else false.B -> 0.U.asTypeOf(io.req.bits),
```



## 编译 Trace 、ChiselDB
```shell
make emu -j128 EMU_THREADS=16 EMU_TRACE=1 \
WITH_DRAMSIM3=1 WITH_CONSTANTIN=1 WITH_CHISELDB=1 \
PGO_WORKLOAD=$NOOP_HOME/ready-to-run/coremark-2-iteration.bin \
PGO_EMU_ARGS="--diff $NOOP_HOME/ready-to-run/riscv64-nemu-interpreter-so" \
LLVM_PROFDATA=llvm-profdata
```

## 运行
--dump-select-db中"lifetime"的名称与实际表名"LifeTimeCommitTrace"不一致。

```shell
# 打波形
$NOOP_HOME/build/emu \
--diff $NOOP_HOME/ready-to-run/riscv64-nemu-interpreter-so \
-W 20000000 -I 40000000 \
-i <workload> \
--dump-wave  -b <开始时钟> -e <结束时钟> --wave-path <波形文件存放地址> \
--dump-db --dump-select-db "lifetime" --db-path <db数据库文件路径> \
--dramsim3-outdir <dramsim3输出文件夹> \
> simulator_out_20m_20m.txt 2> simulator_err_20m_20m.txt

# 不打波形
$NOOP_HOME/build/emu \
--enable-fork \
--diff $NOOP_HOME/ready-to-run/riscv64-nemu-interpreter-so \
-W 20000000 -I 40000000 \
-i <workload> \
--dump-db --dump-select-db "lifetime" --db-path <db数据库文件路径> \
--dramsim3-outdir <dramsim3输出文件夹> \
> simulator_out_20m_20m.txt 2> simulator_err_20m_20m.txt
```

## Gem5 平台配置
## 打patch修正latency，开启lifetime数据库
```diff
diff --git a/configs/example/xiangshan.py b/configs/example/xiangshan.py
index a3d1a88486..816b514257 100644
--- a/configs/example/xiangshan.py
+++ b/configs/example/xiangshan.py
@@ -238,7 +238,7 @@ def build_test_system(np, args):
         test_sys.arch_db.dump_l1_miss_trace = False
         test_sys.arch_db.dump_bop_train_trace = False
         test_sys.arch_db.dump_sms_train_trace = False
-        test_sys.arch_db.dump_lifetime = False
+        test_sys.arch_db.dump_lifetime = True
         test_sys.arch_db.table_cmds = [
             "CREATE TABLE L1MissTrace(" \
             "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
```

## 运行
在运行时添加 --no-pf --no-l3cache

```shell
# Prefetch Off & No L3 配置
gem5.opt \
--redirect-stdout --redirect-stderr \
configs/example/kmh.py \
--arch-db-fromstart=True \      # arch-db从第一拍开始记录，False则从warmup后才开始记录
--generic-rv-cpt=<checkpoint>\
--no-pf --no-l3cache \
--enable-arch-db \
--arch-db-file=m5out/test.db 

# Prefetch On & L3
gem5.opt \
--redirect-stdout --redirect-stderr \
configs/example/kmh.py \
--arch-db-fromstart=True \      # arch-db从第一拍开始记录，False则从warmup后才开始记录
--generic-rv-cpt=<checkpoint>\
--enable-arch-db \
--arch-db-file=m5out/test.db 
```

