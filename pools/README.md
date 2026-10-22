# Pools

Object pools the simulated implements show on a terminal (docs/PLAN.md phase 3).

| File | What | Origin |
|---|---|---|
| `base.iop` | The stack's seeder example screen: a run screen with six section toggles and an auto/manual button, statistics, settings and alarms pages, two alarm masks (no speed, no task controller) | AgIsoStack++ `examples/seeder_example/BasePool.iop`, MIT, unchanged; its object IDs in `base_pool_ids.hpp` |

Every shipped profile uses `base.iop` for now, the sprayers included, so a terminal under test has a
pool with real behaviour behind it. The machines' own pools (SEEDER.md and SPRAYER.md section 6)
come after, from AgIsoTerminalDesigner. Any other `.iop` named in a profile is loaded and shown
without behaviour: keys and values still come out as events.
