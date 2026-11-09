# Outlands WOW

A source and operations kit for rebuilding the Living Azeroth World of Warcraft 3.3.5a server when its owner chooses to bring it online again. This repository contains custom gameplay modules, SQL schema changes, deployment and lifecycle scripts, observability definitions, a player launcher, and a patch recording local changes to third-party code.

The original running server, its data, and its access configuration are not included. This kit is a source snapshot for future engineering. It is not an installable, tested release. The examples were scrubbed of live host paths, network addresses, account names, runtime fingerprints, and backup identifiers. Rebuild and validate every gate on an isolated machine before opening any game port.

## Downloadable snapshot

The same reviewed source is also available as [a ZIP snapshot](outlands-wow-source-20261109.zip). Verify its SHA-256 using [SHA256SUMS](SHA256SUMS). The files below are browseable in this repository; the ZIP is retained as the original 2026-11-09 snapshot.

## Contents

- `source/modules/mod-living-azeroth-*`: economy, guild, war, and population customizations, config templates, SQL migrations, and offline tests.
- `tools/`: lifecycle controller, backup and restore methods, service supervision, observability, validation tools, and tests.
- `deploy/`: Linux build/runtime Dockerfiles and entrypoints.
- `operations/`: example systemd and monitoring definitions.
- `client/wow-launcher/`: Tailscale-aware Windows player launcher example.
- `patches/third-party-local-changes.patch`: local changes to the imported AzerothCore/Playerbots/Ollama source snapshot.
- `third_party_licenses/` and `source/LICENSE`: retained third-party license texts.

## Rebuild path

See [UPSTREAM-PINS.md](UPSTREAM-PINS.md) for exact source revisions and reconstruction patches, [REBUILD.md](REBUILD.md) for dependencies and staged acceptance, and [THIRD_PARTY.md](THIRD_PARTY.md) before combining or redistributing upstream components.

No game client, Blizzard assets, database content, player or account records, AI prompts or chat, secrets, logs, private Tailscale policy, or live backup was copied here. No license is assigned to the kit as a whole.
