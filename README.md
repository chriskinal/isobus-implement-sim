# isobus-implement-sim

Simulated ISOBUS implements (sprayers, seeders, planters, spreaders) for developing task
controllers and virtual terminals without owning the machines. Built on
[AgIsoStack++](https://github.com/Open-Agriculture/AgIsoStack-plus-plus), on Linux SocketCAN.

Each implement comes from a profile file (booms, sections, rates, screens), is driven by
commands, reports what the system under test did as events, and can misbehave on demand: drop
off the bus, restart, upload a new pool, answer late.

**Status:** phase 1 of [docs/PLAN.md](docs/PLAN.md): a task controller client built from a
profile. The DDOP comes from the profile's booms, sections and products; sections follow the
task controller's setpoints after the profile's delay; events come out as JSON lines. No
terminal client, commands or faults yet. The machines are described in
[docs/MODEL.md](docs/MODEL.md), [docs/SEEDER.md](docs/SEEDER.md) and
[docs/SPRAYER.md](docs/SPRAYER.md); the shipped profiles are in `profiles/`.

## Run

```sh
build/implement --profile profiles/sprayer-24.json --interface vcan0
```

Options: `--driver virtual` for the stack's in-process bus (the only one on macOS),
`--section-control setpoint|actualSettable|perElement` to override the profile's section
control method, `--log debug|info|warn|error` for the stack's log on stderr. Events are one
JSON object a line on stdout, for example:

```
{"t":622,"event":"address","address":128}
{"t":9120,"event":"tc","connected":true,"booms":1,"sections":64,"version":3,"address":247}
{"t":9850,"event":"set","element":2,"ddi":290,"value":1365,"boom":0,"what":"sections","chunk":0}
{"t":10150,"event":"sections","boom":0,"setpoint":"101010101010","commanded":"101010101010","actual":"101010101010"}
```

## Tests

`ctest --test-dir build` runs the model and DDOP tests without a bus. On Linux with `vcan0`,
`tests/phase1_tc.py` runs every shipped profile against a headless AOG-TaskController and
checks that sections follow with the profile's delay.

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
