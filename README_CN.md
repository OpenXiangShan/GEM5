
# xs-gem5-bayesian-scripts

[简体中文](README_CN.md) [English](README.md)

本仓库包含用于优化香山（XiangShan）在 gem5 上参数的贝叶斯优化脚本。
这些脚本设计用于配合 [xs-gem5](https://github.com/OpenXiangShan/GEM5) 仓库使用。

## 文件说明

* `bayesianOpt.py`：用于执行贝叶斯优化的主脚本。
* `checkrun.py`：用于检查仿真任务状态。
* `config.py`：用于将 YAML 配置文件转换为运行时配置。
* `remote.py`：用于在远程服务器上运行仿真任务。
* `runGem5.py`：用于执行常规 gem5 仿真任务。
* `requirements.txt`：列出了运行这些脚本所需的 Python 包。
* `configs/template.yaml`：脚本的示例配置文件。

## 使用方法

### 1. 暴露参数

首先，需要修改 gem5 源码，将你希望训练的参数暴露为命令行参数。
例如，将 `act_entries` 参数暴露为脚本参数 `--l1d-act-entries`（可能需要修改如下文件：
`$GEM5_HOME/src/mem/sms.cc`、`$GEM5_HOME/src/mem/PrefetcherConfig.py`、`$GEM5_HOME/configs/common/PrefetcherConfig.py` 等）。
请根据你需要训练的参数，做相应修改。


### 2. 构建 `configs.yaml` 文件

你需要提供一个 YAML 配置文件，来指定 gem5 仿真所需的所有配置。
我们在 `configs/template.yaml` 中提供了一个模板文件，你可以根据自己的需要进行修改。
YAML 文件格式的详细说明可参考：[configs/README_CN.md](configs/README_CN.md)。


### 3. 运行普通 gem5 仿真任务

如果你只是想运行常规的 gem5 仿真任务，可以使用以下命令：

```bash
python3 runGem5.py configs/your_config.yaml
```


### 4. 运行贝叶斯优化任务

如果你想进行贝叶斯优化训练参数，可以使用以下命令：

```bash
python3 bayesianOpt.py configs/your_config.yaml
```

当然可以，以下是该 README 的中文翻译：

---

## 示例

这是一个用于优化预取器 SMS 参数（`act_entries`、`pht_entries`、`pht_assoc`、`pht_pf_level`）的示例。

### 1. 暴露参数

你需要将 `act_entries`、`pht_entries`、`pht_assoc`、`pht_pf_level` 暴露为脚本参数，分别为：
`--l1d-act-entries`、`--l1d-pht-entries`、`--l1d_pht_associativity`、`--l1d-pht-pf-level`。

具体的修改细节可以参考此分支：[OpenXiangshan/gem5\:example-optimize-SMS-params](https://github.com/OpenXiangShan/GEM5/tree/example-optimize-SMS-params)。
然后你需要重新编译 gem5。

```sh
cd $GEM5_HOME
git checkout origin/example-optimize-SMS-params
scons build/RISCV/gem5.opt -j8
cp build/RISCV/gem5.opt $BIN_HOME/gem5.create_a_tag_by_yourself
```

### 2. 构建 `optimize_sms.yaml` 文件

示例 yaml 文件可在 `configs/example_optimize_sms.yaml` 中找到。

**注意**：你必须修改 `environment`、`gem5_bin`、`workload_path` 以适应你的环境。
其中 `gem5_bin` 应该是你编译好的 gem5 可执行文件的名称。

```yaml
environment:
  gem5_home: "" # **Attention**: replace with your gem5 home
  bin_home:  "" # **Attention**: replace with your directory where gem5 binary is located
  restorer:
    type: "GCB_RESTORER"
    path: ""  # if checkpoint has included restorer, leave it empty
  ref_so: 
    type: "GCBV_REF_SO"
    path: "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"

running:
  gem5_bin: "" # **Attention**: replace with your gem5 binary
  output_base_dir: "./output/Optimize/sms"
  resume: false
  max_proc_per_server: 4

workloads:     
  workloads_path: # **Attention**: replace with your directory where workloads are located
    "/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_archive/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0"
```

`optimization` 部分是最重要的部分，如果你希望优化参数，必须在该部分中指定参数及其取值范围。
在这个示例中，我们要优化 `act_entries`、`pht_entries`、`pht_assoc`、`pht_pf_level`。
其取值范围如下：

```yaml
optimization:
  constant_params:
    - "--ideal-kmhv3"
    - "--l1d-enable-pht"
  param_space:
    - name: "--l1d-act-entries"  # => [16, 32, 64]
      type: "pow2"
      min_exp: 4
      max_exp: 6
    - name: "--l1d-pht-entries"  # => [32, 64, 128, 256, 512, 1024]
      type: "pow2"
      min_exp: 5
      max_exp: 10
    - name: "--l1d-pht-associativity"  # => [1, 2, 4, 8, 16, 32, 64]
      type: "pow2"
      min_exp: 1
      max_exp: 6
    - name: "--pht-pf-level"  # => [1, 2, 3]
      type: "categorical"
      values: [1, 2, 3]
```

### 3. 运行贝叶斯优化

直接运行以下命令即可：

```bash
python3 bayesianOpt.py configs/example_optimize_sms.yaml
```

### 4. 查看最终优化效果

你可以运行以下命令来检查优化效果：

```bash
python pklReader.py output/Optimize/sms/optimize_checkpoint.pkl
```

你可能会看到如下输出结果：

```bash
ID    score          Parameters
--------------------------------------
0     18.642         [64, 512, 16, 2]
1     18.636         [64, 512, 8, 2]
2     18.635         [64, 512, 32, 2]
3     18.632         [64, 256, 16, 2]
4     18.626         [64, 512, 2, 2]
5     18.617         [64, 512, 4, 2]
...
```

