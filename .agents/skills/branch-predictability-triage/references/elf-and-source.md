# ELF、路径与源码定位

以下本机路径仅作 fallback；命令按实际输入替换。

## 环境路径

外部 checkpoint、ELF 和源码通常不在 GEM5 仓库内。按下面顺序解析，使用前先检查路径
是否存在：

1. 用户显式给出的路径。
2. 环境变量：`CHECKPOINT_PROFILE_ROOT`、`SPEC06_SOURCE_ROOT`。
3. 本机已知默认路径。

个人 home 下的路径可以作为本机默认值，但不能当作所有环境都成立的前提。

## 本机常见路径

### 新 profile

- profile 根目录：`${CHECKPOINT_PROFILE_ROOT:-/nfs/home/share/checkpoints_profiles}`
- checkpoint 目录：`<profile-root>/<profile>/checkpoint-0-0-0`
- ELF 目录：`<profile-root>/<profile>/elf`

常见映射：

- `gcc_typeck` / `gcc_scilab` / `gcc_expr2` / `gcc_200` -> `elf/gcc`
- `perlbench_splitmail` / `perlbench_diffmail` -> `elf/perlbench`
- `bzip2_*` -> `elf/bzip2`
- `gobmk_*` -> `elf/gobmk`
- `astar_*` -> `elf/astar`
- `gamess_*` -> `elf/gamess`
- `mcf` -> `elf/mcf`
- `sjeng` -> `elf/sjeng`

### 老 profile

- 根目录：`/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc`
- benchmark ELF：`elf/<bench>_base.riscv64-linux-gnu-gcc12.2.0`
- 运行镜像：`bin/*-bbl-linux-spec.bin`

注意：

- `bin/*-bbl-linux-spec.bin` 往往不是 ELF，不能直接 `addr2line`
- 真正可用于静态语义分析的通常是 `elf/` 下的 benchmark ELF

### 本地源码树

常见 SPEC2006 源码路径：

- `${SPEC06_SOURCE_ROOT:-/nfs/home/yanyue/tools/cpu2006_analyze/benchspec/CPU2006}`

例如：

- `400.perlbench/src`
- `403.gcc/src`
- `429.mcf/src`
- `458.sjeng/src`

## 推荐工具

优先使用：

- `file`
- `readelf -S`
- `readelf -Wl`
- `nm -n`
- `llvm-symbolizer`
- `llvm-objdump -d --line-numbers --source`
- `rg`

必要时使用：

- `gdb -batch -ex 'info line *ADDR'`
- `readelf --debug-dump=decodedline`

不建议默认依赖系统自带 `addr2line`，因为某些 RISC-V + DWARF 组合下它可能只能给函数名，不能稳定给源码行。

## 标准分析流程

### 第一步：确认 ELF 是否可用

先检查：

```bash
file <elf>
readelf -S <elf> | rg 'debug|symtab|strtab'
readelf -Wl <elf>
```

目标：

- 确认是否是 ELF
- 是否带 `debug_info`
- 代码装载地址范围是什么

### 第二步：判断 PC 是否属于该 ELF

如果 PC 明显不在 LOAD 段范围内：

- 直接标记为“非该 benchmark ELF 主体地址”
- 不要继续做伪映射

### 第三步：先到函数级

优先拿到函数名：

```bash
llvm-symbolizer --obj=<elf> 0xPC
nm -n <elf> | rg '<附近符号>'
```

如果只能到函数名，也不要停。函数级 + 本地源码通常已经足够做语义分析。

### 第四步：查看函数内分支上下文

```bash
llvm-objdump -d --line-numbers --source \
  --start-address=<pc附近起点> \
  --stop-address=<pc附近终点> \
  <elf>
```

重点看：

- 比较指令前的 load / and / shift / compare
- branch 是：
  - `beqz/bnez`
  - `blt/bge`
  - 循环回边
  - 早退条件
  - “刷新最大值”类选择分支

### 第五步：映射到本地源码块

如果 line table 不够稳定：

- 用函数名在本地源码树里找定义
- 再用汇编语义对到源码块

示例：

```bash
SPEC06_SOURCE_ROOT=${SPEC06_SOURCE_ROOT:-/nfs/home/yanyue/tools/cpu2006_analyze/benchspec/CPU2006}
rg -n '^.*\\bpush_slidE\\b\\s*\\(' "$SPEC06_SOURCE_ROOT"/458.sjeng/src/*.c
```

这一步的目标不是强行制造“精确某一行”，而是定位到：

- 哪个函数
- 哪个 `if/else/loop`
- 它的输入依赖是什么
