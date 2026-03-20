# yaml Config File Format Specification
[简体中文](README_CN.md) [English](README.md)

This configuration file defines the environment and settings required for running gem5 simulations. It is organized into multiple sections that control environment paths, execution behavior, workload definitions, architecture configurations, server allocation, and optional optimization settings.

## Top-Level Structure

```yaml
environment:         # [Required] gem5 environment setup
running:             # [Required] Run control parameters
workloads:           # [Required] Workload list and path
archs:               # [Required] Architecture configurations
servers:             # [Required] Server pool for parallel execution
optimization:        # [Optional] Parameter optimization space (Only Necessary in bayesianOpt.py)
```

---

## 1. `environment` Section [Required]

Defines paths and shared objects necessary to run gem5 simulations.

| Field       | Type   | Description                                      |
| ----------- | ------ | ------------------------------------------------ |
| `gem5_home` | string | Path to the gem5 root directory                  |
| `bin_home`  | string | Path to directory containing simulation binaries |
| `gem5_data_proc_home` | string | Path to repo [gem5_data_proc](https://github.com/shinezyy/gem5_data_proc) |
| `restorer`  | object | Configuration for checkpoint restoration tool    |
| `ref_so`    | object | Configuration for the reference shared object    |

### Example

```yaml
environment:
  gem5_home: "/nfs/home/$(whoami)/repos/gem5"
  bin_home:  "/nfs/home/$(whoami)/repos/gem5/build/RISCV"
  gem5_data_proc_home: "/nfs/home/$(whoami)/repos/gem5_data_proc"
  restorer:
    type: "GCB_RESTORER"
    path: ""  # if checkpoint has included restorer, leave it empty
  ref_so: 
    type: "GCBV_REF_SO"
    path: "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
```

See detailed type of `restorer` and `ref_so` info here:
👉 [https://github.com/OpenXiangShan/GEM5/blob/xs-dev/README.md#environment-variables](https://github.com/OpenXiangShan/GEM5/blob/xs-dev/README.md#environment-variables)

---

## 2. `running` Section [Required]

Controls how gem5 simulations are launched and managed.

| Field                 | Type    | Description                                   |
| --------------------- | ------- | --------------------------------------------- |
| `gem5_bin`            | string  | gem5 executable name (relative to `bin_home`) |
| `output_base_dir`     | string  | Base directory for storing simulation output  |
| `resume`              | boolean | Whether to resume from existing checkpoints   |
| `max_proc_per_server` | int     | Max number of processes per server            |

### Example

```yaml
running:
  gem5_bin: "gem5.opt"
  output_base_dir: "/nfs/home/$(whoami)/gem5_output"
  resume: false
  max_proc_per_server: 4
```

### Note
The real path of `gem5_bin` in this example is::

```bash
/nfs/home/$(whoami)/repos/gem5/build/RISCV/gem5.opt
```

---

## 3. `workloads` Section [Required]

Defines the list of benchmarks and the directory containing checkpoint files.

| Field            | Type         | Description                                |
| ---------------- | ------------ | ------------------------------------------ |
| `workloads_path` | string       | Absolute path to checkpoint directory      |
| `run_weight`     | float (0-1)  | The least sum of checkpoints weight to run |
| `workload_list`  | list[string] | List of benchmark names to simulate        |

### Example

```yaml
workloads:
  workloads_path: "/nfs/home/$(whoami)/gem5_checkpoints"
  run_weight: 0.5
  workload_list:
    - "workload1"
    - "workload2"
    - "workload3"
```

### Note
The checkpoint can be in any subdirectory under `workloads_path`, and the script will search for them recursively.

The checkpoint files should be named in the format `.*_\d+_\d\.\d+.zstd`, where:
- The first part is not important (e.g., `foo`, `bar`, even empty).
- The second part is an integer (e.g., `100000000`, `200000000`, etc.).
- The third part is a float which is represent it's weight (e.g., `0.25`, `0.5`, etc.).

The Tree of workloads should look like this:

```bash
/nfs/home/$(whoami)/gem5_checkpoints
├── workload1
│   ├── checkpoint1
│   │   ├── foo_100000000_0.25.zstd
│   ├── checkpoint2
│   │   └── bar_200000000_0.5.zstd
|   ...
├── workload2
│   ├── checkpoint1
│   │   └── baz_300000000_0.75.zstd
|   ...
...
```

---

## 4. `archs` Section [Required]

Specifies different architecture configurations used during simulation runs.

| Field           | Type         | Description                                                      |
| --------------- | ------------ | ---------------------------------------------------------------- |
| `name`          | string       | Unique identifier for the architecture                           |
| `script_file`   | string       | Path to the gem5 config script to run  (relative to `gem5_home`) |
| `script_params` | list[string] | Command-line arguments for the script                            |

### Example

```yaml
archs:
  - name: "example-config-kmh_v3"
    script_file: "configs/example/xiangshan.py"
    script_params:
      - "--ideal-kmhv3"
      
  - name: "example-config-kmh_v2"
    script_file: "configs/example/kmh.py"
    script_params:
      - "--kmh-align"
```

### Note
The `script_file` should be relative to the `gem5_home` path. The real path of `scripts_file` of `example-config-kmh_v3` is:

```bash
/nfs/home/$(whoami)/repos/gem5/configs/example/xiangshan.py
```

The `script_params` are passed directly to the gem5 script. You can use any command-line arguments that the script supports.

---

## 5. `servers` Section [Required]

Defines the list of servers that can be used for running simulations in parallel.

| Field     | Type         | Description                                                   |
| --------- | ------------ | ------------------------------------------------------------- |
| `servers` | list[string] | List of server hostnames. Comment out to disable temporarily. |

### Example

```yaml
servers:
  servers:
    - "localhost"
    - "server1"
    - "192.168.1.123"
    # - "server2" # Temporarily disable this server
    - "server3"
```

---

## 6. `optimization` Section [Optional]

Specifies a search space for parameter tuning. This section is optional(`Only Necessary in bayesianOpt.py`).

| Field             | Type         | Description                                         |
| ----------------- | ------------ | --------------------------------------------------- |
| `constant_params` | list[string] | Parameters that stay fixed in all optimization runs |
| `param_space`     | object[]     | Definitions of tunable parameters                   |

### Supported Parameter Types

| Type          | Required Fields          | Description                             |
| ------------- | ------------------------ | --------------------------------------- |
| `categorical` | `values`                 | Discrete set of string values           |
| `pow2`        | `min_exp`, `max_exp`     | Exponent range, generates values as 2^x |
| `float`       | `min_float`, `max_float` | Continuous float range                  |
| `integer`     | `min_int`, `max_int`     | Integer value range                     |
| `boolean`     | *(no extra fields)*      | Boolean values (true/false)             |

### Example

```yaml
optimization:
  constant_params:
    - "--ideal-kmhv3"
    - "--l1d-enable-pht"
  param_space:
    - name: "--example-categorical"
      type: "categorical"            
      values: ["value1", "value2"]
    - name: "--example-pow2"
      type: "pow2"                   
      min_exp: 2                     
      max_exp: 6                     
    - name: "--example-continuous-float"
      type: "float"
      min_float: 1.0
      max_float: 10.0
    - name: "--example-continuous-integer"
      type: "integer"
      min_int: 1
      max_int: 10
    - name: "--example-boolean"
      type: "boolean"
```

### Note
The `constant_params` are passed directly to the gem5 script.

The `param_space` defines the parameters that will be optimized. 

The `name` field is the command-line argument that will be passed to the gem5 script.

When doing Bayesian optimization, parameters would be passed to script in the following format:

```bash
--ideal-kmhv3 --l1d-enable-pht --example-categorical=value1 --example-pow2=4 --example-continuous-float=5.0 --example-continuous-integer=5 --example-boolean=True
```

---

## Tips and Best Practices

* Use **absolute paths** for file and directory references whenever possible.
* Lists such as `workload_list`, `archs`, and `servers` are fully customizable and extensible.
* Use `run_weight` to sample a subset of large checkpint sets of workloads.
* The `optimization` section can be excluded if not using bayesianOpt.py.
* Ensure that the `script_file` paths are correct and accessible from the machine running the script.
* For large-scale simulations, consider using a server pool to distribute the workload.

