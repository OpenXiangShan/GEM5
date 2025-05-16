from skopt import gp_minimize
from skopt.utils import use_named_args
import pickle
import os
import re
import copy

# import cust
import runGem5
import config


def power_of_two_range(min_power, max_power):
    """Create a list of powers of two from 2^min_power to 2^max_power."""
    return [2**i for i in range(min_power, max_power+1)]


ENV_CONFIGS: config.EnvironmentConfig
RUN_CONFIGS: config.RunningConfig
WORKLOAD_LIST: list[config.WorkloadConfig]
ARCH_LIST: list[config.ArchParamConfig]
SERVER_LIST: list[str]
OPT_CONFIG: config.OptimizationConfig

# define the object value


def objective_function(**params):
    print(f"\nTry Params:")
    for key, value in params.items():
        print(f"  {key} = {value}")
    # convert params to script params
    script_params = copy.deepcopy(OPT_CONFIG.constant_params)
    for key, value in params.items():
        script_params += [f"{key}={value}"]
    stamp = f"{'_'.join([str(v) for v in params.values()])}"

    run = RUN_CONFIGS

    arch_list = [copy.deepcopy(ARCH_LIST[0])]
    arch_list[0].arch_name = f"config_{stamp}"
    arch_list[0].script_params += script_params


    # issue config to run
    issued_configs = runGem5.issue_archs(
        env=ENV_CONFIGS,
        run=run,
        workload_list=WORKLOAD_LIST,
        arch_list=arch_list,
        server_list=SERVER_LIST,
    )

    # monitor finish
    finished_configs = runGem5.monitor_run_progress(
        issued_configs, RUN_CONFIGS.output_base_dir, 10)

    # compute final score
    score_files = runGem5.calculate_performance_scores(
        finished_configs, RUN_CONFIGS.output_base_dir, ENV_CONFIGS)

    score_file = os.path.join(RUN_CONFIGS.output_base_dir, score_files[0])

    with open(score_file, 'r') as f:
        content = f.read()
        match = re.search(r'Estimated Int score per GHz: ([\d.]+)', content)
        if match:
            score = float(match.group(1))
            print(f"score: {score}")
        else:
            print("no score something error")
            score = 0
    return -score


if __name__ == "__main__":
    n_calls = 50
    n_parallel = 4
    n_initial_points = 10

    import argparse
    parser = argparse.ArgumentParser(
        description="Bayesian Optimization for gem5 configurations"
    )

    parser.add_argument(
        "config_file",
        type=str,
        help="Path to the YAML configuration file"
    )

    args = parser.parse_args()

    config_file = args.config_file

    ENV_CONFIGS, RUN_CONFIGS, WORKLOAD_LIST, ARCH_LIST, SERVER_LIST = config.load_yaml(
        config_file)
    OPT_CONFIG = config.load_optimization_config(config_file)

    output_dir = RUN_CONFIGS.output_base_dir
    result_file = f"{output_dir}/optimize_result.pkl"
    checkpoint_file = f"{output_dir}/optimize_checkpoint.pkl"
    plot_convergence_file = f"{output_dir}/optimization_convergence.png"

    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    if os.path.exists(checkpoint_file):
        print("Loading Bayesian Checkpoints...")
        with open(checkpoint_file, 'rb') as f:
            checkpoint = pickle.load(f)
            start_params = checkpoint['params']
            start_score = checkpoint['score']
    else:
        start_params = None
        start_score = None

    def checkpoint_callback(res):
        with open(checkpoint_file, 'wb') as f:
            pickle.dump({
                'params': res.x_iters,
                'score': res.func_vals
            }, f)
            print(f"Saving Checkpoint to {checkpoint_file}")

    result = gp_minimize(
        func=use_named_args(dimensions=OPT_CONFIG.param_space)(
            objective_function),
        dimensions=OPT_CONFIG.param_space,
        n_calls=n_calls,
        n_initial_points=n_initial_points if start_params is None else 0,
        n_jobs=n_parallel,
        acq_func="LCB",
        x0=start_params,
        y0=start_score,
        random_state=42,
        verbose=True,
        callback=[checkpoint_callback],
    )

    with open(result_file, 'wb') as f:
        pickle.dump(result, f)
        print(f"Saving Result to {result_file}")

    print(f"optimize finish: {result_file}")
    print("best Param")
    for i, param_name in enumerate([dim.name for dim in OPT_CONFIG.param_space]):
        print(f"{param_name}: {result.x[i]}")
