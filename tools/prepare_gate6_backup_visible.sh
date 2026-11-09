#!/usr/bin/env bash
set -uo pipefail

readonly BACKUP_ROOT=/var/backups/outlands-wow
readonly STATUS_FILE=/tmp/living-azeroth-gate6-backup-root.status

clear
printf 'LIVING AZEROTH · GATE 6 BACKUP STORAGE\n\n'
printf 'This creates only the dedicated protected directory:\n  %s\n\n' "${BACKUP_ROOT}"
printf 'No game data, database volume, Windows file, or existing backup is removed.\n'
printf 'Enter your Ubuntu sudo password if prompted.\n\n'
printf 'RUNNING\n' >"${STATUS_FILE}"

if sudo install -d -o wowserver -g wowserver -m 0700 "${BACKUP_ROOT}"; then
    stat -c 'Directory: %n%nMode: %a%nOwner: %U:%G' "${BACKUP_ROOT}"
    printf 'READY\n' >"${STATUS_FILE}"
    printf '\nBACKUP STORAGE READY\n'
else
    result=$?
    printf 'FAILED %s\n' "${result}" >"${STATUS_FILE}"
    printf '\nBACKUP STORAGE FAILED (exit status %s)\n' "${result}" >&2
fi

printf '\nLeave this window open and return to Codex.\n'
read -r -p 'Press Enter to close this terminal... '
