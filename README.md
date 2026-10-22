# isobus-implement-sim

Simulated ISOBUS implements (sprayers, seeders, planters, spreaders) for developing task
controllers and virtual terminals without owning the machines. Built on
[AgIsoStack++](https://github.com/Open-Agriculture/AgIsoStack-plus-plus), on Linux SocketCAN.

Each implement comes from a profile file (booms, sections, rates, screens), is driven by
commands, reports what the system under test did as events, and can misbehave on demand: drop
off the bus, restart, upload a new pool, answer late.

**Status:** phase 7 of [docs/PLAN.md](docs/PLAN.md), with phase 6 (capture and replay) still
to do: a task controller client and a terminal client built from a profile, driven by
commands, with rates, totals, faults on demand and auxiliary functions. The DDOP comes from the profile's booms,
sections and products; sections follow the task controller's setpoints after the profile's
delay; the profile's pool goes to a terminal, with the stack's seeder screen behind it for now;
commands arrive on stdin or a TCP port and events go out as JSON lines; the implement can go
silent, answer late, restart, re-upload its pool or a bad DDOP, and claim a held address. The
machines are described in [docs/MODEL.md](docs/MODEL.md), [docs/SEEDER.md](docs/SEEDER.md)
and [docs/SPRAYER.md](docs/SPRAYER.md); the shipped profiles are in `profiles/`, the pools in
`pools/`.

## Run

```sh
build/implement --profile profiles/sprayer-24.json --interface vcan0
```

Options: `--driver virtual` for the stack's in-process bus (the only one on macOS),
`--port 0` (or a number) to take commands on a local TCP port as well as stdin, `--pool <file.iop>`
to show another pool than the profile's (one extracted from a capture, or `pools/aux_functions.iop`),
`--section-control setpoint|actualSettable|perElement` to override the profile's section
control method, `--log debug|info|warn|error` for the stack's log on stderr. Events are one
JSON object a line on stdout and to every TCP client, for example:

```
{"t":622,"event":"address","address":128}
{"t":9120,"event":"tc","connected":true,"booms":1,"sections":64,"version":3,"address":247}
{"t":9850,"event":"set","element":2,"ddi":290,"value":1365,"boom":0,"what":"sections","chunk":0}
{"t":10150,"event":"sections","boom":0,"setpoint":"101010101010","commanded":"101010101010","actual":"101010101010"}
```

Commands are one a line, answered with a `command` event: `switch <section> on|off`,
`master on|off [boom]`, `auto on|off [boom]`, `rate <value> [product]`, `bin <level> [product]`
(`tank` on a sprayer), `fill [product]`, `depth <mm>`, `speed <km/h>` or `speed bus`,
`fault row|nozzle <section> blocked|clear`, `alarm <name>` or `alarm clear`, `key <object>`,
`mask <id>`, `value <object> <n>`, `status`, `help`, `quit`; and the faults of PLAN.md
section 7: `fault silent <s>`, `fault slow <ms>`, `fault restart`, `fault version <label>`,
`fault bigpool <kB>`, `fault ddop bad`, `fault conflict [address]`, `fault busoff [frames]`.
Try it by hand:

```sh
build/implement --profile profiles/sprayer-24.json --driver virtual
speed 12
switch 3 off
status
```

`build/ddop_check <file.ddop>` parses a binary DDOP, such as one a task controller saved, and
prints its element tree and any broken child references. `build/vt_stub --interface vcan0` is
a headless virtual terminal for the scenarios: it takes the implement's pool and injects key
presses and values from stdin. `build/tc_stub --interface vcan0` is a headless task controller
that takes any DDOP, sends set-value, request and measurement commands from stdin, and prints
every value the implement reports. `build/aux_input --interface vcan0` is a joystick with a
button and a slider for AUX-N, driven from stdin.

## Releases

A tag `v*` builds release tarballs for Linux x64 and arm64 (`.github/workflows/release.yml`):
the binaries, the profiles, the pools, the scenario harness and the documents, with a
`RELEASE.md` that says how to start. `./package.sh build v0.1.0` makes the same tarball from a
local build.

## Tests

`ctest --test-dir build` runs the model and DDOP tests without a bus.
`tests/scenario_commands.py build/implement` drives one implement over its TCP port on the
virtual driver, on any platform. On Linux with `vcan0`, `tests/scenario_tc.py` runs every
shipped profile against a headless AOG-TaskController and checks that sections follow with the
profile's delay and that the operator's commands override it; `tests/scenario_vt.py` runs the
screens against `vt_stub`; `tests/scenario_rates.py` drives rates and totals through `tc_stub`;
`tests/scenario_faults.py` runs every fault against AOG-TaskController and `vt_stub`;
`tests/harness.py` is the library the scenarios share.

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
