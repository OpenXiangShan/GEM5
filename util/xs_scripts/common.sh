script_dir=$(dirname -- "$( readlink -f -- "$0"; )")

# 兼容脚本位于 util/xs_scripts/ 以及 util/xs_scripts/trace/ 等子目录的情况：
# - 对于 util/xs_scripts/*.sh：     script_dir=.../util/xs_scripts
#   gem5_home = dirname(dirname(script_dir))    -> 仓库根目录
# - 对于 util/xs_scripts/trace/*.sh：script_dir=.../util/xs_scripts/trace
#   gem5_home = dirname(dirname(dirname(script_dir))) -> 仓库根目录
if [ "$(basename "${script_dir}")" = "trace" ]; then
    export gem5_home=$(dirname "$(dirname "$(dirname "${script_dir}")")")
else
    export gem5_home=$(dirname "$(dirname "${script_dir}")")
fi

# Support configurable GEM5 build type via environment variable
# Default to gem5.opt for backward compatibility
export GEM5_BUILD_TYPE=${GEM5_BUILD_TYPE:-opt}
export gem5=$(realpath $gem5_home/build/RISCV/gem5.$GEM5_BUILD_TYPE) # GEM5 executable

echo "Using gem5 binary: $gem5"

function checkForVariable() {
    local env_var=
    env_var=$(declare -p "$1")
    desc=$2
    if !  [[ -v $1 && $env_var =~ ^declare\ -x ]]; then
        echo "$1 environment variable is not defined. $desc"
        exit 1
    else
        var_value=$(echo $env_var | cut -d'=' -f2)
        echo "\$$1 environment variable is $var_value"
    fi
}
