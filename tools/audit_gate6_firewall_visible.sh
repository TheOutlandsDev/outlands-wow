#!/usr/bin/env bash
set -uo pipefail

readonly STATUS_FILE=/tmp/living-azeroth-gate6-firewall-audit.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate6-firewall-audit.log

clear
printf 'LIVING AZEROTH · READ-ONLY HOST FIREWALL AUDIT\n\n'
printf 'This reads the active UFW, nftables, iptables, IPv6, socket, and Tailscale\n'
printf 'state. It does not add, remove, or edit a firewall or network rule.\n\n'
printf 'Enter your Ubuntu sudo password if prompted.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"

set +e
{
    date --iso-8601=seconds
    sudo ufw status verbose
    sudo nft list ruleset
    sudo iptables-save
    sudo ip6tables-save
    sudo ss -lntup
    tailscale serve status --json
    tailscale funnel status --json
} 2>&1 | tee "${OUTPUT_FILE}"
result=${PIPESTATUS[0]}
set -e

if [[ ${result} -eq 0 ]]; then
    printf 'READY\n' >"${STATUS_FILE}"
    printf '\nREAD-ONLY FIREWALL AUDIT CAPTURED\n'
else
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    printf '\nFIREWALL AUDIT FAILED (exit status %s)\n' "${result}" >&2
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
