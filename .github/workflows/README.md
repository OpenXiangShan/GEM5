# GEM5 分层CI架构 (Tiered CI)

解决当前CI在PR阶段运行过久、拖慢开发效率的问题。

---

## 📊 核心改进

| 阶段 | 之前 | 现在 | 改进 |
|-----|------|------|------|
| PR快速反馈 | 2-4小时 | **5-10分钟** | ⚡ 95%+ |
| 按需性能测试 | 每个PR强制 | 需要时触发 | 🎯 按需 |
| 完整测试 | 每个PR重复 | 只在合入后 | ✅ DRY |

---

## 层次一：PR快速检查 (Tier 1) ⚡

**文件**: `.github/workflows/pr-quick-check.yml`

**目标**: 5-10分钟内给出快速反馈

**触发**: 每次 push 到 PR 分支

**内容**:
- ✅ 编译 GEM5 opt 版本
- ✅ 单元测试 (Unit Tests)
- ✅ 冒烟测试 (Difftest Check)

**说明**: 
- 遵循 DRY 原则，这些测试不会在 Post-Merge 阶段重复运行
- 使用本地 DRAMSim3 缓存，避免网络IO

---

## 层次 1.5：按需性能测试 (Tier 1.5) 🎯

**文件**:
- `.github/workflows/gem5-ideal-btb-perf.yml`
- `.github/workflows/gem5-align-btb-0.3c.yml`
- `.github/workflows/on-demand-spec-rvv.yml`

**目标**: 在合入前，按需检查有性能风险的 PR

**触发**: 在 PR 上添加对应标签

### 支持的标签

- `perf`: 触发 `idealkmhv3.py` + `gcc15-spec06-1.0c`
- `perf-align`: 触发 `kmhv3.py` + `gcc15-spec06-0.3c`
- `rvv`: 触发 `idealkmhv3.py` + `spec06int-rvv-0.8c`

### 权限控制

仅以下角色可触发：OWNER / MEMBER / COLLABORATOR

### 当前实现

- 添加 `perf` / `perf-align` 标签会在对应性能 workflow 中触发测试，workflow 会记录标签创建时 PR 的 head SHA 确保结果对应正确的 commit
- `rvv` 标签仍由独立的 RVV on-demand workflow 触发
- Label 触发只允许同仓库 PR；外部 fork PR 需要先由维护者同步到受信任分支，再通过 label 或 `manual-perf.yml` 触发
- 需要手动选择配置、benchmark 或 branch/SHA 时，请使用 `manual-perf.yml`
- `idealkmhv3.py` 默认关闭动态预取；`smt_idealkmhv3.py` 保持当前默认行为
- 需要手动切换动态预取时，请在 `manual-perf.yml` 的 `extra_args` 中直接写 `--enable-dynamic-pf=True|False`

### Dynamic Prefetch Toggle

`manual-perf.yml` 不再提供独立的动态预取输入。相关行为由 `extra_args` 显式控制：

| 开关值 | 语义 |
| --- | --- |
| `False` | 显式关闭动态预取控制 |
| `True` | 显式启用动态预取控制，窗口为 8000 cycles，并打开 L1D/L2/L2Wrapper PFBad 表 |

如果 `extra_args` 没有显式传 `--enable-dynamic-pf`，`idealkmhv3.py` 会默认保持关闭。SMT 配置保持现有默认行为不变。

### 性能结果

由现有的性能评论机器人 (`actions_gem5.py`) 自动处理；label workflow 本身不再发送触发提示评论：
- 📊 与主分支性能对比
- 📊 与PR上一个commit对比
- 📊 详细的性能指标表格

### 优势

- 只在需要时运行，节省资源
- 支持多种 benchmark 类型
- 添加新 benchmark 类型只需修改 template

---

## 层次二：主线完整测试 (Tier 2) 🛡️

**目标**: 确保 `xs-dev` 分支永远健康、可发布

**触发**:
- PR 合入 `xs-dev` 分支后自动运行
- 在目标分支为 `xs-dev` 的同仓库 PR 上添加 `regression` 标签，合入前按 PR head commit 运行

### 包含的测试 Workflows

#### 1. `gem5.yml` - 功能回归测试
5 个并行 jobs（遵循 DRY 原则，排除已在 Tier 1 运行的测试）

维护者可以在 PR 上添加 `regression` 标签，提前运行与合入后相同的完整功能回归。为避免 `pull_request_target` 执行外部代码，该入口仅接受目标分支为 `xs-dev` 的同仓库 PR。

**已移除**（避免重复）:
- ~~`unit_tests`~~ → 在 `pr-quick-check.yml`
- ~~`difftest_check`~~ → 在 `pr-quick-check.yml`
- ~~legacy GC/GCB checkpoint suite~~ → 覆盖陈旧且失败定位成本高
- ~~standalone RV64GCBV checkpoint smoke~~ → 已有 vector micro-tests 和 RVV checkpoint 性能回归
- ~~L2TLB checkpoint regression~~ → 长期未发现测试本体失败

#### 2. `gem5-ideal-btb-perf.yml` - Ideal BTB 性能测试
默认跑 `gcc15-spec06-1.0c`，在 `xs-dev`、`*-perf` 分支和 PR `perf` 标签上自动触发

#### 3. `gem5-align-btb-0.3c.yml` - Align 性能测试
默认跑 `gcc15-spec06-0.3c`，在 `xs-dev`、`*-align` 分支和 PR `perf-align` 标签上自动触发

#### 4. 其他测试
- `gem5-vector.yml` - RVV 扩展测试
- `gem5-ideal-btb-perf-weekly.yml` - 定时任务（每周四），包含 gcc15/spec17 常规回归、gcc12 `idealkmhv3.py` 动态预取回归，以及 SMT SPEC06 int-only dynamic prefetch 回归

---

## 🔑 配套策略

### 1. "主线红了" 怎么办：立即回滚 (Revert)

**原则**: 不允许主线 (`xs-dev`) 保持红色状态

**动作**:
```bash
git revert <merge-commit-sha> -m 1
git push origin xs-dev
```
或者直接在github 网页端，找到已经被关闭的PR, 在最下方有revert 按钮，来直接revert 这个PR.

**后续**: 原 PR 作者修复 Bug 后，重新提交新的 PR

### 2. 合并策略：必须支持回滚

**推荐**: ✅ "Create a merge commit"
- 保留 PR 完整提交历史
- 回滚简单

**禁用**: ❌ "Rebase and Merge"
- 难以回滚
- 回滚操作危险

---

## 📖 使用指南

### PR 作者

```bash
# 场景1: 小改动（文档/注释）
# 只需要通过 Tier 1 快速检查即可

# 场景2: 性能相关改动
# 在 PR 上添加 perf 标签，运行 Ideal BTB 性能测试（idealkmhv3.py / gcc15-spec06-1.0c）
# 在 PR 上添加 perf-align 标签，运行 Align BTB 性能测试（kmhv3.py / gcc15-spec06-0.3c）

# 或者把当前分支改名为*-perf, 每次 push 会自动运行 gcc15-spec06-1.0c。
# 如果是对齐 RTL 的轻量评估，可使用 *-align, 每次 push 会自动运行 gcc15-spec06-0.3c。
```

### 维护者

1. 检查 Tier 1 快速检查结果
2. 对于性能敏感的 PR，添加 `perf` 或 `perf-align` 标签
3. 对于可能影响 gem5 功能回归的 PR，添加 `regression` 标签
4. 审查代码和性能影响
5. 合入后监控 Tier 2 测试
6. 如发现失败，立即回滚

---

## 🤖 性能评论机器人

**位置**: `https://github.com/OpenXiangShan/env-scripts/blob/main/github/actions_gem5.py`

**运行**:
```bash
python actions_gem5.py --token <github-token> --always-on

# 可以联系yanyue 来重新触发机器人
```

**兼容性**: 完全兼容新的分层 CI

---

## 🎯 设计原则

- **DRY**: 测试不重复，配置单一来源
- **KISS**: 简化 workflow，最小化复杂度
- **Fail Fast**: PR 阶段快速发现问题
- **Separation of Concerns**: 快速检查 vs 完整验证

---

## 📚 相关文件

- `.github/workflows/pr-quick-check.yml` - Tier 1
- `.github/workflows/gem5-perf-template.yml` - 性能测试模板
- `.github/workflows/gem5.yml` - `xs-dev` 合入后或 `regression` 标签触发的完整功能回归
- `.github/workflows/gem5-ideal-btb-perf.yml` - `xs-dev` / `*-perf` / `perf` 标签默认性能测试
- `.github/workflows/gem5-align-btb-0.3c.yml` - `xs-dev` / `*-align` / `perf-align` 标签默认对齐性能测试
- `.github/workflows/on-demand-spec-rvv.yml` - `rvv` 标签 RVV 性能测试
- `env-scripts/github/actions_gem5.py` - 性能评论机器人

---

## 💡 常见问题

**Q: 为什么 PR 不再自动运行性能测试？**
A: 性能测试耗时长，会拖慢 PR 审查。现在改为按需触发，既节省资源，又保持灵活性。

**Q: 如何触发性能测试？**
A: 在 PR 上添加 `perf` 或 `perf-align` 标签；需要自定义配置时使用 `manual-perf.yml`。

**Q: 如何在合入前运行完整的 gem5 功能回归？**
A: 在目标分支为 `xs-dev` 的同仓库 PR 上添加 `regression` 标签。workflow 会按标签创建时的 PR head commit 运行，外部 fork PR 不会触发。

**Q: 为什么外部 fork PR 加标签不会触发性能测试？**
A: 性能测试会 checkout 并执行 PR 代码。为了避免 `pull_request_target` 执行外部 fork 代码，label 触发仅允许同仓库 PR。

**Q: 新增 benchmark 类型需要修改哪些文件？**
A: 只需修改 `gem5-perf-template.yml`

**Q: 如何跑动态预取性能测试？**
A: 使用 `manual-perf.yml`，在 `extra_args` 里直接传 `--enable-dynamic-pf=True`。base 对比请显式传 `--enable-dynamic-pf=False`。`idealkmhv3.py` 默认关闭动态预取，SMT 配置保持当前默认行为不变。

---

## 🎉 总结

分层 CI 架构核心价值：

1. **开发效率提升 95%+**：PR 反馈从 2-4 小时降至 5-10 分钟
2. **资源优化**：性能测试按需运行
3. **灵活性**：支持多种 benchmark 类型
4. **主线稳定**：Post-Merge 完整测试确保质量
5. **易于维护**：集中管理配置，遵循 DRY 和 KISS 原则
6. **易于回滚**：保留 merge commit，回滚简单安全
