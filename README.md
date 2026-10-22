# isobus-implement-sim

Simulated ISOBUS implements (sprayers, seeders, planters, spreaders) for developing task
controllers and virtual terminals without owning the machines. Built on
[AgIsoStack++](https://github.com/Open-Agriculture/AgIsoStack-plus-plus), on Linux SocketCAN.

Each implement comes from a profile file (booms, sections, rates, screens), is driven by
commands, reports what the system under test did as events, and can misbehave on demand: drop
off the bus, restart, upload a new pool, answer late.

**Status:** phase 0 of [docs/PLAN.md](docs/PLAN.md): the repository builds AgIsoStack++ at a
pinned commit and runs the stack's seeder example unchanged. No simulator yet. The machines are
planned in [docs/MODEL.md](docs/MODEL.md), [docs/SEEDER.md](docs/SEEDER.md) and
[docs/SPRAYER.md](docs/SPRAYER.md).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # fetches AgIsoStack++
cmake --build build
```

Linux builds the stack with SocketCAN and the virtual driver; macOS with the virtual driver
only, enough to compile and to run a binary with `--driver virtual`. To see the seeder example
claim an address on a virtual bus (what CI checks):

```sh
sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
tests/phase0_vcan.sh build/seeder_example/SeederExample vcan0
```

Not ISO 11783 certified: a test tool, not a machine.

## Licence

MIT. See [LICENSE](LICENSE).
