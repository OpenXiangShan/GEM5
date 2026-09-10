# GEM5 NEMU Reference

## 版本与构建配置

`lock.json` 是唯一源码版本入口。普通单核、H、SMT/多核都使用同一个
NEMU commit，依赖库也固定 commit；不要求将 NEMU 加入 GEM5 submodule。

| 变体 | 用途 | memdedup | RVV agnostic | Multicore difftest |
| --- | --- | --- | --- | --- |
| `normal` | 普通单核、RVV、solver、PGO、Quick Check | 关闭 | 开启 | 关闭 |
| `normal-dedup` | 普通 SPEC06/17/26、RVV 性能切片 | 开启 | 开启 | 关闭 |
| `h` | H checkpoint / H 功能测试 | 关闭 | 关闭 | 关闭 |
| `multi` | SMT、真正多核 | 开启 | 开启 | 开启 |

`normal` 和 `normal-dedup` 只在内存分配方式上不同，不是两条源码版本线。
SPEC 年份不决定 REF：普通单核统一选择 normal 系列，H 和多核按运行方式选择。
普通配置支持现有 RVA23 workload 所需扩展，不等同于完整 RVA23 合规认证。

公共配置固定 RVV、RVH、VCSR、FCSR 布局，寄存器区为 1376 字节，并支持 Sv48。
普通单核通过 `scalar.config` 保留 Smstateen、Zacas 等此前 scalar REF 的扩展；
H/多核不继承这些扩展增量，例如 H 的 SSTC 必须与 GEM5 H 执行路径匹配。
`TVAL_EX_II`、`GUIDED_TVAL` 均开启。普通单核保留 SDTRIG 执行支持，但不将
SDTRIG CSR 加入 regcpy 布局，因为 GEM5 尚未提供这些同步字段。

GEM5 正式同步并检查 `fcsr = (frm << 5) | fflags`。NEMU REF 不再兼容
1368 字节的无 FCSR 布局，加载时直接报错；Spike 后端不在本次 ABI 迁移范围内。
大小检查防止错误复制长度，不是完整的 ABI 协商协议，因此仍须使用配套配置。

## 可复现构建

在 GEM5 根目录运行：

```bash
python3 util/nemu_ref/build.py \
  --source /path/to/NEMU \
  --output /tmp/nemu-release/d30fff1ece9e-gem5-r3 \
  --jobs 8
```

`--source` 中需要已有 lock 指定的 NEMU 和 resource 依赖的 git 对象。
脚本用 `git archive` 导出固定版本，不读取工作区修改、不修改 NEMU checkout，
也不自动从网络取得未锁定依赖。需要 GCC/G++、make、Kconfig 构建依赖、zlib/zstd。
可重复使用 `--variant normal --variant normal-dedup` 限定构建范围。

每个变体目录包含 `.so`、实际 `.config`、`autoconf.h`、`manifest.json`、
`build.log` 和隔离的 `source/`。构建先检查 Kconfig 是否保留了要求的配置，
再编译并检查实际导出的 `DIFFTEST_REG_SIZE`。已存在的变体目录拒绝覆盖。
这里保证源码、依赖和配置可追溯；编译器和构建路径仍可能改变二进制哈希，
不宣称跨环境逐字节一致。manifest 记录的是实际发布产物的 SHA。

发布目录约定：

```text
/nfs/home/share/gem5_ci/ref/releases/<release>/<variant>/
    riscv64-nemu-interpreter-so
    riscv64-nemu-interpreter-so.config
    riscv64-nemu-interpreter-so.autoconf.h
    manifest.json
```

先在新目录完成验证，再发布上述四个文件；不要覆盖历史文件，也不要建立
随意移动的全局 `latest`。`resolve.py` 校验源码版本、依赖版本、配置片段和
产物校验和后输出路径。修改源码或配置时必须变更 release 名称。

```bash
python3 util/nemu_ref/resolve.py normal
python3 util/nemu_ref/resolve.py normal-dedup --root /tmp/nemu-release
```

本地普通单核运行可将 `GCBV_REF_SO` 设置为 `resolve.py normal` 的输出。
旧的 shell 环境变量不会被自动覆盖；指向旧 ABI 的变量须显式更新。

## memdedup 的边界

单核使用 `normal-dedup` 时必须加 `--enable-mem-dedup`；使用 `normal`
时不要加。GEM5 会检查 REF 确实接收了 COW backing memory，不能仅根据
`difftest_get_backed_memory` 符号存在就判断支持，因为不支持时 NEMU 也导出空实现。
SMT/多核保留已有的自动开启行为。

普通性能 workflow 在未指定 `gcbv_ref_so` 时选 `normal-dedup` 并附加开关。
指定自定义 REF 后不会自动附加开关，自定义 dedup REF 须在 `extra_args` 中显式开启。
H 和 SMT profiles 使用专用 REF；PGO 训练始终使用单核 `normal`。
性能归档记录选定 REF 路径、SHA 和存在时的构建 manifest。

不在 GEM5 全局默认开启：raw image 大于等于模拟 RAM 时仍会进入
`DedupMemory::initRootFromExistingFile()` 的未实现路径；H、设备及其他运行方式
也不能仅由普通 SPEC 切片结果推断兼容。默认普通性能切片不走该 raw-image 路径。

## 升级与回归

每次升级至少检查：FCSR/布局错误拒绝、memdedup 错配拒绝、CoreMark、RVV
小测试、SPEC06/17/26 checkpoint、H checkpoint、SMT 双线程和 Ruby/CHI 真多核。
memdedup A/B 固定 GEM5 二进制、源码配置、checkpoint、指令数，并比较模拟统计。
内存评估需计入 memfd 留存页面，不能只比较 RSS；短测收益不能代表所有 workload。

Quick Check 的历史 base 分支若尚未支持 FCSR，仍使用它原有的 REF。
这是历史 GEM5 配套测试，不是当前版本继续维护旧 NEMU 分支。
当前 base 合入本次支持后会自动使用 base 自己的 lock。

NEMU 上游配置 PR 可基于这些配置和回归结果单独整理；这里不携带 NEMU 源码补丁，
也不把配置发布与 GEM5 的 FCSR/内存接口修改混成不可追溯的裸 `.so` 替换。
