# SE 模式 Matrix Smoke 实现说明

## 目标

这份实现的目标不是完整支持 AME，也不是直接推进到 FS/Linux。

当前目标很明确：

- 在 gem5 中补齐当前软件栈真正需要的最小 matrix 子集
- 让 gem5 **初步支持在 SE 模式下运行用户态 AME 程序**
- 先把功能跑通，作为后续 FS 工作前的 smoke 基线
- 把下面这些程序作为后续回归基线
  - `libc_mmap_smoke_xsai`
  - `precomp_rand_repro`
  - `gemm_precomp`
  - `hello_xsai` 作为更长链路的补充验证

## 当前范围

当前分支只做 gem5 侧的 SE smoke 支持。

包含：

- 当前 smoke 路径需要的最小 matrix 指令 decode
- `RiscvISA::ISA` 中的最小 matrix 架构状态
- matrix tile 配置
- matrix load/store helper
- `int8 x int8 -> int32 accumulate` 的最小功能模型
- `msyncreset` / `mrelease` / `macquire` 的最小 token 语义
- SE 模式下为了 syscall/trap 正确性而关闭 rename folding 的配置修复

不包含：

- 完整 AME 指令覆盖
- FS/raw Linux bring-up
- 周期准确的 matrix 执行时序模型
- matrix 单元和 cache hierarchy 的真实时序交互
- 比当前 smoke 程序更广的 matrix 指令族

## 当前实现思路

### 1. matrix 状态放在 `RiscvISA::ISA`

当前 SE smoke 走的是一个简单的功能模型，状态直接保存在
`RiscvISA::ISA` 里：

- tile 尺寸
  - `matrixTileM`
  - `matrixTileK`
  - `matrixTileN`
- matrix 数据缓冲
  - `matrixTileA`
  - `matrixTileB`
  - `matrixAcc`
- token 状态
  - `matrixTokens`

这对当前 smoke 用例已经足够，因为当前只关心功能正确性，不关心精细时序。
可以把它理解成：

- 当前先把 matrix 单元建成一个 gem5 内部的功能模型
- 先保证用户态程序能“算对、跑通”
- 暂时不建真正的硬件时序

### 2. matrix 内存访问走 translating proxy

`mlae8` / `mlbe8` / `mlce32` / `msce32` 当前使用：

- SE 下：`SETranslatingPortProxy`
- 兼容路径下：`TranslatingPortProxy`

这样实现简单，而且和当前 smoke 目标匹配。

这里的含义是：

- 当前 matrix load/store 主要是按“功能正确”的方式访问内存
- 目标是保证用户态程序看到正确结果
- 不是去模拟一个真实的 AME memory pipeline

因此当前实现里：

- 计算本身是功能模型
- 和 cache 的交互也主要是功能正确优先
- 没有给 AME 指令补单独的执行延迟
- 没有做 matrix 单元与 LSU/L2 的时序建模

### 3. token 语义是最小功能版

当前 token 模型是故意简化的：

- `msyncreset(tok)`：把 token 清零
- `mrelease(tok)`：token 加一
- `macquire(tok, target)`：要求 `token >= target`

这不是完整的异步完成模型，只是当前 smoke 所需的最小契约。

### 4. SE 模式关闭 rename folding

`configs/example/se.py` 里会关闭：

- `enableMoveElimination`
- `enableConstantFolding`

原因是：

SE 下 trap/syscall 对寄存器的写回绕过了正常的 renamed writeback 路径。
如果保留 rename-time folding/elimination，O3 里用户态可见的 architectural
寄存器更新可能会丢失。

这件事虽然不是 matrix 指令本身，但真实 smoke 程序依赖 libc/syscall，
所以它是 SE matrix smoke 能稳定跑起来的前提。

## 当前支持的指令子集

当前只实现了现有 smoke 程序真正用到的子集：

- 配置 / 状态
  - `msettilem`
  - `msettilek`
  - `msettilen`
  - `mzero1r`
- 内存
  - `mlae8`
  - `mlbe8`
  - `mlce32`
  - `msce32`
- 计算
  - `mmacc.w.b`
- 同步 / token
  - `msyncreset`
  - `mrelease`
  - `macquire`

## 和主线的关系

当前实现默认依赖最新 `xs-dev` 已经包含的两块修复：

- aligned-L2 foreign snoop 过滤
- classic-L2 的 DRRIP slice 参数初始化

这两块已经单独拆成 PR 并进入主线，所以当前 SE matrix smoke 分支不再重复携带。

## 当前验证结论

已经验证通过：

- `libc_mmap_smoke_xsai`
  - 通过
- `precomp_rand_repro`
  - 通过
  - `errors_shown=0`
- `gemm_precomp`
  - 通过
  - 8 个 precomputed case 全过

补充验证：

- `hello_xsai`
  - 能正常进入 userland
  - allocator 测试全部通过
  - 能进入 randomized matrix fuzz
  - 在扩大的 timeout 窗口内没有完整跑完，但在 timeout 前没有观察到 correctness failure

## 一条典型运行命令

```bash
cd GEM5
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-se-gemm-precomp \
  configs/example/se.py \
  -c firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector --no-pf
```

## 已知限制

- 当前还是 smoke-oriented 的功能模型，不是完整 AME 模型
- token 语义还是最小功能版，没有和真正异步完成路径绑定
- 当前主要是“功能支持”，还没有做 matrix 指令的时序模拟
- 当前计算和 cache 交互主要是功能正确、无延迟的形式
- `hello_xsai` 的 randomized fuzz 明显比核心 smoke 用例更耗时
- FS/Linux 支持应该作为后续独立工作推进

## 下一步建议

这个 SE smoke 支持进入主线后，建议：

1. 把 `libc_mmap_smoke_xsai`、`precomp_rand_repro`、`gemm_precomp`
   固化为回归基线
2. 把后续主要 bring-up 重心转到 FS
3. 只有在真实 workload 需要时，再继续扩展 matrix 指令覆盖和时序模型
