#!/usr/bin/env bash
set -Eeuo pipefail

readonly ROOT=/opt/outlands-wow
readonly PYTHON=/opt/outlands-wow/.venv/bin/python3
readonly CONTROLLER="${ROOT}/tools/gate3_server_controller.py"
readonly STATUS_FILE=/tmp/living-azeroth-gate7-validation.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate7-validation.log
readonly STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
readonly EVIDENCE_DIR="${ROOT}/run/private/gate7/${STAMP}-production-hardening"
validation_passed=0

mkdir -p "${EVIDENCE_DIR}"
chmod 0700 "${ROOT}/run/private/gate7" "${EVIDENCE_DIR}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"
printf 'RUNNING\n' >"${STATUS_FILE}"

cleanup_on_failure() {
    result=$?
    trap - EXIT
    if [[ ${validation_passed} -eq 0 ]]; then
        printf '\nGate 7 failed; requesting a fail-closed controller stop.\n' | tee -a "${OUTPUT_FILE}"
        "${PYTHON}" "${CONTROLLER}" stop >>"${OUTPUT_FILE}" 2>&1 || true
        sudo systemctl stop living-azeroth.service >>"${OUTPUT_FILE}" 2>&1 || true
        printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    fi
    exit "${result}"
}
trap cleanup_on_failure EXIT

clear
printf 'LIVING AZEROTH · GATE 7 PRODUCTION HARDENING\n\n' | tee "${OUTPUT_FILE}"
printf 'This performs one controlled server-only systemd boot, one validated live\n' | tee -a "${OUTPUT_FILE}"
printf 'backup with retention, a graceful service stop, and a clean second boot.\n' | tee -a "${OUTPUT_FILE}"
printf 'It then returns the server to explicit Tailscale-only player access.\n\n' | tee -a "${OUTPUT_FILE}"
printf 'No player should log in during this validation.\n' | tee -a "${OUTPUT_FILE}"
printf 'Enter your Ubuntu sudo password if prompted.\n\n' | tee -a "${OUTPUT_FILE}"

cd "${ROOT}"
sudo -v

printf '[1/9] Verify the current explicit player-access runtime\n' | tee -a "${OUTPUT_FILE}"
"${PYTHON}" tools/verify_gate7_runtime_status.py player-access \
    --output "${EVIDENCE_DIR}/initial-player-access.json" | tee -a "${OUTPUT_FILE}"

printf '[2/9] Gracefully stop the current runtime\n' | tee -a "${OUTPUT_FILE}"
"${PYTHON}" "${CONTROLLER}" stop | tee -a "${OUTPUT_FILE}"
"${PYTHON}" tools/verify_gate7_runtime_status.py stopped \
    --output "${EVIDENCE_DIR}/first-stop.json" | tee -a "${OUTPUT_FILE}"

printf '[3/9] Start through systemd in fail-closed server-only mode\n' | tee -a "${OUTPUT_FILE}"
sudo systemctl start living-azeroth.service
sleep 15
"${PYTHON}" tools/verify_gate7_runtime_status.py server-only \
    --output "${EVIDENCE_DIR}/first-server-only-boot.json" | tee -a "${OUTPUT_FILE}"

printf '[4/9] Create and restore-test a live backup, then apply retention 14\n' | tee -a "${OUTPUT_FILE}"
"${PYTHON}" "${CONTROLLER}" backup --retention-count 14 | tee -a "${OUTPUT_FILE}"
cp "${ROOT}/run/private/gate6/latest-status.json" "${EVIDENCE_DIR}/backup-status.json"
chmod 0600 "${EVIDENCE_DIR}/backup-status.json"

printf '[5/9] Stop the systemd-managed boot gracefully\n' | tee -a "${OUTPUT_FILE}"
sudo systemctl stop living-azeroth.service
"${PYTHON}" tools/verify_gate7_runtime_status.py stopped \
    --output "${EVIDENCE_DIR}/systemd-stop.json" | tee -a "${OUTPUT_FILE}"

printf '[6/9] Start a second server-only boot through systemd\n' | tee -a "${OUTPUT_FILE}"
sudo systemctl start living-azeroth.service
sleep 15
"${PYTHON}" tools/verify_gate7_runtime_status.py server-only \
    --output "${EVIDENCE_DIR}/second-server-only-boot.json" | tee -a "${OUTPUT_FILE}"

printf '[7/9] Return to explicit Tailscale-only player access\n' | tee -a "${OUTPUT_FILE}"
"${PYTHON}" "${CONTROLLER}" restart --player-access | tee -a "${OUTPUT_FILE}"
"${PYTHON}" tools/verify_gate7_runtime_status.py player-access \
    --output "${EVIDENCE_DIR}/final-player-access.json" | tee -a "${OUTPUT_FILE}"

printf '[8/9] Enable the validated boot service and backup timer\n' | tee -a "${OUTPUT_FILE}"
sudo systemctl enable living-azeroth.service living-azeroth-backup.timer
sudo systemctl start living-azeroth-backup.timer
systemctl is-enabled living-azeroth.service living-azeroth-backup.timer | tee -a "${OUTPUT_FILE}"
systemctl is-active living-azeroth.service living-azeroth-backup.timer | tee -a "${OUTPUT_FILE}"
systemctl list-timers living-azeroth-backup.timer --no-pager | tee -a "${OUTPUT_FILE}"

printf '[9/9] Record bounded service state\n' | tee -a "${OUTPUT_FILE}"
systemctl show living-azeroth.service living-azeroth-backup.timer \
    -p ActiveState -p SubState -p UnitFileState -p FragmentPath -p User -p Group \
    >"${EVIDENCE_DIR}/systemd-status.txt"
chmod 0600 "${EVIDENCE_DIR}/systemd-status.txt"

validation_passed=1
printf 'PASS\n' >"${STATUS_FILE}"
printf '\nGATE 7 LOCAL VALIDATION PASSED\n' | tee -a "${OUTPUT_FILE}"
printf 'The server is running with Tailscale-only player access.\n' | tee -a "${OUTPUT_FILE}"
printf 'Raw evidence: %s\n' "${EVIDENCE_DIR}" | tee -a "${OUTPUT_FILE}"

trap - EXIT
printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
