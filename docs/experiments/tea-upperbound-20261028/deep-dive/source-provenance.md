# SPEC06 mcf / perlbench_checkspam 源码与 ELF 溯源

profile `/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820` 对两 workload 均保存独立 ELF、rootfs、firmware、manifest、build log 和 workload command。流式提取 rootfs 的 `spec/mcf`、`spec/perlbench` 后，SHA256 分别与 `elf/mcf.elf`、`elf/perlbench_checkspam.elf` 完全一致；两个 rootfs 和 firmware 本体 SHA256 与各自 manifest 一致。JSON 保留完整 hash 与路径。`metadata/requests/*.yaml` 记录原始 firmware 路径及 20M interval；原始 `/nfs/home/share/wujiabin/workload_bin/...` 路径当前不可访问，因此没有核验原始 firmware。

`logs/build_elf/{mcf,perlbench_checkspam}.log` 记录实际 runspec 构建路径 `/nfs/home/wujiabin/work/workload-builder/build/linux-workloads/spec2006/spec-src`，构建 GCC 16、`-O3 -flto` 等。用户已于本轮开放目录权限，当前构建源可读；初查时权限不足的缺口已解决。可读 SPEC 参考源码分别位于 `/nfs/home/share/speccpu_install/cpu2006v99/benchspec/CPU2006/429.mcf/src` 与 `.../400.perlbench/src`。perlbench checkspam 输入 `.../400.perlbench/data/ref/input/checkspam.pl` SHA256 与 rootfs `spec/checkspam.pl` 均为 `b9668148e57059ef2c3657d074c21a4a313258a7bd6295b002448823651c0332`，只证明该输入脚本一致。

两 ELF 虽有 `.debug_info`，观察到的 DW_AT_comp_dir 仅为 GCC toolchain libgcc 路径；mcf `addr2line -a -f -i 0x108d2 0x108e4` 均落在 `main`，源文件与行号均为 `??:?`。反汇编 0x108d2 为 `lw t4,24(a5)`、0x108e4 为 `ld t6,0(t6)`；可读源码 `pbeampp.c:167-170` 的扫描与依赖结构相似，仅为候选，不是已确认的 LTO 内联源行。perlbench 的热点 PC 还未选定；后续需实际 PC、符号与指令序列，再评估可否对照源代码。


## 编译参数补核

实际构建日志中mcf的12条编译/链接命令、perlbench_checkspam的52条命令均使用`-O3 -flto`，没有`-g*`选项。这里仅计实际执行的绝对路径命令，不重复计日志中`COMP:`/`LINK:`模板。热点无源码行的主要原因是SPEC应用未生成可用的行级调试信息；ELF里观察到的调试信息来自libgcc。

如果获得加`-g`重编的ELF，先比较`.text`地址与字节是否一致，再用于旧PC的行号映射；不把另一份重新布局的ELF地址直接套到原checkpoint。当前精确汇编和参考源码语义分析仍可继续，不把这项缺口当作性能实验阻塞。

## 实际目录开放后的核对

当前mcf的25个C/H文件、astar的19个CPP/H文件与共享参考树完全相同；perlbench的125个文件中只有`perlio.c`、`scope.h`、`win32/win32.h`不同，`regexec.c`相同。三处差异分别符合两树自身SPEC MANIFEST，不能直接称为workload-builder新增补丁。

当前目录在10月重建，三个当前ELF均与8月profile ELF不同；编译参数新增`-mtune=xiangshan-kunminghu`、`-mstrict-align`和ISA项，mcf/perlbench的allocator符号解析也变化。因此当前源可用于核对结构和算法，但没有8月源码包hash，仍不能把当前ELF替代checkpoint的精确ELF，或将历史PC武断映射到某一行。详见[逐文件与构建时间线审计](spec-actual-source-audit.md)、[当前实际源码热点映射](actual-source-hotspot-map.md)。
