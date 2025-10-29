# AGENTS.md

本文件定义本项目的开发规范与本仓库范围内的回收站删除流程（Recycle Bin SOP）。

适用范围：本仓库全目录（若子目录包含更具体的 `AGENTS.md`，则子目录文件优先）。

## 回收站删除流程（Recycle Bin SOP）

目标：任何“删除”操作都应视为“移入临时回收站”，实现可回溯、可恢复、审阅更友好（KISS / YAGNI / SOLID 中 SRP-单一职责：删除=迁移；DRY：统一流程）。

- 目录：`.recycle_bin/`（已创建，并在 `.gitignore` 中忽略其内容；通过 `.gitkeep` 保持目录存在）
- 忽略规则：
  - `.recycle_bin/*`（忽略所有内容）
  - `!.recycle_bin/.gitkeep`（保留占位文件）

### 标准操作

1) 使用 `git mv` 移动待删除文件/目录至回收站：

```bash
# 保持相对目录结构（git 会自动创建所需子目录）
git mv path/to/file.ext .recycle_bin/path/to/file.ext
```

2) 提交信息格式：

```
chore(trash): move path/to/file.ext to .recycle_bin/ (reason)

- reason: 简述迁移原因（弃用/重复/重构/临时下线等）
```

3) 代码引用清理：
- 迁移后，若存在编译/运行引用，按需修正；避免“软删除”造成死链。

4) 保留与清理：
- `.recycle_bin/` 中的内容可在评审/回滚窗口后再做永久删除（`git rm`）；
- 若需恢复，直接 `git mv .recycle_bin/path/... path/...`。

### 注意事项

- `.gitignore` 仅影响未跟踪文件。通过 `git mv` 移入回收站的文件仍受版本控制，可被评审与回滚。
- 大体量迁移建议分多次提交，保持每次改动集中、可审阅。
- 若子项目已有独立回收策略，以子项目 `AGENTS.md` 为准。

## 其他建议（可选）

- 提交信息：遵循 `area: imperative summary`；正文阐述动机与影响。
- 编码/格式化：遵循仓库已有规范（参见 `project_core_bundle` 记忆）。

## 本地钩子 / Pre-commit 检查

- 已配置本地 `pre-commit` 钩子，禁止直接删除：
  - 配置文件：`.pre-commit-config.yaml`
  - 脚本：`util/hooks/check_recycle_bin.sh`
  - 行为：若检测到 `git diff --cached` 中存在 `D <path>`（且不在 `.recycle_bin/` 下），提交会被阻止，并提示改用 `git mv` 移入 `.recycle_bin/`。

- 安装（首次）：
  ```bash
  pre-commit install
  ```

- 允许的情况：
  - 将文件移动至 `.recycle_bin/`（`git mv`，通常显示为 `R*` 重命名）。
  - 对 `.recycle_bin/` 内的文件执行永久删除（`git rm .recycle_bin/...`）。

- 恢复：
  - `git mv .recycle_bin/path/... path/...`

