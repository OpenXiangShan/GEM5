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

**文件**: `.github/workflows/on-demand-spec.yml`

**目标**: 在合入前，按需检查有性能风险的 PR

**触发**: 在 PR 上添加 `perf` 标签（默认跑 spec06-0.8c），或手动触发 workflow 选择 benchmark

### 支持的命令

```bash
/run-spec                    # 默认：SPEC06 INT 80%覆盖率 (~500 checkpoints)
/run-spec spec06-1.0c        # SPEC06 100%覆盖率
/run-spec spec17-1.0c        # SPEC17 100%覆盖率
/run-spec spec06-rvv-1.0c    # SPEC06 RVV扩展 100%
/run-spec spec06int-rvv-0.8c # SPEC06 INT RVV 80%
```

### 权限控制

仅以下角色可触发：OWNER / MEMBER / COLLABORATOR

### 当前实现

- 添加 `perf` 标签会自动触发 spec06-0.8c，workflow 会记录标签创建时 PR 的 head SHA 确保结果对应正确的 commit
- 需要其他 benchmark 时，可通过 Actions -> On-Demand SPEC workflow 手动输入 PR 号和类型

### 性能结果

由现有的性能评论机器人 (`actions_gem5.py`) 自动处理：
- 📊 与主分支性能对比
- 📊 与PR上一个commit对比
- �� 详细的性能指标表格

### 优势

- 只在需要时运行，节省资源
- 支持多种 benchmark 类型
- 添加新 benchmark 类型只需修改 template

---

## 层次二：主线完整测试 (Tier 2) 🛡️

**目标**: 确保 `xs-dev` 分支永远健康、可发布

**触发**: PR 合入 `xs-dev` 分支后自动运行

### 包含的测试 Workflows

#### 1. `gem5.yml` - 功能回归测试
8个并行 jobs（遵循DRY原则，排除已在 Tier 1 运行的测试）

**已移除**（避免重复）:
- ~~`unit_tests`~~ → 在 `pr-quick-check.yml`
- ~~`difftest_check`~~ → 在 `pr-quick-check.yml`

#### 2. `gem5-perf.yml` - 标准性能测试
SPEC06 80%覆盖率性能基线

#### 3. `gem5-ideal-btb-perf.yml` - BTB性能测试
BTB 配置下的 SPEC06 性能测试

#### 4. 其他测试
- `gem5-vector.yml` - RVV 扩展测试
- `gem5-ideal-btb-perf-nosc.yml` - 无SC的BTB测试
- `gem5-ideal-btb-perf-weekly.yml` - 定时任务（每周四）

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
/run-spec                    # 标准性能测试
/run-spec spec06-1.0c        # 完整覆盖率测试

# 或者把当前分支改名为*-perf, 这样每次push 会自动运行v3 的性能。
```

### 维护者

1. 检查 Tier 1 快速检查结果
2. 对于性能敏感的 PR，评论 `/run-spec`
3. 审查代码和性能影响
4. 合入后监控 Tier 2 测试
5. 如发现失败，立即回滚

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
- `.github/workflows/on-demand-spec.yml` - Tier 1.5
- `.github/workflows/gem5-perf-template.yml` - 性能测试模板
- `.github/workflows/gem5.yml` - Tier 2 功能测试
- `env-scripts/github/actions_gem5.py` - 性能评论机器人

---

## 💡 常见问题

**Q: 为什么 PR 不再自动运行性能测试？**
A: 性能测试耗时长，会拖慢 PR 审查。现在改为按需触发，既节省资源，又保持灵活性。

**Q: 如何触发性能测试？**
A: 在 PR 评论中输入 `/run-spec [可选benchmark类型]`

**Q: 新增 benchmark 类型需要修改哪些文件？**
A: 只需修改 `gem5-perf-template.yml`

---

## 🎉 总结

分层 CI 架构核心价值：

1. **开发效率提升 95%+**：PR 反馈从 2-4 小时降至 5-10 分钟
2. **资源优化**：性能测试按需运行
3. **灵活性**：支持多种 benchmark 类型
4. **主线稳定**：Post-Merge 完整测试确保质量
5. **易于维护**：集中管理配置，遵循 DRY 和 KISS 原则
6. **易于回滚**：保留 merge commit，回滚简单安全
