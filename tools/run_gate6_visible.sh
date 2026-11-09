#!/usr/bin/env bash
set -uo pipefail

readonly ROOT=/opt/outlands-wow
readonly PYTHON=/opt/outlands-wow/.venv/bin/python3
readonly STATUS_FILE=/tmp/living-azeroth-gate6-validation.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate6-validation.log

clear
printf 'LIVING AZEROTH · GATE 6 SERVER-ONLY VALIDATION\n\n'
printf 'This builds the operational image, starts the private backend twice, creates\n'
printf 'and restore-tests one durable logical backup, and gracefully stops everything.\n'
printf 'Player access, Docker host ports, and Tailscale Serve remain closed.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"

cd "${ROOT}" || exit 1
set +e
"${PYTHON}" tools/gate3_server_controller.py gate6 \
    --backup-root /var/backups/outlands-wow \
    --stable-seconds 30 2>&1 | tee "${OUTPUT_FILE}"
result=${PIPESTATUS[0]}
set -e

if [[ ${result} -eq 0 ]]; then
    printf 'PASS\n' >"${STATUS_FILE}"
    printf '\nGATE 6 SERVER-ONLY VALIDATION PASSED\n'
else
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    printf '\nGATE 6 SERVER-ONLY VALIDATION FAILED (exit status %s)\n' "${result}" >&2
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
