---
name: gem5-manual-perf-trigger
description: 用于在本地通过 `gh` 远程触发 OpenXiangShan/GEM5 的 `manual-perf.yml`，并在触发后检查 run 是否正常创建。适用于用户明确要求触发 `manual-perf`、组装 `gh workflow run` 命令、校验 workflow 输入项、或确认触发后的 Actions run 状态。
---

# GEM5 Manual Perf Trigger

## 概述

这个 skill 用于安全、可追溯地触发远端 GitHub Actions `manual-perf.yml`。核心目标不是只把命令发出去，而是先确认本地触发条件，再确认输入参数，再在触发后检查 run 是否真的正常创建。

当前约束：

- 已确认 `manual-perf.yml` 在本仓库上可通过 `gh workflow run` 触发。
- 已实际走通的配置只有 `kmhv3.py`。
- 其他配置虽然在 workflow 的 `inputs` 中存在，但当前没有实际跑通记录，触发前必须明确提醒用户这一点。

## 工作流

### 第 1 步：检查本地触发能力

优先检查 `gh` 是否可用：

```bash
gh --version
```

如果 `gh` 不可用：

- 明确提示用户先安装 `gh`。
- 或者提供 GitHub REST API `workflow_dispatch` 的 `curl` 方案。
- 但必须同时说明：`curl` 触发流程在当前仓库协作里未实际走通过，只能作为备选方案，可靠性不如 `gh` 已验证路径。

不要在 `gh` 缺失时直接假设 `curl` 方案可用并继续执行。

### 第 2 步：检查认证状态

检查 `gh` 是否已登录：

```bash
gh auth status
```

如果未登录，或者 token 无效：

- 提示用户先执行 `gh auth login`
- 或者提示用户配置具备 `repo`、`workflow` 权限的 token

说明要点：

- 使用 `gh workflow run` 时，推荐优先走 `gh auth login`
- 如果未来改走 REST API，则需要用户自行准备 token

### 第 3 步：读取 workflow 输入项

在触发前，读取并核对 `.github/workflows/manual-perf.yml` 的 `workflow_dispatch.inputs`。

当前已知关键输入项：

- `configuration`
- `benchmark_type`
- `specific_benchmarks`
- `vector_type`
- `branch`

当前已验证存在的配置选项：

- `kmhv2.py`
- `kmhv3.py`
- `idealkmhv3.py`

但必须提醒用户：

- `manual-perf` 已实际走通的是 `kmhv3.py`
- 其他配置目前只是 workflow 中支持，暂未在本技能流程里实际验证

### 第 4 步：向用户确认触发参数

在真正触发前，必须向用户确认以下信息：

- 配置文件：如 `kmhv3.py`
- benchmark 类型：如 `gcc12-spec06-1.0c`
- 是否只跑指定 benchmark：如 `mcf,omnetpp`
- 目标分支、tag 或 SHA：通常是当前分支
- `vector_type`：默认 `base`

如果用户没有给全：

- 主动补问缺失项
- 不要私自猜测 benchmark 集合

如果用户要触发非 `kmhv3.py` 配置：

- 必须先提示“当前只有 `kmhv3.py` 经过实际触发验证，其他配置暂未在此流程中验证”

### 第 5 步：先展示命令，再执行

触发前先把准备执行的命令原样展示给用户，得到确认后再执行。

标准命令模板：

```bash
gh workflow run manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref <branch> \
  -f configuration=<configuration> \
  -f benchmark_type=<benchmark_type> \
  -f specific_benchmarks=<specific_benchmarks> \
  -f vector_type=<vector_type> \
  -f branch=<branch>
```

说明：

- `--ref` 使用要触发的远端分支
- `branch` input 也通常填写同一个分支
- `specific_benchmarks` 可为空；为空表示按 workflow 跑该集合全部 benchmark

### 第 6 步：触发后回查 run

只拿到 “Created workflow_dispatch event” 不够。触发成功后必须继续检查：

1. 返回的 run 链接是否存在
2. `gh run view <run-id>` 是否能正常读取
3. run 是否进入了正常状态，例如 `queued`、`in_progress`，而不是立刻 `failure` 或不存在

建议的检查方式：

```bash
gh run view <run-id> --repo OpenXiangShan/GEM5
```

必要时可继续查看：

```bash
gh run view <run-id> --repo OpenXiangShan/GEM5 --json status,conclusion,url,name,headBranch,headSha
```

如果 run 创建失败、找不到、或立刻异常终止：

- 不要简单说“已触发”
- 要明确说明触发异常，并给出当前检查到的状态

## 输出要求

执行本 skill 时，回复应遵守以下格式约束：

- 先说明当前准备做什么
- 如果缺少 `gh` 或认证，先阻断并提示用户处理
- 触发前先展示完整命令
- 明确提醒：`manual-perf` 当前实际走通过的是 `kmhv3.py`，其他配置暂未实际验证
- 触发后提供 run 链接
- 触发后追加 run 状态检查结果，而不是只给链接

## 失败处理

### `gh` 未安装

给出两条路径：

1. 安装 `gh` 后再触发
2. 使用 REST API `workflow_dispatch`

但必须注明：

- REST API `curl` 路径当前未在本流程中实际走通过
- 它是备选，不是首选

### `gh` 已安装但未登录

提示：

```bash
gh auth login
gh auth status
```

### workflow 输入不完整

阻断执行，向用户补问：

- 配置文件
- benchmark 类型
- benchmark 子集
- 分支

### 触发后找不到 run

优先检查：

- 仓库名是否正确
- 分支是否已 push 到远端
- workflow 文件名是否正确
- token 是否具备 `workflow` 权限

## 示例

### 示例 1：只跑 `mcf,omnetpp`

用户意图：

- 配置：`kmhv3.py`
- benchmark：`gcc12-spec06-1.0c`
- 仅跑：`mcf,omnetpp`
- 分支：`turbo-pfalign-dualPort-l2Bank-nopf`

先展示命令：

```bash
gh workflow run manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref turbo-pfalign-dualPort-l2Bank-nopf \
  -f configuration=kmhv3.py \
  -f benchmark_type=gcc12-spec06-1.0c \
  -f specific_benchmarks=mcf,omnetpp \
  -f vector_type=base \
  -f branch=turbo-pfalign-dualPort-l2Bank-nopf
```

触发后再检查 run 状态，并返回链接与状态。

## 资源

当前 skill 不依赖额外 `scripts/`、`references/`、`assets/`。
