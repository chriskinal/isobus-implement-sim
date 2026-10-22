# isobus-implement-sim

Simulated ISOBUS implements (sprayers, seeders, planters, spreaders) for developing task
controllers and virtual terminals without owning the machines. Built on
[AgIsoStack++](https://github.com/Open-Agriculture/AgIsoStack-plus-plus), on Linux SocketCAN.

Each implement comes from a profile file (booms, sections, rates, screens), is driven by
commands, reports what the system under test did as events, and can misbehave on demand: drop
off the bus, restart, upload a new pool, answer late.

**Status:** planning. Nothing is built yet. See [docs/PLAN.md](docs/PLAN.md).

Not ISO 11783 certified: a test tool, not a machine.

## Licence

MIT. See [LICENSE](LICENSE).
