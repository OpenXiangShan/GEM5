# 检查指定文件夹下所有子文件夹的 stdout 文件是否有包含"because a thread reached the max instruction count"的行

import os
import sys
import re
from tqdm import tqdm
import argparse

def check_run(path) -> tuple[int, int, int, int]:
    """
    Check the running status of all checkpoints in the specified directory

    Args:
        path (str): The path to the directory to check.
    
    Returns:
        tuple: A tuple containing the number of completed runs, the number of errors, the total number of runs, and a list of error paths.
    """

    complete  = 0
    error = 0
    total = 0
    error_list = []
    for root, dirs, files in os.walk(path):
        if len(dirs) == 0:
            total += 1
            if "simout" in files and "simerr" in files:
                simout_path = os.path.join(root, "simout")
                simerr_path = os.path.join(root, "simerr")
                with open(simout_path, "r") as simout_file, open(simerr_path, "r") as simerr_file:
                    simout = simout_file.read()
                    simerr = simerr_file.read()

                if any(re.search(pattern, simout) for pattern in [
                                "because a thread reached the max instruction count",
                                "because m5_exit instruction encountered when simulating XS"
                            ]):
                    complete += 1
                elif any(re.search(pattern, simerr) for pattern in [
                            "Program abort at tick",
                            "Failed to execute default signal handler!",
                            "gem5 has encountered a segmentation fault!",
                            "error: ambiguous option:",
                        ]):
                    error_list.append(root)
                    error += 1
                else:
                    # print(f"Unknown error in {root}")
                    pass
            else:
                error_list.append(root)
                error += 1
    return complete, error, total, error_list

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", help="DIR for check running status.")

    args = parser.parse_args()
    complete, error, total, error_list = check_run(args.dir)

    for e in error_list:
        print(f"Error Path: {e}")

    print(f"Complete: {complete}/{total}")
    print(f"Error: {error}/{total}")
    print(f"Success rate: {complete/total*100:.2f}%")