#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this installer with sudo." >&2
  exit 2
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dashboard_source="${repo_root}/operations/observability/living-azeroth-operations.json"
rules_source="${repo_root}/operations/observability/living-azeroth-alerts.yml"
service_source="${repo_root}/operations/systemd/living-azeroth-observability.service"
timer_source="${repo_root}/operations/systemd/living-azeroth-observability.timer"
dashboard_target="/opt/outlands-wow/observability/grafana/command-center/living-azeroth-operations.json"
rules_target="/opt/outlands-wow/observability/prometheus/rules/living-azeroth-alerts.yml"

jq empty "${dashboard_source}"
docker run --rm --network none \
  --mount "type=bind,source=${rules_source},target=/rules.yml,readonly" \
  --entrypoint promtool \
  prom/prometheus:v3.14.0-distroless check rules /rules.yml

install -o wowserver -g wowserver -m 0644 "${dashboard_source}" "${dashboard_target}"
install -o wowserver -g wowserver -m 0644 "${rules_source}" "${rules_target}"
install -o root -g root -m 0644 "${service_source}" /etc/systemd/system/living-azeroth-observability.service
install -o root -g root -m 0644 "${timer_source}" /etc/systemd/system/living-azeroth-observability.timer

systemctl daemon-reload
systemctl enable --now living-azeroth-observability.timer
systemctl start living-azeroth-observability.service

docker exec observability-prometheus-1 promtool check rules /etc/prometheus/rules/living-azeroth-alerts.yml
docker kill --signal HUP observability-prometheus-1 >/dev/null

systemctl --no-pager --full status living-azeroth-observability.timer living-azeroth-observability.service || true
echo "Living Azeroth observability is installed. Grafana will discover the dashboard without a restart."
