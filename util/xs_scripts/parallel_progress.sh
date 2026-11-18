function run_slice() {
    local task="$1" checkpoint="$2" work_dir="$3" status=0
    if [[ -f "$work_dir/completed" ]]; then
        printf '[skip] %s already completed\n' "$task"
        return
    fi

    printf '[start] %s\n' "$task"
    (run "$checkpoint" "$work_dir") >"$work_dir/$log_file" 2>&1 || status=$?
    if [[ -f "$work_dir/completed" ]]; then
        printf '[done] %s\n' "$task"
    else
        printf '[fail] %s log=%s\n' "$task" "$work_dir/$log_file"
    fi
    return "$status"
}

export -f run_slice

function run_parallel_with_progress() (
    set -o pipefail
    printf 'Selected slices: %s\n' "$#"
    printf '%s\n' "$@" |
        parallel --line-buffer -a - -j "$num_threads" arg_wrapper '{}' |
        awk -v total="$#" '
            $1 == "[start]" { $0 = $0 " assigned=" ++assigned "/" total }
            $1 == "[skip]" { ++assigned }
            $1 ~ /^\[(done|fail|skip)\]$/ { $0 = $0 " finished=" ++finished "/" total }
            { print; fflush() }
        '
)
