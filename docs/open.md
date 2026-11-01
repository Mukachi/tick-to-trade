# Open items

Every item a note's Review leaves open, one row each. Each gets one decision: **fix** (a task;
closed by its commit), **limit** (stated where it qualifies a figure; closed) or **parked**
(changes no published conclusion; closed, with what would reopen it). A note does not reach
Measured while an item it raised has no decision.

| # | Item | Note | Touches a published figure | Decision |
|---|---|---|---|---|
| T1 | The adapter learns each instrument's locate from directory updates, which the publisher never replays: a consumer process (arrangement C) must attach before the day's Stock Directory messages (the program's `--wait-consumers`) | 002 | No | Limit (Shep, 31 Oct): stated in `adapter.hpp`; note 004 starts the consumer first |
| T2 | The feed handler's recovery from a snapshot (its note 011) takes its publisher by type; the program (arrangements A and B) needs it to take the strategy's sink, which has the same calls | 002 | No | Fix (Shep, 31 Oct): note 004's D4, built (itch-feed-handler d41cae3, pinned here; S2's loss recovered through the sink); closed |
| T3 | Kernel drops over the day at 50,000 packets a second: arrangement C ~59,500 (`itchfh`, both runs, 0.6% of datagrams), A and B 0 to 1,948 a run; every one recovered by re-requests (S1 and S6 hold), cause not found. Note 005 measures tick to trade at 50k | 004 | Not yet (note 005's runs will) | Parked (Shep, 31 Oct): no figure published from these runs; reopened by note 005's runs, which explain it or state it before any figure at 50k is published. Closed (1 Nov, note 005's D5): no kernel drop in any bench-mode point (five loopback runs, 50k included); found outside bench mode only |
| T4 | Journal A (the pipeline, `fdatasync` on ext4 over the P440ar) at 25k: tick to trade max 4.1, 4.2 and 77 ms in the three loopback runs, all in the gateway-and-venue stage; p99.9 174-194 µs; cause not found (the run files keep percentiles only) | 005 | Yes: journal A's max, published as measured with this item | Parked (Shep, 1 Nov): journal A is beside the headline, not the default; reopened if journal A is proposed as a default, by trials keeping their files and tracing the journal thread and the disk |
