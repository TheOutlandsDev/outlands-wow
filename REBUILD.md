# Rebuild guide

1. Fetch the exact revisions in [UPSTREAM-PINS.md](UPSTREAM-PINS.md) and apply the four reconstruction patches. The Ollama revision is a compatible reconstruction pin, not a proven original import revision. Verify each upstream license.
2. Add the three `mod-living-azeroth-*` directories under the fork's `modules/`. Review their config templates and SQL migrations. The example seller account, character and GUID are placeholders; create new identities only in an isolated test realm.
3. Apply `patches/third-party-local-changes.patch` with `git apply -p2` after reconstructing the pinned upstream components. Retain upstream notices and assess the GPL/AGPL terms before combining modules.
4. Inspect `deploy/` and `tools/` as examples. The original deployment controller has historical gate names and needs configuration for a new host, data source, image digests, backup, and network identity. Zero-filled digests and `example-*` values intentionally fail closed. Do not run lifecycle or restore commands against an existing service without a fresh review.
5. Build the backend out of source, then run offline unit tests. For the database check, use the pinned core checkout and an owner-private evidence directory outside this repository:

   ```sh
   python3 tools/run_gate2_synthetic.py \
     --core-source /path/to/pinned/azerothcore-wotlk \
     --evidence-dir /path/to/private/new-gate2-evidence
   ```

   The script imports the public auth, character, and world base SQL, applies the eight public custom character migrations, inserts one synthetic account and character, and verifies a logical export and restore. It creates a unique MySQL 8.4 container with `--network none`, no published ports, and a disposable volume. On PASS it removes that exact container and volume; on FAIL it stops and preserves them for inspection. Never copy a live MySQL data directory or use live account data for this check. The historical private-dump Gate 2 helper was removed from the current kit. This synthetic check does not produce the private promotion manifest expected by the historical Gate 3 controller or authorize a live realm start.
6. For a server-only synthetic start, build `deploy/Dockerfile.gate1` from the reconstructed source, then run `tools/run_gate3_synthetic.py`. Provide an owner-controlled client asset Docker volume containing `dbc`, `maps`, `vmaps`, `mmaps`, and `Cameras`; the script mounts it read-only. Choose an unused private IPv4 test subnet. Set `KIT`, `CORE`, `IMAGE`, `ASSET_VOLUME`, `TEST_SUBNET`, and `PRIVATE_DIR` to your own locations and values before running:

   ```sh
   docker build --target gate1 \
     --build-arg SOURCE_REVISION=ceeb3116ebedf4b35f12b75e5481b6ddd0de7a89 \
     -f "$KIT/deploy/Dockerfile.gate1" -t "$IMAGE" "$CORE"
   python3 "$KIT/tools/run_gate3_synthetic.py" \
     --core-source "$CORE" --runtime-image "$IMAGE" \
     --client-assets-volume "$ASSET_VOLUME" --test-subnet "$TEST_SUBNET" \
     --evidence-dir "$PRIVATE_DIR/gate3"
   ```

   The script imports 439 pinned core base files, 94 post-base core updates, 35 playerbots base files, eight custom character migrations, and six Ollama chat character migrations into fresh MySQL 8.4. It inserts only a synthetic account and character, saves a private logical dump, renders the disabled AI profile, and checks auth/world readiness and a short stability interval. It uses a temporary internal Docker network and publishes no host ports. On PASS it removes its own containers, network, volume, and generated secrets. On FAIL it stops and preserves new resources and evidence. The default preflight requires the client asset volume to have no existing container references; `--allow-stopped-asset-ref` can be repeated for exact prior synthetic world containers after inspecting their stopped state. Never use this option for an active or unrelated container.

7. Rehearse a fresh restore with the synthetic dump and status from step 6. Use a new private evidence directory and an unused test subnet; if the prior run passed and removed its network, the same subnet can be reused:

   ```sh
   python3 "$KIT/tools/run_gate4_restore.py" \
     --core-source "$CORE" --runtime-image "$IMAGE" \
     --client-assets-volume "$ASSET_VOLUME" --test-subnet "$TEST_SUBNET" \
     --source-status "$PRIVATE_DIR/gate3/status.json" \
     --source-dump "$PRIVATE_DIR/gate3/synthetic-handoff.sql" \
     --evidence-dir "$PRIVATE_DIR/gate4"
   ```

   This verifies the dump hash and size, restores it into a new MySQL volume, checks synthetic table and identity counts, starts auth/world with the AI profile disabled, then checks stability and clean shutdown. The dump and receipts remain owner-private and must not be committed. The scripts were exercised on one host with the pinned reconstructed source; they do not establish a restore on a different host.
8. Only after server and restore checks pass, conduct a narrow client test through a verified private route. Keep Funnel off and game ports closed until then. Capture a new known-good checkpoint and review logs, resource use, firewall, backup restoration, and access before any live promotion. The active economy's price catalog source remains unresolved; enabled AI/playerbots and active economy have not been accepted.

The scripts use Python 3.12 or newer, Bash, Docker, systemd, MySQL 8.4 and a Linux AzerothCore build. The launcher is for Windows. Replace every sample path and host identity with values chosen for the future deployment. Runtime data and third-party game assets must be obtained and handled separately.
