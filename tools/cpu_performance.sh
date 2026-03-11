#!/bin/bash
set -euo pipefail

readonly DATA_CPUS=($(seq 16 31))
readonly GOVERNOR="performance"

log() {
    echo "[cpu-performance] $*"
}

set_governor() {
    local cpu="$1"
    local governor_file="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
    if [[ ! -w "$governor_file" ]]; then
        log "skip cpu${cpu}: ${governor_file} not writable"
        return 0
    fi

    echo "$GOVERNOR" >"$governor_file"
    log "cpu${cpu} governor -> $GOVERNOR"
}

main() {
    local cpu
    for cpu in "${DATA_CPUS[@]}"; do
        set_governor "$cpu"
    done
}

main "$@"
