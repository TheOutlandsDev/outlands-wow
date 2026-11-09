#!/usr/bin/env bash
set -uo pipefail

readonly ROOT=/opt/outlands-wow
readonly PYTHON=/opt/outlands-wow/.venv/bin/python3
readonly STATUS_FILE=/tmp/living-azeroth-gate6-soak.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate6-soak.log

clear
printf 'LIVING AZEROTH · TEN-MINUTE TAILSCALE ACCESS SOAK\n\n'
printf 'This observer checks the three containers, persistent volumes, database health,\n'
printf 'exact Tailscale TCP routes, Funnel state, and empty Docker host bindings.\n'
printf 'It has no start, stop, backup, firewall, or shell authority.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"

cd "${ROOT}" || exit 1
set +e
"${PYTHON}" tools/observe_gate6_soak.py --duration 600 --interval 30 \
    2>&1 | tee "${OUTPUT_FILE}"
result=${PIPESTATUS[0]}
set -e

if [[ ${result} -eq 0 ]]; then
    printf 'PASS\n' >"${STATUS_FILE}"
else
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
