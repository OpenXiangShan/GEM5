import os
import time
import argparse
from datetime import timedelta
from tqdm import tqdm

# Load custom modules
import remote
import checkrun
import config


def run_cmd(env: config.EnvironmentConfig,
            run: config.RunningConfig,
            workload: config.WorkloadConfig,
            arch: config.ArchParamConfig,
            server_list: list[str]):
    """
    Run gem5 simulation in [server_list] for specified checkpoints with given configuration

    Args:
        env: Configuration parameters for environment
        run: Configuration parameters for execution
        workload: Configuration parameters for workload
        arch: Configuration parameters for arch and script
        server_list: List of server names or IP addresses
    """

    restorer = env.restorer
    ref_so = env.ref_so

    gem5_bin = run.gem5_bin
    max_proc_per_server = run.max_proc_per_server
    output_base_dir = run.output_base_dir
    resume = run.resume

    arch_name = arch.arch_name
    script_path = arch.script_path
    script_params = arch.script_params

    workload_name = workload.workload_name
    cpt_path_list = workload.cpt_path_list

    for cpt in tqdm(cpt_path_list, desc=f"Issuing {workload_name}", leave=False, unit="checkpoint"):
        # Extract identification information from checkpoint path
        inst_num = os.path.basename(cpt).split("_")[-3]
        weight = os.path.basename(cpt).split("_")[-2]
        

        # Set up output directory
        cpt_output_dir = os.path.join(
            output_base_dir, arch_name, f"{workload_name}_{inst_num}_{weight}")
        os.makedirs(cpt_output_dir, exist_ok=True)

        # Skip if output directory exists and simulation is complete
        if resume and checkrun.check_run(cpt_output_dir)[0] == 1:
            tqdm.write(
                f"Skip {cpt_output_dir} because reached max instruction count or m5_exit")
            continue

        # Build the command in sections for better readability
        # Environment variables
        env_setup = [
            # f"export GEM5_HOME={env_config.gem5_home}",
            f"export {restorer['type']}={restorer['path']}",
            f"export {ref_so['type']}={ref_so['path']}"
        ]

        # Directory preparation
        dir_setup = [
            f"mkdir -p {cpt_output_dir}",
            f"cd {cpt_output_dir}"
        ]

        # Gem5 binary and output redirection
        gem5_cmd = [
            gem5_bin,
            "--redirect-stdout",
            "--redirect-stderr",
            script_path,
            f"--generic-rv-cpt={cpt}",
        ] + script_params + ["&"]

        gem5_cmd = " ".join(gem5_cmd)

        # Construct the full command
        cmd_parts = env_setup + dir_setup + [gem5_cmd]
        cmd = "; ".join(cmd_parts)

        # Try to distribute the job until successful
        distribute_ok = False
        # tqdm.write(cmd)
        # exit()
        while not distribute_ok:
            for server in server_list:
                if distribute_ok:
                    tqdm.write(
                        f"Distribute to {server} with cpt dir: {cpt_output_dir}")
                    break

                time.sleep(2)
                distribute_ok = remote.check_load_and_run(
                    server, cmd,
                    os.path.basename(gem5_bin),
                    max_proc_per_server
                )


def issue_archs(env: config.EnvironmentConfig,
                run: config.RunningConfig,
                workload_list: list[config.WorkloadConfig],
                arch_list: list[config.ArchParamConfig],
                server_list: list[str]) -> list[str]:
    """
    Issue all architecture configurations for execution

    Args:
        env: Environment configuration
        run: Running configuration
        workload_list: List of workload configurations
        arch_list: List of architecture configurations
        server_list: List of server names or IP addresses

    Returns:
        List of successfully issued arch config names
    """

    issued_configs = []

    for arch in tqdm(arch_list, desc="Issuing configurations", unit="config"):
        try:
            start_time = time.time()
            # Process each workload for the current configuration
            for workload in tqdm(workload_list, desc=f"Issuing {arch.arch_name}", leave=False, unit="workload"):
                run_cmd(env=env, run=run,
                        workload=workload,
                        arch=arch,
                        server_list=server_list)

            # Report completion time
            elapsed = time.time() - start_time
            elapsed_str = str(timedelta(seconds=int(elapsed)))
            tqdm.write(
                f"✓ Configuration {arch.arch_name} issued in {elapsed_str}")
            issued_configs.append(arch.arch_name)

        except Exception as e:
            tqdm.write(f"! Error Issuing {arch.arch_name}: {e}")

    return issued_configs


def monitor_run_progress(configs: list[str], base_dir: str, check_interval: int = 10):
    """
    Monitor the progress of all running configurations until completion.

    Args:
        configs: List of configurations to monitor
        base_dir: Base directory where outputs are stored
        check_interval: Time between progress checks in seconds

    Returns:
        List of completed configurations
    """
    # Initialize progress tracking for each configuration
    progress_trackers = {}
    finish_configs = set()

    for config_name in configs:
        complete, error, total, _ = checkrun.check_run(
            os.path.join(base_dir, config_name))
        progress_trackers[config_name] = {
            "tracker": tqdm(
                total=total,
                initial=complete+error,
                desc=f"Progress {config_name}",
                unit="checkpoint"
            ),
            "complete": complete,
            "error": error,
            "total": total
        }

    # Monitor progress until all configurations are complete
    while finish_configs != set(configs):
        for config_name in set(configs) - finish_configs:
            # Get updated status
            new_complete, new_error, new_total, _ = checkrun.check_run(
                os.path.join(base_dir, config_name))

            # Update progress bar
            progress = progress_trackers[config_name]
            finished_tasks = new_complete + new_error - \
                progress["complete"] - progress["error"]
            if finished_tasks > 0:
                progress["tracker"].update(finished_tasks)

            # Update stored values
            progress["complete"] = new_complete
            progress["error"] = new_error
            progress["total"] = new_total

            # Check if configuration is complete
            if (new_complete + new_error) == new_total:
                finish_configs.add(config_name)
                tqdm.write(
                    f"✓ Finish: {config_name} | Success: {new_complete}/{new_total} | Errors: {new_error}/{new_total}")

        if finish_configs != set(configs):
            time.sleep(check_interval)

    # Close progress bars
    for progress in progress_trackers.values():
        progress["tracker"].close()

    return list(finish_configs)


def calculate_performance_scores(finish_configs: list[str], base_dir: str, env: config.EnvironmentConfig):
    """
    Calculate performance scores for completed configurations.

    Args:
        finish_configs: List of completed configuration names
        base_dir: Base result directory where configurations are stored

    Returns:
        List of paths to generated score files
    """
    
    score_files = []

    for config_name in finish_configs:
        # Calculate performance score for completed configuration
        config_path = os.path.join(base_dir, config_name)
        score_file = f"{config_path}.score.txt"

        # Prepare score calculation command
        score_cmd = [
            f"export PYTHONPATH={env.gem5_data_proc_home}:$PYTHONPATH",
            f"cd {env.gem5_data_proc_home}",
            f"bash example-scripts/gem5-score-ci.sh {config_path} /nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json > {score_file}"
        ]

        # Execute score calculation
        os.system(" && ".join(score_cmd))
        score_files.append(score_file)

    return score_files


if __name__ == "__main__":
    # Process all configurations

    argparse.ArgumentParser()
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=str, help="Configuration File (yaml)")
    args = parser.parse_args()

    env, run, workload_list, arch_list, server_list = config.load_yaml(
        args.config)

    issued_arch = issue_archs(env=env,
                              run=run,
                              workload_list=workload_list,
                              arch_list=arch_list,
                              server_list=server_list)

    # # Start monitoring and get completed configurations
    finished_arch = monitor_run_progress(
        issued_arch, run.output_base_dir, 10)

    # Calculate performance scores for completed configurations
    score_files = calculate_performance_scores(
        finished_arch,
        run.output_base_dir,
        env)
