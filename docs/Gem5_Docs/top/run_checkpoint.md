# 入门-跑一遍checkpoint

## 安装环境
1. 连接好大机房、小机房，配置好rsa密钥
    1. windows本机下载git bash，生成一份，copy到大机房一个节点就行，由于nfs共享文件系统，其他节点也能直接连接
    2. vscode 远程连接就好
2. 配置代理
    1. 发现主机能ping通大机房节点，反之不行
    2. windows用ipconfig, linux 用ifconfig 查询ip地址
    3. 关闭windows防火墙，大机房也能ping通了
    4. export http_proxy=主机ip:7890
    5. 能流畅在大机房访问github了
3. 安装gem5
    1. 按照REAMDE, git clone gem5, 用默认xs-dev分支
    2. `scons build/RISCV/gem5.opt --gold-linker -j20` 编译  --gold-linker 是多线程链接器，比默认ld 链接器更快
4. 配置各个环境变量, 下载nemu编译nemu，编译gcpt——restorer
    1. nemu 用于difftest比对，restorer 用于切片恢复代码（恢复寄存器和内存）
    2. 暂时用GCBV = GCB, GCB 是指令集名称缩写(G=global=imafd通用指令, C为压缩指令，B为Bit扩展，V为向量扩展）

```bash
export NEMU_HOME=/nfs/home/yanyue/workspace/NEMU

# riscv compiler path
export PATH=/nfs/share/riscv/bin:$PATH

export GCB_REF_SO=$NEMU_HOME/build/riscv64-nemu-interpreter-so
export GCBV_REF_SO=$GCB_REF_SO

export GCB_RESTORER=$NEMU_HOME/resource/gcpt_restore/build/gcpt.bin
export GCBV_RESTORER=$GCB_RESTORER

# parallel path
export PATH=/nfs/home/yanyue/tools/parallel/parallel-20240722/src:$PATH
```

5. 下载parallel
    1. wget **.tar.bz
    2. tar -xzvf **.tar
    3. ./configure && make
    4. 或者用nix-env -iA nixpkgs.parallel 下载安装
6. 单线程运行一个切片
    1. 目前切片位置在`/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc`
    2. 主要使用其中的zstd-checkpoint-0-0, zstd为一种压缩算法，比gzip快，用zstd -d 解压
    3. 内部包含不同程序，每个程序的不同切片文件
    4. 用gem5直接运行一个切片

```c
cd gem5/util/xs_scripts/example
bash ../kmh_6wide.sh /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/astar_biglakes/31/_31_0.016784_.zstd
等价于
command line: /nfs/home/yanyue/workspace/GEM5-internal/build/RISCV/gem5.opt /nfs/home/yanyue/workspace/GEM5-internal/configs/example/xiangshan.py --generic-rv-cpt=/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/astar_biglakes/31/_31_0.016784_.zstd
```

f. 之后会具体怎么跑，如何和nemu来进行对比的？需要详细看看

7. 用多线程跑

```bash
cd gem5/util/xs_scripts/example1

/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc

bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/zstd-checkpoint-0-0-0/checkpoint-0-0-0.lst /nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc yanyue
# 修改了parallel_sim 中线程数为127， 可以改到192的，先跑一下试试吧

```

    1. 这一个文件夹内共60个程序，每个程序有10多个切片，共计1200个程序要跑，每个程序10分钟，假设120个核心，每个核心10个程序，共计1.5小时左右
8. 跑3组切片

```bash
位置在/nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c
分别为checkpoint-0-0-0 checkpoint-0-1-0 checkpoint-0-2-0
cd workspace/GEM5-internal/util/xs_scripts/example1/

export FP_PATH=/nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c
# bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` /nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c/checkpoint-0-0-0/checkpoint-0-0-0.lst /nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c/checkpoint-0-0-0 test0

bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` $FP_PATH/checkpoint-0-0-0/checkpoint-0-0-0.lst $FP_PATH/checkpoint-0-0-0 test0
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` $FP_PATH/checkpoint-0-1-0/checkpoint-0-1-0.lst $FP_PATH/checkpoint-0-1-0 test1
bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` $FP_PATH/checkpoint-0-2-0/checkpoint-0-2-0.lst $FP_PATH/checkpoint-0-2-0 test2
```

10. 这3轮只包含浮点程序，总共438个程序，似乎半小时就能跑完了，现在需要分析数据

```bash

cd /nfs/home/yanyue/workspace/gem5_data_proc
export PYTHONPATH=`pwd`
mkdir -p results
# 指定待测项目
example_stats_dir=/nfs/home/yanyue/workspace/GEM5-internal/util/xs_scripts/example1/test0
batch.py -s $example_stats_dir -t --topdown-raw -o results/example.csv
python3 simpoint_cpt/compute_weighted.py  --fp-only \
-r results/example.csv \
-j /nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c/checkpoint-0-0-0/cluster-0-0.json \
--score results/example-score.csv
# -r 为生成的数据，  -j 为权重信息，  -o 输出结果, -j 需要调整！
mv results results1

```



```bash
git log -p xxx  
可以查看xxx 这个分支对应的细节和时间
```



总结

```bash
golden checkpoints path:
    nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc

test checkpoints path:
    /nfs/home/hebo/test/kunminghu-fp/8ff1bd8e38aaee3a26048c594c815a6c
    checkpoints-0-0-0  , 0-1-0 , 0-2-0

gem5 running results:
    /nfs/home/yanyue/workspace/GEM5-internal/util/xs_scripts/example1/golden
    golden
    test0 test1 test2
    每个文件夹大约5G, 运行一次需要1小时左右

gem5 data stats:
    /nfs/home/yanyue/workspace/gem5_data_proc
    results_golden
    results0
    results1
    results2
```



```bash
cd workspace/GEM5-internal/util/xs_scripts/lbm_check

bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` /nfs/home/hebo/BOSC/Simpoint_Checkpoint/auto_checkpoint/archive/fdc64512e812aaa9f6150087ffdd3054/checkpoint-0-0-0/checkpoint-0-0-0.lst /nfs/home/hebo/BOSC/Simpoint_Checkpoint/auto_checkpoint/archive/fdc64512e812aaa9f6150087ffdd3054/checkpoint-0-0-0 ref0 

bash ../parallel_sim.sh `realpath ../kmh_6wide.sh` /nfs/home/hebo/BOSC/Simpoint_Checkpoint/auto_checkpoint/archive/a48e1767f39f7fd48b940baabfde21d5/checkpoint-0-0-0/checkpoint-0-0-0.lst /nfs/home/hebo/BOSC/Simpoint_Checkpoint/auto_checkpoint/archive/a48e1767f39f7fd48b940baabfde21d5/checkpoint-0-0-0 test0


```

## 代码阅读
1. parallel_sim.sh 脚本

```bash
parallel_sim.sh 脚本 worklist 切片目录  tag

# workload 示例： 名称 地址 跳过指令数（0） warmup(0) detailed_warmup(20) 采样指令(20?)
mcf_9578 mcf/9578 0 0 20 20
mcf_11392 mcf/11392 0 0 20 20

function prepare_env() {
    set -x
    echo "prepare_env $@"
    all_args=("$@")
    task=${all_args[0]}			# mcf_9578
    task_path=${all_args[1]}	# mcf/9578
    suffix="zstd"	
    checkpoint=$(find -L $cpt_dir -wholename "*${task_path}*${suffix}" | head -n 1)
    # 在目录下找模式匹配为mcf/9578 * zstd 的文件 
    echo $checkpoint

    export work_dir=$full_work_dir/$task	# 当前运行输出目录 yanyue/mcf_9578
    echo $work_dir
    mkdir -p $work_dir	#
}

function arg_wrapper() {
    prepare_env $@

    all_args=("$@")
    args=(${all_args[0]})

    skip=${args[2]}  0
    fw=${args[3]}    0
    dw=${args[4]}    20
    sample=${args[5]} 20

    run $checkpoint $work_dir >$work_dir/$log_file 2>&1
    # 运行 *.zstd 当前目录 > log file 且重定向到同一个文件
}
function run() {
    set -x	# 调试模式
    hostname	# 输出主机名字

    cd $2  # work_dir

    if test -f "completed"; then		# 如果存在completed文件，跳过
        echo "Already completed; skip $1"
        return
    fi


    rm -f abort
    rm -f completed

    touch running

    script_dir=$(dirname -- "$( readlink -f -- "$0"; )")
    bash $arch_script $1 # checkpoint	# 执行单线程运行模式
    check $?		# 比较结果

    rm running
    touch completed	# 创建文件完成了！
}

function parallel_run() {
    export num_threads=127
    cat $workload_list | parallel -a - -j $num_threads arg_wrapper {}
    # -a 指定标准输入读取
    # -j 指定127线程
    # 参数解析
}

总体： parallel_run -> arg_wapper -> prepare_env -> run
```
