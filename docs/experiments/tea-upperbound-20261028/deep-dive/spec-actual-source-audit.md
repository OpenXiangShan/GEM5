# SPEC 实际构建树与参考源核对

已可读取 `/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src`。但它不是已证实的 2026-08-20 profile 历史快照：当前目录及其 runspec 构建产物为 10 月 13 日准备和构建，当前 mcf、perlbench、astar 三个 ELF 的 SHA256 均与 profile 保存的 8 月 ELF 不同。路径相同、可读并不证明源码历史身份。profile 自带构建日志分别记录 8 月 17 日 astar 21:06、mcf 21:27、perlbench 21:30 构建完成；checkpoint request 日期 8 月 19 日。时间不能证明中间内容未变。

在 `src` 子目录递归逐文件比较 C/H/C++ 和 inc（排除对象文件、build/run 二进制）：

| workload | 当前 / 参考文件数 | 哈希差异 |
|---|---:|---:|
| 429.mcf | 25 / 25 | 0 |
| 400.perlbench | 125 / 125 | 3 |
| 473.astar | 19 / 19 | 0 |

参考树是 `/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006`。当前 spec-src 的全部 25、125、19 个对应源码又分别与其当前 10 月 runspec build 副本完全相同。JSON 保留每个源文件两侧 SHA256。这个结论提升了“当前两个源码树一致性”的置信度，不能提升为“参考源码与 8 月 profile ELF 完全匹配”。

perlbench 三个差异：`perlio.c` 改动位于 `__sun__` 分支；`scope.h` 增加 `SPEC_CPU_IA64_GCC_ALIGNMENT` 条件；`win32/win32.h` 扩展 Windows 相关条件。当前及参考文件各自的 MD5 均匹配对应 SPEC 树的 `MANIFEST`；其 version.txt 分别为 109 和 92（mcf 98/89、astar 100/92）。这些是树版本间差异的证据，不能将其断定为 workload-builder 定制 patch，也不能仅凭平台条件推断历史 RISC-V 二进制完全不受影响。

8 月 profile 保存的 `cfg/gcc16-linux-riscv64-rva23u64_novec.cfg` 第 63、102–114 行提供 GCC 16.1、`-O3` 和 `-flto` 的配置出处；对应 `logs/build_elf/{mcf,perlbench_checkspam,astar_biglakes}.log` 实际编译/链接命令分别 14、54、14 条，全部含 `-O3 -flto`，未见显式 `-g`。现存 ELF 的 libgcc DWARF 不等于应用源码行信息。构建输出树定向查找未见 `.patch`/`.diff`；`workloads/linux/spec2006/{build.sh,spec2006-package.py,README.md}` 仍被 Permission denied，无法完整判断构建期间是否有其他补丁或源码生成步骤。

下一步要证明 8 月精确源码，需寻找 profile 同期源码归档、构建目录副本、源清单哈希或可核验的打包记录；无法用当前树和参考树的相互一致性替代这一证据。LTO 反汇编与 `pbeampp.c` 的结构相似性仍属候选定位。

## 新旧构建差异与候选关系

10 月三个 ELF 的应用编译日志也都为 `-O3 -flto` 且未显式 `-g`；其构建命令相对 8 月增加 `-mtune=xiangshan-kunminghu`、`-mstrict-align`，`-march` 还增加 `supm1p0`。因此当前 ELF 差异不能简单视为“加 -g 导致”，而且现有当前产物不是带应用源码行信息的同配置重编。编译器路径相同，但 8 月编译器本体 hash 未找到，不能证明其未变化。

两版日志都包含 `-ljemalloc`，但旧 mcf/perl ELF 的 `malloc` 与 `__libc_malloc` 同址、没有 `malloc_conf`；当前同两项 ELF 存在 `malloc_conf`。这是符号与链接结果改变的直接证据，具体链接器选择原因未知；astar 两版均有 `malloc_conf`。上述编译参数和链接解析变化均足以阻断使用当前 ELF 地址直接替代历史 ELF，不判断每项对各热点的单独贡献。

profile 的 `stamps/*.images.stamp` 是空文件，目标范围内未找到 8 月源码包 hash 或 prepare 标记；当前 `spec-src.e8b58d3dd8b0a77f4ab73d8b1ec855392436e14056601e3e1a4b458dd118caff.prepared` 为 10 月标记，无法验证 8 月来源。

mcf 25/25 一致覆盖 src 中 11 个 `.c`、14 个 `.h`；astar 19/19 覆盖 src 中 11 个 `.cpp` 和 8 个 `.h`。perlbench 的 `regexec.c` 两个当前可读树相同，`regexec.c` 包含 `perl.h`，后者包含有差异的 `scope.h`；增加的 IA64 条件宏未见于 8 月 RISC-V 编译命令。`perlio.c` 差异属于 `__sun__` 分支，`win32.h` 属 Windows 路径，未见它们直接修改 `regexec.c`；但 8 月实际头文件与其他间接影响未能证明，不能据此给历史 LTO 热点升级精确源码行。
