# xs-gem5-bayesian-scripts
[简体中文](README_CN.md) [English](README.md)

this repository contains scripts for optimizing xiangshan gem5 params using Bayesian optimization. 
The scripts are designed to work with the [xs-gem5](https://github.com/OpenXiangShan/GEM5) repository.

## Files
- `bayesianOpt.py`: This script is used to perform Bayesian optimization.
- `checkrun.py`: This script is used to check the status of simulations.
- `config.py`: This script is used to convert yaml file to running config.
- `remote.py`: This script is used to run simulations on remote servers.
- `runGem5.py`: This script is used to run gem5 simulations.
- `requirements.txt`: This file contains the required Python packages for the scripts.
- `configs/template.yaml`: This is an example configuration file for the scripts.

## Usage
### 1. Expose Parameters
First, modify the gem5 code to expose all the parameters that need to be trained. For example, expose `act_entries` as the script parameter `--l1d-act-entries` (this may require modifying files such as `$GEM5_HOME/src/mem/sms.cc`, `$GEM5_HOME/src/mem/PrefetcherCofig.py`, `$GEM5_HOME/configs/common/PrefetcherConfig.py`, etc.). Please make the specific modifications according to the parameters you want to train.

### 2. Construct the `configs.yaml` file.
You should provide a yaml file which specifies all configurations for running gem5. We provide an template file in `configs/template.yaml`. You can modify it according to your needs. The details of the yaml file format can be found in the [configs/README.md](configs/README.md) file.

### 3. Run normal gem5 simulation
if you just want to run normal gem5 simulation, you can run the following command:
```bash
python3 runGem5.py configs/your_config.yaml
```

### 4. Run Bayesian optimization
if you want to run Bayesian optimization, you can run the following command:
```bash
python3 bayesianOpt.py configs/your_config.yaml
```

## Example

This is an example for optimize prefetcher SMS's parameters(`act_entries`, `pht_entries`, `pht_assoc`, `pht_pf_level`).

### 1. Expose Parameters

You need exposes `act_entries`, `pht_entries`, `pht_assoc`, `pht_pf_level` as the script parameters `--l1d-act-entries`, `--l1d-pht-entries`, `--l1d_pht_associativity`, `--l1d-pht-pf-level` respectively. The details of the modifications can be found in this branch [OpenXiangshan/gem5:example-optimize-SMS-params](https://github.com/OpenXiangShan/GEM5/tree/example-optimize-SMS-params). Then you need recompile gem5.

```sh
cd $GEM5_HOME
git checkout origin/example-optimize-SMS-params
scons build/RISCV/gem5.opt -j8
cp build/RISCV/gem5.opt $BIN_HOME/gem5.create_a_tag_by_yourself
```

### 2. Construct the `optimize_sms.yaml` file.

an example yaml file is provided in `configs/example_optimize_sms.yaml`. 

**Attention**: You must modify the `environment`, `gem5_bin`, `workload_path` to fit your environment. The `gem5_bin` should be the name of gem5 binary you compiled.

```yaml
environment:
  gem5_home: "" # replace with your gem5 home
  bin_home:  "" # replace with your directory where gem5 binary is located
  gem5_data_proc_home: "" # [gem5_data_proc](https://github.com/shinezyy/gem5_data_proc)
  restorer:
    type: "GCB_RESTORER"
    path: ""  # if checkpoint has included restorer, leave it empty
  ref_so: 
    type: "GCBV_REF_SO"
    path: "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"

running:
  gem5_bin: "" # replace with your gem5 binary
  output_base_dir: "./output/Optimize/sms"
  resume: false
  max_proc_per_server: 4

workloads:     
  workloads_path: "" # replace with your directory where workloads are located
  run_weight: 0.5
  workload_list:
    # SPEC CPU2006 INT
    - "perlbench_checkspam"
    - "perlbench_diffmail"
    - "perlbench_splitmail"
...
```

Section `optimization` is the most important part, if you want to optimize the parameters, you need to specify the parameters and their ranges in this section. For this example, we want to optimize `act_entries`, `pht_entries`, `pht_assoc`, `pht_pf_level`. The ranges are as follows:

```yaml
optimization:
  constant_params:
    - "--ideal-kmhv3"
    - "--l1d-enable-pht"
  param_space:
    - name: "--l1d-act-entries" # => [16,32, 64]
      type: "pow2"
      min_exp: 4
      max_exp: 6
    - name: "--l1d-pht-entries" # => [32, 64, 128, 256, 512, 1024]
      type: "pow2"
      min_exp: 5
      max_exp: 10
    - name: "--l1d-pht-associativity" # => [1, 2, 4, 8, 16, 32, 64]
      type: "pow2"
      min_exp: 1
      max_exp: 6
    - name: "--pht-pf-level"  # => [1, 2, 3]
      type: "categorical"
      values: [1, 2, 3]
```

### 3. Run Bayesian optimization
Just run the following command:
```bash
python3 bayesianOpt.py configs/example_optimize_sms.yaml
```
### 4. Check the status of simulations
You can run the following command to check the status of simulations:
```bash
python pklReader.py output/Optimize/sms/optimize_checkpoint.pkl
```

you may get result like this
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





