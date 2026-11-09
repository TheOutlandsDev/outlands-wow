# Upstream pins and source reconstruction

These are the upstream commits used for the November 2026 offline reconstruction. The three Playerbots/Auction pins align closely with the archived import. The Ollama pin is a compatible reconstruction point, **not** a proven original import revision. The four patches in `patches/upstream-to-import-*.patch` transform those commits into the imported source used for this kit. They contain source changes, not player data or server configuration. Keep each upstream notice and license with its source.

| Component | Repository | Commit |
| --- | --- | --- |
| Server core | `mod-playerbots/azerothcore-wotlk` | `ceeb3116ebedf4b35f12b75e5481b6ddd0de7a89` |
| Playerbots | `mod-playerbots/mod-playerbots` | `3fa1c1e49f8f1324b72461e576bce7c89b0a6521` |
| Auction bot | `azerothcore/mod-ah-bot` | `a680cc1c98290713e9b3d3289544af78e5186dc1` |
| Ollama chat | `DustinHendrickson/mod-ollama-chat` | `8ba5e791f0a84ee04636f0b19b62d3c4aff3dce1` |

From a clean directory, with this kit checked out as `$KIT`, reconstruct source as follows. Run `git apply --check` before each corresponding `git apply`; stop on any mismatch.

```sh
git clone https://github.com/mod-playerbots/azerothcore-wotlk.git core
git -C core checkout ceeb3116ebedf4b35f12b75e5481b6ddd0de7a89
git -C core apply --check "$KIT/patches/upstream-to-import-core.patch"
git -C core apply "$KIT/patches/upstream-to-import-core.patch"

git clone https://github.com/mod-playerbots/mod-playerbots.git core/modules/mod-playerbots
git -C core/modules/mod-playerbots checkout 3fa1c1e49f8f1324b72461e576bce7c89b0a6521
git -C core/modules/mod-playerbots apply --check "$KIT/patches/upstream-to-import-mod-playerbots.patch"
git -C core/modules/mod-playerbots apply "$KIT/patches/upstream-to-import-mod-playerbots.patch"

git clone https://github.com/azerothcore/mod-ah-bot.git core/modules/mod-ah-bot
git -C core/modules/mod-ah-bot checkout a680cc1c98290713e9b3d3289544af78e5186dc1
git -C core/modules/mod-ah-bot apply --check "$KIT/patches/upstream-to-import-mod-ah-bot.patch"
git -C core/modules/mod-ah-bot apply "$KIT/patches/upstream-to-import-mod-ah-bot.patch"

git clone https://github.com/DustinHendrickson/mod-ollama-chat.git core/modules/mod-ollama-chat
git -C core/modules/mod-ollama-chat checkout 8ba5e791f0a84ee04636f0b19b62d3c4aff3dce1
git -C core/modules/mod-ollama-chat apply --check "$KIT/patches/upstream-to-import-mod-ollama-chat.patch"
git -C core/modules/mod-ollama-chat apply "$KIT/patches/upstream-to-import-mod-ollama-chat.patch"

cp -a "$KIT"/source/modules/mod-living-azeroth-* core/modules/
git -C core apply --check -p2 "$KIT/patches/third-party-local-changes.patch"
git -C core apply -p2 "$KIT/patches/third-party-local-changes.patch"
```

The core patch omits an unrelated upstream development-skill link and a deleted Recast diff file containing historical author metadata. Those two paths are not game code. The reconstruction was compared against the archived import: 10,007 core files, 1,438 Playerbots files, 28 auction files, and 77 Ollama files matched when the complete private import delta was applied. The public core patch preserves the same game code; its two omitted development paths remain different. The final local patch applies with `-p2` after the modules are in place.

The isolated `deploy/Dockerfile.gate1` build compiled and installed authserver, worldserver, dbimport, and unit tests from this reconstructed code. CTest reported 1/1 passed. Later synthetic server startup and logical restore rehearsals passed with the disabled AI profile and private client assets mounted read-only. They did not validate client login, gameplay, enabled AI/playerbots, active economy, or a new host. See [REBUILD.md](REBUILD.md) for the bounded procedures and [THIRD_PARTY.md](THIRD_PARTY.md) for rights and notice boundaries.
