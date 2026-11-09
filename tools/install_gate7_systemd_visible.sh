#!/usr/bin/env bash
set -uo pipefail

readonly ROOT=/opt/outlands-wow
readonly SOURCE_DIR="${ROOT}/operations/systemd"
readonly TARGET_DIR=/etc/systemd/system
readonly STATUS_FILE=/tmp/living-azeroth-gate7-systemd-install.status
readonly OUTPUT_FILE=/tmp/living-azeroth-gate7-systemd-install.log
readonly UNITS=(
    living-azeroth.service
    living-azeroth-backup.service
    living-azeroth-backup.timer
)

clear
printf 'LIVING AZEROTH · GATE 7 SYSTEMD INSTALL\n\n'
printf 'This installs three reviewed Living Azeroth unit files and reloads systemd.\n'
printf 'It does not start, stop, or enable the server or timer.\n\n'
printf 'Enter your Ubuntu sudo password if prompted.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"
: >"${OUTPUT_FILE}"
chmod 0600 "${OUTPUT_FILE}"

install_units() {
    set -e
    for unit in "${UNITS[@]}"; do
        source_path="${SOURCE_DIR}/${unit}"
        target_path="${TARGET_DIR}/${unit}"
        [[ -f "${source_path}" && ! -L "${source_path}" ]] || {
            printf 'Missing or unsafe staged unit: %s\n' "${source_path}" >&2
            return 1
        }
        if sudo test -e "${target_path}" && ! sudo cmp -s "${source_path}" "${target_path}"; then
            printf 'Refusing to overwrite a different existing unit: %s\n' "${target_path}" >&2
            return 1
        fi
    done
    sudo systemd-analyze verify \
        "${SOURCE_DIR}/living-azeroth.service" \
        "${SOURCE_DIR}/living-azeroth-backup.service" \
        "${SOURCE_DIR}/living-azeroth-backup.timer"
    for unit in "${UNITS[@]}"; do
        sudo install -o root -g root -m 0644 \
            "${SOURCE_DIR}/${unit}" "${TARGET_DIR}/${unit}"
    done
    sudo systemctl daemon-reload
    systemctl show -p LoadState -p UnitFileState "${UNITS[@]}"
}

set +e
install_units 2>&1 | tee "${OUTPUT_FILE}"
result=${PIPESTATUS[0]}
set -e

if [[ ${result} -eq 0 ]]; then
    printf 'READY\n' >"${STATUS_FILE}"
    printf '\nGATE 7 SYSTEMD FILES INSTALLED (NOT ENABLED OR STARTED)\n'
else
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    printf '\nGATE 7 SYSTEMD INSTALL FAILED (exit status %s)\n' "${result}" >&2
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
