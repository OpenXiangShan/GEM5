import paramiko
from tqdm import tqdm
import argparse

def check_load_and_run(server: str|None, cmd:str, exec:str, max_run_in_server:int) -> bool:
    if server is None:
        server = "localhost"
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        ssh.connect(hostname=server)
        # 执行任务数量
        _, stdout, _ = ssh.exec_command(f"pgrep -c {exec} -u $(whoami)")
        running_num = int(stdout.read().decode().strip())

        #检查负载
        _, stdout, _ = ssh.exec_command("uptime")
        load = float(stdout.read().decode().strip().split(" ")[-2].split(",")[0])

        # 核心数量
        _, stdout, _ = ssh.exec_command(f"nproc")
        cores = int(stdout.read().decode().strip())

        if running_num > max_run_in_server or float(load) > cores/2:
            ssh.close()
            return False

        _, stdout, _ = ssh.exec_command(cmd)
        ssh.close()
        return True
    except Exception as e:
        tqdm.write(f"Connect to {server} failed, error: {e}")
        return False

def kill_all_run(server, exec):
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        ssh.connect(hostname=server)
        # Count the processes before killing them
        _, stdout, _ = ssh.exec_command(f"pgrep -c {exec} -u $(whoami)")
        totalCount = int(stdout.read().decode().strip())
        # Kill the processes
        _, stdout, _ = ssh.exec_command(f"pkill -c {exec} -u $(whoami)")
        # Count the processes after killing them
        killCount = int(stdout.read().decode().strip())
        ssh.close()

        print(f'On {server}, {killCount} Killed ,{totalCount-killCount} Remain')
    except Exception as e:
        tqdm.write(f"Connect to {server} failed, error: {e}")

if __name__ == "__main__":
    # 获取参数
    parser = argparse.ArgumentParser()
    parser.add_argument("-l", "--list",
                        action="store_true",
                        default=False,
                        help="show server name")
    parser.add_argument("-k", "--kill", 
                        action="store_true", 
                        default=False,
                        help="kill all run exec")
    parser.add_argument("-r", "--run",
                        action="store_true",
                        default=False,
                        help="run")
    parser.add_argument("-n", "--num",
                        type=int,
                        action="store",
                        default=1,
                        help="max run in per server")
    parser.add_argument("-s", "--server",
                        nargs="+",
                        help="server name or ip address",
                        default=["localhost"])
    parser.add_argument("-c", "--cmd",
                        nargs="+",
                        default=[""],
                        help="command to run")
    parser.add_argument("-e", "--exec",
                        required=True,
                        help="exec name")
    args = parser.parse_args()

    if args.list:
        print(args.server)
    
    if args.kill:
        print("Kill all run exec: ", args.exec)
        print("Using Server: ", args.server)
        for server in args.server:
            kill_all_run(server, args.exec)
    
    if args.run:
        cmd_str = " ".join(args.cmd)
        print("Run exec: ", args.exec)
        print("Run command: ", cmd_str)
        print("Max run in per server: ", args.num)
        print("Using Server: ", args.server)
        for server in args.server:
            check_load_and_run(server, cmd_str, args.exec, args.num)
    

