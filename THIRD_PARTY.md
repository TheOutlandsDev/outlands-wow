# Third-party sources and notices

This kit was derived from a local import of the Playerbots AzerothCore ecosystem. It does not mirror the upstream source trees.

- [Playerbots AzerothCore fork](https://github.com/mod-playerbots/azerothcore-wotlk): base server. The imported source contained a GNU GPL version 2 license; its text is retained at `source/LICENSE`.
- [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots): bot module. Its imported GPL version 2 license is retained at `third_party_licenses/mod-playerbots-LICENSE`.
- [mod-ah-bot](https://github.com/azerothcore/mod-ah-bot): auction module; fetch and inspect its current notices separately.
- [mod-ollama-chat](https://github.com/DustinHendrickson/mod-ollama-chat): bot chat module. Its imported GNU AGPL version 3 license is retained at `third_party_licenses/mod-ollama-chat-LICENSE`. The imported README's license description differed from its LICENSE file; use the license file and obtain a rights review before redistributing a combined binary.

The patches contain changes to these third-party components. [UPSTREAM-PINS.md](UPSTREAM-PINS.md) records the compatible reconstruction revisions; the Ollama original revision remains unproven. Public searches for safe custom naming combinations returned no matching implementation, which does not prove authorship or rights. Preserve all upstream notices when reconstructing the system. No new license is assigned here.
