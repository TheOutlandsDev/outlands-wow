# Rebuild guide

1. Fetch the exact revisions in [UPSTREAM-PINS.md](UPSTREAM-PINS.md) and apply the four reconstruction patches. The Ollama revision is a compatible reconstruction pin, not a proven original import revision. Verify each upstream license.
2. Add the three `mod-living-azeroth-*` directories under the fork's `modules/`. Review their config templates and SQL migrations. The example seller account, character and GUID are placeholders; create new identities only in an isolated test realm.
3. Apply `patches/third-party-local-changes.patch` with `git apply -p2` after reconstructing the pinned upstream components. Retain upstream notices and assess the GPL/AGPL terms before combining modules.
4. Inspect `deploy/` and `tools/` as examples. The original deployment controller has historical gate names and needs configuration for a new host, data source, image digests, backup, and network identity. Zero-filled digests and `example-*` values intentionally fail closed. Do not run lifecycle or restore commands against an existing service without a fresh review.
5. Build the backend out of source, then run offline unit tests and SQL checks. Use disposable database volumes and synthetic accounts. Never copy a live MySQL data directory.
6. Validate a server-only start, graceful stop, recovery, and a restorable logical backup. Keep MySQL private. Only after those gates pass, conduct a narrow client test through a verified private Tailscale route. Keep Funnel off and game ports closed until then.
7. Capture a new known-good checkpoint and review logs, resource use, firewall, backup restoration, and access before any live promotion.

The scripts use Python 3.12 or newer, Bash, Docker, systemd, MySQL 8.4 and a Linux AzerothCore build. The launcher is for Windows. Replace every sample path and host identity with values chosen for the future deployment. Runtime data and third-party game assets must be obtained and handled separately.
