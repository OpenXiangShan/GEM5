---
name: gem5-manual-perf-trigger
description: 用于在本地通过 `gh` 远程触发 OpenXiangShan/GEM5 的 `manual-perf.yml`，并在触发后检查 run 是否正常创建。适用于用户明确要求触发 `manual-perf`、组装 `gh workflow run` 命令、校验 workflow 输入项、或确认触发后的 Actions run 状态。
---

# GEM5 Manual Perf Trigger

## 原则

`manual-perf.yml` 会持续变化，workflow 文件本身才是输入项、默认值和 choice 选项的事实来源。不要把这份 skill 中的历史选项当成固定接口。

区分两类请求：

- 用户只要求校验或组装命令：只做只读检查并返回命令。
- 用户明确要求触发：完成必要预检后直接触发，不再重复索要一次确认；触发后必须回查 run。

## 1. 读取目标 ref 的 workflow

先确定仓库和目标 ref，再读取本地及远端版本：

```bash
sed -n '1,180p' .github/workflows/manual-perf.yml
gh workflow view manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref <workflow-ref> \
  --yaml
```

以实际触发 ref 上的 `workflow_dispatch.inputs` 为准。当前常见字段包括：

- `note`
- `configuration`
- `benchmark_type`
- `specific_benchmarks`
- `extra_args`
- `distributed_servers`
- `distributed_jobs_per_server`
- `branch`

这只是字段示例，不替代读取 workflow。若目标 ref 尚未 push，远端无法使用该版本的 workflow。

特别检查 workflow 内的派生逻辑。例如当前 SMT benchmark 类型会强制选择 `smt_idealkmhv3.py`，此时用户传入的普通 `configuration` 不决定最终配置；报告中应把这一点说清楚。

## 2. 校验环境和参数

检查：

```bash
gh --version
gh auth status
git rev-parse --abbrev-ref HEAD
git remote -v
```

沙箱中的 `gh auth status` 可能与正常 shell 不同；认证异常时先在正常 shell 复核，不要仅凭一次沙箱结果断言 token 已失效。

参数规则：

- `--ref` 决定使用哪个远端 ref 上的 workflow 文件。
- `branch` 决定 workflow 实际测试的 branch、tag 或 SHA；留空时通常使用 dispatch ref 的 SHA，具体看目标 workflow。
- choice/required/default 均从目标 workflow 读取。
- `specific_benchmarks` 为空通常表示跑所选集合的全部 benchmark。
- `extra_args` 必须作为单个 `-f` 参数传递，避免 shell 拆词。
- 不补猜用户没有表达的 workload 子集；可安全采用 workflow 明示的默认值。

触发前给出一段简短摘要：workflow ref、被测 ref、配置、benchmark 集合、子集以及额外参数。若这些信息会导致明显不同的实验，且无法从请求或 workflow 默认值确定，再向用户补问。

## 3. 组装和触发

标准形式：

```bash
gh workflow run manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref <workflow-ref> \
  -f configuration=<configuration> \
  -f benchmark_type=<benchmark_type> \
  -f specific_benchmarks=<specific_benchmarks> \
  -f 'extra_args=<extra_args>' \
  -f distributed_servers=<servers> \
  -f distributed_jobs_per_server=<jobs> \
  -f branch=<tested-ref>
```

只传需要覆盖的字段即可；让 workflow defaults 处理其余字段。不要因为旧 skill 没列出某个新选项就拒绝它，只要目标 workflow 明确支持。

## 4. 找到并检查新 run

`gh workflow run` 通常不直接返回 run ID。记录触发时间，然后从 workflow-dispatch runs 中匹配目标 head branch、创建时间和 run 名称：

```bash
gh run list \
  --repo OpenXiangShan/GEM5 \
  --workflow manual-perf.yml \
  --event workflow_dispatch \
  --limit 20 \
  --json databaseId,createdAt,status,conclusion,url,name,displayTitle,headBranch,headSha
```

定位后读取：

```bash
gh run view <run-id> \
  --repo OpenXiangShan/GEM5 \
  --json status,conclusion,url,name,headBranch,headSha
```

至少报告 URL、run ID、head、`status` 和 `conclusion`。`queued` 或 `in_progress` 只表示已创建并开始排队/执行，不等于通过。若用户只要求触发且无需持续盯守，确认 run 存在后停止轮询。

## 失败处理

- `gh` 缺失：说明需要安装 GitHub CLI；不要静默改用未校验的 REST 脚本。
- 认证失败：提示在正常 shell 复核 `gh auth status`。
- 目标 ref 不存在：要求先 push 或改用已有 ref。
- 输入不被接受：重新读取该 ref 的 workflow，核对字段名、choice 和类型。
- 找不到新 run：检查 workflow ref、触发时间、head branch 和 token 的 Actions 权限；不要声称已经成功触发。

## 输出要求

清楚区分：准备触发、dispatch 已提交、run 已创建、run 已完成。没有最终 `conclusion=success` 时，不写“CI 已通过”。
