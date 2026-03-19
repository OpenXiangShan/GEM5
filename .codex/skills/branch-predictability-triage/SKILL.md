---
name: branch-predictability-triage
description: "用于分析分支 PC 对应的 ELF / 函数 / 源码语义，并判断该分支更像是语义上天然难预测，还是更像预测器没有学好。适用于 SPEC06 checkpoint、benchmark ELF、topMispredictsByBranch.csv、单个 branch PC 归因。"
---

# 分支可预测性归因技能

## 何时使用

- 你手里有一个或多个 branch PC，想知道它们属于：
  - benchmark 主体
  - runtime / toolchain（如 `libgcc`、`glibc`、`jemalloc`）
  - `bbl` / kernel / 高地址运行时
- 你想把 branch PC 尽量对应到：
  - ELF
  - 函数名
  - 源码文件 / 代码块
- 你想判断一条分支更像：
  - 语义上天然难预测
  - 结构上本应较容易预测，但 predictor 没抓住
- 你想分析 SPEC06 切片，例如：
  - 新 profile：`/nfs/home/share/checkpoints_profiles/.../checkpoint-0-0-0`
  - 老 profile：`/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/...`

## 目标

输出一份面向分支预测分析的结论，而不是单纯做地址翻译。最终结论至少要回答：

1. 这条 branch PC 属于哪类代码。
2. 能否对应到 benchmark 自身源码。
3. 这条分支的控制流模式是什么。
4. 更像是“天然难预测”还是“predictor 还有提升空间”。

## 输入优先级

优先收集以下信息：

1. branch PC 列表
2. 切片名或 benchmark 名
3. 切片根目录
4. 可能的 `topMispredictsByBranch.csv` / `topMisrateByBranch.csv`
5. 如有，用户本地 benchmark 源码树

如果用户只给了 PC，没有给切片名，也可以先按地址区间做粗分类。

## 地址分类规则

先判断 PC 是否属于 benchmark ELF 的装载范围，不要一上来就找源码行。

### A. 低地址、落在 benchmark ELF `.text`

常见表现：

- `0x10000` 左右开始的静态可执行映像
- `llvm-symbolizer` / `nm` 能解析到 benchmark 函数

处理策略：

- 优先认为是 benchmark 主体或静态链接进 benchmark 的 runtime/helper

### B. 高地址，如 `0x8000xxxx`

常见表现：

- 不落在 benchmark ELF 的 LOAD 段
- 更像 `bbl` / opensbi / kernel / 其他 runtime

处理策略：

- 不要强行拿 benchmark ELF 解
- 先说明该地址大概率不属于 benchmark 主体
- 如果没有对应 runtime ELF，只能停在“非 benchmark 代码”这一层

## 仓库内常见路径

### 新 profile

- checkpoint 根目录：`/nfs/home/share/checkpoints_profiles/<profile>/checkpoint-0-0-0`
- ELF 目录：`/nfs/home/share/checkpoints_profiles/<profile>/elf`

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

- `/nfs/home/yanyue/tools/cpu2006_analyze/benchspec/CPU2006`

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
rg -n '^.*\\bpush_slidE\\b\\s*\\(' /nfs/home/yanyue/tools/cpu2006_analyze/benchspec/CPU2006/458.sjeng/src/*.c
```

这一步的目标不是强行制造“精确某一行”，而是定位到：

- 哪个函数
- 哪个 `if/else/loop`
- 它的输入依赖是什么

### 第六步：判断分支类型

每条分支至少归到以下一种：

- `loop-exit`
- `guard / fastpath`
- `predicate-result`
- `max/min update`
- `pointer/null/empty check`
- `state-machine / parser / regex`
- `runtime/helper`

### 第七步：输出预测性结论

结论至少包含：

- 该分支更像“结构型易预测”还是“语义型难预测”
- 如果 predictor 表现差，更该怀疑：
  - predictor 模型 / 历史建模 / alias / 容量
  - 还是输入分布本身导致的不可规整

## 预测性判断准则

下面是默认启发式，不是绝对规则。

### 通常偏容易预测

- `for/while` 循环退出条件
- 连续扫描直到边界/空值/哨兵值
- 长度下界检查
- 空指针 / 空格 / `npiece` / `frame` / null-check
- 稳定模式位，例如 `captures`、`mode`、`flag` 长时间不变
- 明显偏置的错误路径 / 稀有路径

常见表现：

- 连续若干次 taken，然后一次 not-taken
- 连续若干次 not-taken，然后一次 taken
- 同一 phase 下高度偏置

如果这类分支 mispredict 很高，更值得怀疑：

- predictor 没学住简单结构
- 同一 PC 混入太多上下文
- 表项别名或容量冲突

### 通常更难预测

- regex / parser / symbol-table / search-state 驱动的判断
- `if (value > best)` 这种“刷新最大值/最小值”类分支
- 依赖 `load` 出来的动态值，再做分类/比较
- 依赖输入真假分布的 filter / predicate 结果
- 依赖多重全局状态的启发式判断
- 匹配成功/失败、查表命中/未命中、搜索剪枝命中/未命中

常见表现：

- 同一 PC 在不同 phase 下行为变化很大
- taken ratio 接近中间值
- 结果高度依赖输入内容或状态机位置

如果这类分支 mispredict 很高，不一定说明 predictor 有明显问题；可能是语义上本来就更难。

## 典型案例模板

### 案例 A：滑动子走子生成

类似：

- `board[target] == npiece`
- `board[target] != frame`

判断：

- 这是典型扫描型分支
- 通常结构规整，偏容易预测
- 如果预测差，优先怀疑 predictor 没把 ray 长度/phase 模式学好

### 案例 B：搜索排序中的“刷新最大值”

类似：

- `if (move_ordering[i] > best)`

判断：

- 这是数据相关分支
- 依赖 move ordering 分布
- 比 loop-exit 明显更难
- 预测差未必是 predictor bug

### 案例 C：regex / match 成败

类似：

- `if (!s) goto nope;`
- `if (CALLREGEXEC(...))`

判断：

- 强依赖输入文本、状态、匹配位置
- 通常比长度检查更难预测

## 输出格式建议

对每个 branch PC，建议输出以下字段：

- `pc`
- `benchmark`
- `elf`
- `belongs_to`
  - `benchmark`
  - `runtime/toolchain`
  - `bbl/high-address`
- `function`
- `source_candidate`
- `semantic_pattern`
- `predictability`
  - `easy`
  - `medium`
  - `hard`
- `why`
- `tage_interpretation`
  - `more_like_predictor_issue`
  - `more_like_semantically_hard`
  - `mixed`

## 使用时的注意事项

- 不要把“无法精确到行号”误判为“完全无法分析”。
- 对 benchmark 主体，函数级定位 + 本地源码块通常已经足够做预测性判断。
- 对 `0x8000xxxx` 这类地址，先排除 runtime/bbl，再谈源码。
- 对 `libgcc/glibc` helper，要明确告诉用户：这不是 benchmark 自身算法分支。
- 如果用户的目标是比较 predictor 设计优劣，优先找：
  - 结构上本应好预测，但 mispredict 很高的分支
- 如果用户的目标是解释 workload 本身难度，优先找：
  - 语义上强数据相关的分支

## 默认结论风格

回答时优先给出：

1. 该 PC 属于什么代码
2. 它大致对应哪段源码逻辑
3. 它属于哪种分支模式
4. 我为什么判断它偏 easy / hard
5. 我更倾向把责任归到 predictor 还是 workload 语义
