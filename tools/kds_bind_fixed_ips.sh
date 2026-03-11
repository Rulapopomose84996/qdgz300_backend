#!/usr/bin/env bash
set -euo pipefail

# Bind fixed data-plane IPs to the Intel X710 ports on kds.
# Mapping is aligned with config/receiver.yaml:
#   Array Face 1 -> enP1s25f0 -> 192.168.1.101/24
#   Array Face 2 -> enP1s25f1 -> 192.168.2.101/24
#   Array Face 3 -> enP1s25f2 -> 192.168.3.101/24
#
# This script is idempotent and uses `nmcli` for persistence.

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "run as root: sudo bash $0" >&2
        exit 1
    fi
}

bind_one() {
    local iface="$1"
    local conn="qdgz300-${iface}"
    local cidr="$2"

    if nmcli -t -f NAME connection show | grep -Fxq "${conn}"; then
        nmcli connection modify "${conn}" \
            connection.interface-name "${iface}" \
            ipv4.method manual \
            ipv4.addresses "${cidr}" \
            ipv4.gateway "" \
            ipv4.dns "" \
            ipv6.method ignore \
            connection.autoconnect yes
    else
        nmcli connection add type ethernet \
            con-name "${conn}" \
            ifname "${iface}" \
            ipv4.method manual \
            ipv4.addresses "${cidr}" \
            ipv6.method ignore \
            connection.autoconnect yes
    fi

    nmcli connection up "${conn}"
}

require_root

bind_one enP1s25f0 192.168.1.101/24
bind_one enP1s25f1 192.168.2.101/24
bind_one enP1s25f2 192.168.3.101/24

echo
echo "Bound fixed IPs:"
ip -br addr show dev enP1s25f0
ip -br addr show dev enP1s25f1
ip -br addr show dev enP1s25f2
