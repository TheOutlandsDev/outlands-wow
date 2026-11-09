# Living Azeroth faction guilds and rankings

This additive AzerothCore module separates the old shared-guild prototype into
two native faction guilds and provides an owned, persistent ranking projection.

It ships disabled and observe-only. It never enables cross-faction guilds.
Native membership/rank writes require explicit enrollment/rank switches and a
world configuration with `AllowTwoSide.Interaction.Guild = 0`.

Version 1 ranks deterministic durable progression:

`level * 100 + rewarded quests * 20 + honorable kills * 50`

Rank zero is reserved for the guild master. Earned ranks are 1 through 9.
Points change no combat statistic, item, currency, permission, or guild-bank
limit. The migration owns only `la_guild_*` objects and contains no native guild
or character-table DML.

The runtime first projects every character into `la_guild_character` in bounded
batches. Humans in an existing external guild are never moved. Native changes
require observe-only to be disabled, cross-faction guild interaction to be off,
and a separate action switch. Missing-guild creation is also separately gated
and can only be performed by the exact configured leader name/GUID for that
faction. An existing target guild whose leader does not match that identity is
blocked rather than adopted.
