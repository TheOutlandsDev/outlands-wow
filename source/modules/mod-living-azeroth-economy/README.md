# Living Azeroth Economy

This module owns Living Azeroth's deterministic, seller-only auction actuator.
It is independent from the pinned `mod-ah-bot` source.

The module is disabled by default. Its first backend-only gate can provision
one locked seller identity (`EXAMPLEECONOMY` / `ExampleSeller`) and reconcile a fixed
300-item catalog across three auction houses, producing 900 exact business keys.
A second reconciliation produces zero identity or auction mutations.

The pure planners live in `src/LivingAzerothEconomyPlan.h` and
`src/LivingAzerothEconomyProvisionPlan.h`; their fixtures are under `tests/`.
They have no update or delete intents.

`LivingAzerothProfileCommercePlan.h` adds the next fail-closed contract for
persistent AI profiles to post surplus, tradable inventory at 80-120% of a
validated reference price and to buy useful items within per-profile money,
reserve, daily, and cycle limits. It rejects self-trades, same-account trades,
unowned sellers, human listings by default, unsafe items, price violations,
and duplicate actions. The accompanying actuator only considers independent
owned random-playerbot profiles physically near a native auctioneer. It routes
posts and buyouts through AzerothCore's native session handlers so deposits,
inventory, money, auction removal, mail, and sale hooks use the same transaction
path as players. Daily purchase/spend caps use the existing persistent
Playerbots event store. Human listings remain protected and auctions with an
existing bid are skipped. The actuator is source-staged only and must pass its
offline, synthetic-owned, recovery, and soak gates before activation.
