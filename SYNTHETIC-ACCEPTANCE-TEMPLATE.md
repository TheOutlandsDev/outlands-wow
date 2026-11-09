# Synthetic rebuild acceptance record

Copy this template into an owner-private evidence directory for each test run. Fill it from direct observations. Do not commit the filled record, status JSON, SQL dump, logs, secrets, client assets, or host identifiers to the public kit.

## Test identity

| Field | Observation |
| --- | --- |
| UTC date and unique test ID | Pending |
| Kit commit | Pending |
| Pinned core commit | Pending |
| Runtime image digest | Pending |
| Test host class and Python/Docker versions | Pending; keep exact host identity private |
| Evidence directory and hashes | Pending; owner-private only |

## Preconditions

| Check | Result | Evidence |
| --- | --- | --- |
| Four upstream checkouts match documented commits | NOT RUN | |
| Reconstructed source and patch checks complete | NOT RUN | |
| Client assets are owner-controlled and mounted read-only | NOT RUN | |
| Database starts empty and contains no player data | NOT RUN | |
| Test network is isolated and has no published game ports | NOT RUN | |
| Generated secrets are confined to private evidence | NOT RUN | |

## Test results

Use `PASS`, `FAIL`, or `NOT RUN`. Record a failing step as `FAIL`; keep its evidence and exact stopped resources for review. Do not promote a later retry over an earlier failure in the same record.

| Gate | Result | Direct observation and private evidence reference |
| --- | --- | --- |
| Offline build and unit tests | NOT RUN | |
| Synthetic SQL import and logical export | NOT RUN | |
| Auth and world startup with AI disabled | NOT RUN | |
| Stability interval and clean shutdown | NOT RUN | |
| Fresh synthetic logical restore and restart | NOT RUN | |
| Client login through a private route | NOT RUN | |
| Gameplay check | NOT RUN | |
| Enabled AI/playerbots | NOT RUN | |
| Active economy and price catalog source | NOT RUN | |
| Restore on another host | NOT RUN | |

## Data and cleanup

- Synthetic account/character counts before and after: Pending.
- Table counts and dump hash/size: Pending; retain exact values privately.
- No live account, character, chat, or player data used: Unverified.
- Successful run's exact containers, network, volume, and generated secrets removed: Unverified.
- Failed run's exact resources stopped and preserved: Not applicable until a failure occurs.
- Remaining limitations and next test: Pending.

## Decision

Current acceptance scope: **None recorded**. A synthetic `PASS` establishes only the gates directly observed above. This template does not authorize a live realm, public network access, or distribution of third-party assets.
