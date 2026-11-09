#!/usr/bin/env bash
set -uo pipefail

readonly STATUS_FILE=/tmp/living-azeroth-gate7-host-audit.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate7-host-audit.log

clear
printf 'LIVING AZEROTH · GATE 7 READ-ONLY HOST AUDIT\n\n'
printf 'This reads the protected host backup framework and systemd schedule.\n'
printf 'It does not edit services, timers, backup profiles, or stored data.\n\n'
printf 'Enter your Ubuntu sudo password if prompted.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"

run_audit() {
    set -e
    date --iso-8601=seconds
    printf '\n[service backup implementation]\n'
    sudo sed -n '1,420p' /usr/local/sbin/server-service-runtime-backup
    printf '\n[service profile inventory: paths and modes only]\n'
    sudo find /etc/server-services -maxdepth 2 -type f \
        -printf '%M %u %g %p\n' | sort
    printf '\n[active backup schedule]\n'
    systemctl list-timers --all --no-pager
}

set +e
run_audit 2>&1 | tee "${OUTPUT_FILE}"
result=${PIPESTATUS[0]}
set -e

if [[ ${result} -eq 0 ]]; then
    printf 'READY\n' >"${STATUS_FILE}"
    printf '\nREAD-ONLY HOST AUDIT CAPTURED\n'
else
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    printf '\nHOST AUDIT FAILED (exit status %s)\n' "${result}" >&2
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
