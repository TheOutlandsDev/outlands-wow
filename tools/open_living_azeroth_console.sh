#!/usr/bin/env bash
set -euo pipefail

readonly ROOT=/opt/outlands-wow
readonly PYTHON=/opt/outlands-wow/.venv/bin/python3
readonly CONTROLLER_LOG="${ROOT}/run/private/gate6/controller.log"

if ! command -v gnome-terminal >/dev/null 2>&1; then
    echo "gnome-terminal is required for the visible Living Azeroth console." >&2
    exit 1
fi

mkdir -p "${ROOT}/run/private/gate6"
touch "${CONTROLLER_LOG}"
chmod 0600 "${CONTROLLER_LOG}"

gnome-terminal --window --title="Living Azeroth · Operator" \
    -- bash -lc "cd '${ROOT}'; exec '${PYTHON}' tools/living_azeroth_console.py"
gnome-terminal --tab --title="Living Azeroth · Status" \
    -- bash -lc "cd '${ROOT}'; exec '${PYTHON}' tools/living_azeroth_status_view.py"
gnome-terminal --tab --title="Living Azeroth · World" \
    -- bash -lc "cd '${ROOT}'; exec '${PYTHON}' tools/follow_living_azeroth_container.py world"
gnome-terminal --tab --title="Living Azeroth · Auth" \
    -- bash -lc "cd '${ROOT}'; exec '${PYTHON}' tools/follow_living_azeroth_container.py auth"
gnome-terminal --tab --title="Living Azeroth · Database" \
    -- bash -lc "cd '${ROOT}'; exec '${PYTHON}' tools/follow_living_azeroth_container.py database"
gnome-terminal --tab --title="Living Azeroth · Python controller" \
    -- bash -lc "cd '${ROOT}'; exec tail -n 200 -F '${CONTROLLER_LOG}'"
