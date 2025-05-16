import yaml
import os
import pathlib
from dataclasses import dataclass
from skopt.space import Dimension, Categorical, Integer, Real


@dataclass
class ArchParamConfig:
    """
    Class to hold arch arch configuration parameters
    """
    arch_name: str
    script_path: str
    script_params: list[str]


@dataclass
class WorkloadConfig:
    """
    Class to hold workload configuration parameters
    """
    workload_name: str
    cpt_path_list: list[str]


@dataclass
class EnvironmentConfig:
    """
    Class to hold environment configuration parameters
    """
    gem5_home: str
    bin_home: str
    gem5_data_proc_home: str
    restorer: dict[str, str]
    ref_so: dict[str, str]


@dataclass
class RunningConfig:
    """
    Class to hold running configuration parameters
    """
    gem5_bin: str
    max_proc_per_server: int
    output_base_dir: str
    resume: bool


@dataclass
class OptimizationConfig:
    """
    Class to hold optimization configuration parameters
    """
    constant_params: list[str]
    param_space: list[Dimension]


def pow2range(min_power, max_power):
    """Create a list of powers of two from 2^min_power to 2^max_power."""
    return [2**i for i in range(min_power, max_power+1)]


def parse_param_space(param: dict):
    """
    Parse a parameter space definition into a skopt.space Dimension object.
    
    Args:
        param: Dictionary with parameter space definition including 'name', 'type',
               and type-specific values
    
    Returns:
        A skopt.space Dimension object representing the parameter space
    
    Raises:
        ValueError: If an unsupported parameter space type is provided or required parameters are missing
        TypeError: If parameters have incorrect types
    """
    if not isinstance(param, dict):
        raise TypeError(f"Parameter definition must be a dictionary, got {type(param).__name__}")
    
    if "name" not in param:
        raise ValueError("Parameter definition missing required 'name' field")
    
    if "type" not in param:
        raise ValueError("Parameter definition missing required 'type' field")
    
    name = param["name"]
    if not isinstance(name, str):
        raise TypeError(f"Parameter name must be a string, got {type(name).__name__}")
        
    space_type = param["type"].lower()

    if space_type == "integer":
        if "min_int" not in param:
            raise ValueError(f"Integer parameter '{name}' missing required 'min_int' field")
        if "max_int" not in param:
            raise ValueError(f"Integer parameter '{name}' missing required 'max_int' field")
        
        min_val = param["min_int"]
        max_val = param["max_int"]

        if not isinstance(min_val, int):
            raise TypeError(f"'min_int' for parameter '{name}' must be an integer, got {type(min_val).__name__}")
        if not isinstance(max_val, int):
            raise TypeError(f"'max_int' for parameter '{name}' must be an integer, got {type(max_val).__name__}")
            
        if min_val > max_val:
            raise ValueError(f"'min_int' ({min_val}) must be less than or equal to 'max_int' ({max_val}) for parameter '{name}'")
            
        return Integer(min_val, max_val, name=name)
        
    elif space_type == "float":

        if "min_float" not in param:
            raise ValueError(f"Float parameter '{name}' missing required 'min_float' field")
        if "max_float" not in param:
            raise ValueError(f"Float parameter '{name}' missing required 'max_float' field")
            
        min_val = param["min_float"]
        max_val = param["max_float"]
        
        if not isinstance(min_val, (int, float)):
            raise TypeError(f"'min_float' for parameter '{name}' must be a number, got {type(min_val).__name__}")
        if not isinstance(max_val, (int, float)):
            raise TypeError(f"'max_float' for parameter '{name}' must be a number, got {type(max_val).__name__}")
            
        if min_val > max_val:
            raise ValueError(f"'min_float' ({min_val}) must be less than or equal to 'max_float' ({max_val}) for parameter '{name}'")
            
        return Real(min_val, max_val, name=name)
        
    elif space_type == "pow2":

        if "min_exp" not in param:
            raise ValueError(f"Pow2 parameter '{name}' missing required 'min_exp' field")
        if "max_exp" not in param:
            raise ValueError(f"Pow2 parameter '{name}' missing required 'max_exp' field")
            
        min_exp = param["min_exp"]
        max_exp = param["max_exp"]
        
        if not isinstance(min_exp, int):
            raise TypeError(f"'min_exp' for parameter '{name}' must be an integer, got {type(min_exp).__name__}")
        if not isinstance(max_exp, int):
            raise TypeError(f"'max_exp' for parameter '{name}' must be an integer, got {type(max_exp).__name__}")
            
        if min_exp > max_exp:
            raise ValueError(f"'min_exp' ({min_exp}) must be less than or equal to 'max_exp' ({max_exp}) for parameter '{name}'")
            
        return Categorical(
            pow2range(min_exp, max_exp),
            name=name
        )
        
    elif space_type == "categorical":

        if "values" not in param:
            raise ValueError(f"Categorical parameter '{name}' missing required 'values' field")
            
        values = param["values"]
        
        if not isinstance(values, list):
            raise TypeError(f"'values' for parameter '{name}' must be a list, got {type(values).__name__}")
            
        if len(values) == 0:
            raise ValueError(f"'values' list for categorical parameter '{name}' cannot be empty")
            
        return Categorical(values, name=name)
        
    elif space_type == "boolean":
        return Categorical([True, False], name=name)
        
    else:
        raise ValueError(f"Unsupported parameter space type: {space_type}")

def load_optimization_config(config_file: str) -> OptimizationConfig:
    """
    Parse optimization configuration from a YAML file.
    
    Args:
        config_file: Path to the YAML configuration file
        
    Returns:
        OptimizationConfig object with parsed configuration
        
    """
    with open(config_file, 'r') as f:
        config = yaml.safe_load(f)

    opt_config = config["optimization"]
    constant_params = opt_config["constant_params"] if "constant_params" in opt_config else []
    return OptimizationConfig(
        constant_params=constant_params,
        param_space=[
            parse_param_space(param)
            for param in opt_config["param_space"]
        ]
    )

def getcpts(workloads_path: str, workload_name: str, weight_threshold: float) -> list:
    """
    Get checkpoint paths for a workload that meet a coverage threshold.

    Args:
        workloads_path: Path to the directory containing workload checkpoints
        workload_name: Name of the workload to find checkpoints for
        weight_threshold: Minimum cumulative weight of checkpoints to include

    Returns:
        List of checkpoint paths sorted by weight (highest first)
    """
    # Find all directories matching the workload pattern
    workload_dirs = [
        str(path) for path in pathlib.Path(workloads_path).glob(workload_name)
        if path.is_dir()
    ]

    # Find all checkpoint files across these directories
    checkpoint_paths = []
    for wl_dir in workload_dirs:
        paths = [
            str(path) for path in pathlib.Path(wl_dir).glob("**/*.zstd")
            if path.is_file()
        ]
        checkpoint_paths.extend(paths)

    # Extract weights from paths and create (weight, path) pairs
    weighted_checkpoints = []
    for path in checkpoint_paths:
        try:
            weight = float(path.split("/")[-1].split("_")[-2])
            weighted_checkpoints.append({"weight": weight, "path": path})
        except (IndexError, ValueError):
            continue  # Skip paths with invalid format

    # Sort checkpoints by weight in descending order
    sorted_checkpoints = sorted(
        weighted_checkpoints, key=lambda x: x["weight"], reverse=True)

    # Select checkpoints up to the weight threshold
    selected_paths = []
    cumulative_weight = 0

    for checkpoint in sorted_checkpoints:
        if cumulative_weight >= weight_threshold:
            break
        selected_paths.append(checkpoint["path"])
        cumulative_weight += checkpoint["weight"]

    return selected_paths

def load_yaml(config_file: str) -> tuple[EnvironmentConfig, RunningConfig, list[WorkloadConfig], list[ArchParamConfig], list[str]]:
    """
    Load configuration from a YAML file.

    Args:
        config_file: Path to the YAML configuration file

    Returns:
        Tuple containing:
            - EnvironmentConfig: Configuration for the environment
            - RunningConfig: Configuration for running parameters
            - List of WorkloadConfig: List of workload configurations
            - List of ArchParamConfig: List of architecture parameter configurations
            - List of server names or IP addresses
    """    
    with open(config_file, 'r') as f:
        config = yaml.safe_load(f)

    env = EnvironmentConfig(
        gem5_home=config["environment"]["gem5_home"],
        bin_home=config["environment"]["bin_home"],
        gem5_data_proc_home=config["environment"]["gem5_data_proc_home"],
        restorer=config["environment"]["restorer"],
        ref_so=config["environment"]["ref_so"]
    )

    run = RunningConfig(
        gem5_bin=os.path.join(env.bin_home, config["running"]["gem5_bin"]),
        output_base_dir=os.path.abspath(config["running"]["output_base_dir"]),
        resume=config["running"]["resume"],
        max_proc_per_server=config["running"]["max_proc_per_server"],
    )
    workload_list = [
        WorkloadConfig(
            workload_name=workload,
            cpt_path_list=getcpts(
                config["workloads"]["workloads_path"],
                workload,
                config["workloads"]["run_weight"])
        )
        for workload in config["workloads"]["workload_list"]
    ]

    arch_list = [
        ArchParamConfig(
            arch_name=arch["name"],
            script_path=os.path.join(env.gem5_home, arch["script_file"]),
            script_params=arch["script_params"] if "script_params" in arch else []
        )
        for arch in config["archs"]
    ]

    server_list = config["servers"]

    return env, run, workload_list, arch_list, server_list

def print_config(config_file: str):
    """
    Print the configuration loaded from a YAML file.

    Args:
        config_file: Path to the YAML configuration file
    """

    # Print the loaded configuration
    env, run, workload_list, arch_list, server_list = load_yaml(config_file)

    # Print the configuration in a formatted way
    def print_header(title, width=80):
        print("\n" + "=" * width)
        print(f" {title} ".center(width, "="))
        print("=" * width)

    print_header("Environment")
    print(f"gem5_home: {env.gem5_home}")
    print(f"bin_home:  {env.bin_home}")
    print(f"gem5_data_proc_home: {env.gem5_data_proc_home}")
    print(f"restorer:")
    print(f"  type: {env.restorer['type']}")
    print(f"  path: {env.restorer['path']}")
    print(f"ref_so:")
    print(f"  type: {env.ref_so['type']}")
    print(f"  path: {env.ref_so['path']}")

    print_header("Running")
    print(f"gem5_bin:            {run.gem5_bin}")
    print(f"max_proc_per_server: {run.max_proc_per_server}")
    print(f"output_base_dir:     {run.output_base_dir}")
    print(f"resume:              {run.resume}")

    print_header("Workloads")
    for i, workload in enumerate(workload_list, 1):
        print(f"Workload #{i}: {workload.workload_name}")
        print("  checkpoints:")
        for path in workload.cpt_path_list:
            print(f"    - {path}")
        print()

    print_header("Architecture Parameters")
    for i, arch in enumerate(arch_list, 1):
        print(f"ArchName #{i}: {arch.arch_name}")
        print(f"  Script: {arch.script_path}")
        print("  Params:")
        for param in arch.script_params:
            print(f"    - {param}")
        print()

    print_header("Servers")
    for i, server in enumerate(server_list, 1):
        print(f"{i}. {server}")
        

    try:
        optConfig = load_optimization_config(config_file=config_file)
        print_header("Optimization")
        print("Constant Parameters:")
        for param in optConfig.constant_params:
            print(f"  - {param}")
        
        print("\nParameter Space:")
        for param in optConfig.param_space:
            print(f"  - {param.name}: {param}")
    except KeyError as e:
        return


if __name__ == "__main__":
    import argparse

    argparse = argparse.ArgumentParser(
        description="Load configuration from a YAML file and print it.")
    argparse.add_argument(
        "config_file",
        type=str,
        help="Path to the YAML configuration file"
    )
    args = argparse.parse_args()
    config_file = args.config_file

    print_config(config_file)
