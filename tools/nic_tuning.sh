#!/bin/bash
set -euo pipefail

readonly DATA_CPUS="16-31"
readonly MGMT_CPUS="0-15"
readonly DATA_PORT="9999/udp"
readonly DATA_IFACES=("enP1s25f0" "enP1s25f1" "enP1s25f2" "enP1s25f3")
readonly MGMT_IFACES=("enP1s24f0")

log() {
    echo "[nic-tuning] $*"
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

iface_exists_root() {
    ip link show "$1" >/dev/null 2>&1
}

first_netns_for_iface() {
    local iface="$1"
    local ns
    while read -r ns _; do
        [[ -z "${ns}" ]] && continue
        if ip netns exec "$ns" ip link show "$iface" >/dev/null 2>&1; then
            echo "$ns"
            return 0
        fi
    done < <(ip netns list 2>/dev/null || true)
    return 1
}

run_in_iface_context() {
    local iface="$1"
    shift
    if iface_exists_root "$iface"; then
        "$@"
        return 0
    fi

    local ns
    if ns="$(first_netns_for_iface "$iface")"; then
        ip netns exec "$ns" "$@"
        return 0
    fi

    return 1
}

tune_ring() {
    local iface="$1"
    if run_in_iface_context "$iface" ethtool -G "$iface" rx 4096 tx 4096; then
        log "ring tuned for $iface"
    else
        log "skip ring tuning for $iface: interface not found"
    fi
}

bind_irqs() {
    local iface="$1"
    local cpus="$2"
    local irqs
    irqs="$(grep "$iface" /proc/interrupts | awk '{print $1}' | tr -d ':' || true)"
    if [[ -z "$irqs" ]]; then
        log "no IRQ found for $iface"
        return 0
    fi

    local irq
    for irq in $irqs; do
        if [[ -w "/proc/irq/$irq/smp_affinity_list" ]]; then
            echo "$cpus" >"/proc/irq/$irq/smp_affinity_list"
            log "irq $irq for $iface -> $cpus"
        else
            log "cannot write /proc/irq/$irq/smp_affinity_list"
        fi
    done
}

open_firewall_port() {
    if ! have_cmd firewall-cmd; then
        log "firewall-cmd not found, skip firewalld configuration"
        return 0
    fi

    if ! systemctl is-active --quiet firewalld; then
        log "firewalld inactive, skip firewall configuration"
        return 0
    fi

    if firewall-cmd --quiet --query-port="$DATA_PORT"; then
        log "firewalld already allows $DATA_PORT"
        return 0
    fi

    firewall-cmd --permanent --zone=public --add-port="$DATA_PORT"
    firewall-cmd --reload
    log "firewalld opened $DATA_PORT"
}

main() {
    if ! have_cmd ip || ! have_cmd ethtool; then
        log "required commands missing"
        exit 1
    fi

    open_firewall_port

    local iface
    for iface in "${DATA_IFACES[@]}"; do
        tune_ring "$iface"
        bind_irqs "$iface" "$DATA_CPUS"
    done

    for iface in "${MGMT_IFACES[@]}"; do
        bind_irqs "$iface" "$MGMT_CPUS"
    done

    log "nic tuning complete"
}

main "$@"
