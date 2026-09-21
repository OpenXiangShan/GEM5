# 分支模式与案例

这些模式只帮助形成假设；静态语义和 taken ratio 不能单独证明预测上限或 predictor 缺陷。

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
