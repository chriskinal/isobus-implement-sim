# Plan: ISOBUS implement simulator

> Simulated ISOBUS implements for developing task controllers and virtual terminals without
> owning the machines. Written 2026-10-22. **Nothing here is built.**

## 1. Why

A task controller or terminal is only as good as the implements it has been tried against, and
few developers own any. The open stack (AgIsoStack++) ships one example seeder; beyond that there
are a few expensive commercial simulators. Every open project in this area (AOG-TaskController,
AgOpenWeb-HAT's `aow-hatd` and planned `aow-vtd`, espVT, AgIsoVirtualTerminal) has hit the same
wall.

A simulator also does what a real implement will not: misbehave on demand. Implements drop off the
bus, restart mid-session, upload a new pool, answer late. Those are the cases where task
controllers and terminals break, and they cannot be produced with a real machine on a bench.

What it cannot do is prove that we read the standard the way the manufacturers did. Testing
against real implements still has to happen; the simulator moves it later and makes it shorter.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Stack | AgIsoStack++ at upstream HEAD, fetched by CMake at a pinned commit that is moved forward before each release | Same stack as the projects it serves; its examples already contain a seeder, a section control client and test pools |
| Language | C++17 | The stack's language; the examples port straight in |
| Licence | MIT | Same as the stack and its examples, so GPL and MIT projects alike can use it and contribute back |
| One process | One implement. Several implements: several processes | Each has its own address claim and NAME, as on a real bus; a crash takes down one machine |
| CAN | SocketCAN interface name on the command line | `vcan` for automated tests, a USB adapter on any Linux PC, or a second channel on the same board as the system under test (AgOpenWeb-HAT: CAN3 wired to CAN2) |
| Platforms | Linux first. macOS and Windows through the stack's own drivers later, if asked for | SocketCAN is where the projects it serves run |
| The machine | A profile file (JSON): type, booms, sections, geometry, rates, tank, screens | Most task controller bugs are about geometry; one fixed seeder never finds them |
| Control | A line-based command protocol on stdin and a local TCP port; events out as JSON lines | Tests drive it from any language; a person drives it from a terminal |
| Behaviour | Simple models: section actual follows setpoint after a set delay, rate follows setpoint with noise, tank drains with area and rate, speed from the bus | Enough for the system under test to see a working machine. Not agronomy |
| The stack, patched | Never in place. Gaps go upstream as pull requests | The stack is moving fast and takes fixes quickly |
| Identity on the bus | Manufacturer code 1407 and identity numbers from the profile, as the stack's examples do | A placeholder until open item 1 is settled |
| Conformance | Not claimed. It is a test tool | Its behaviour is our reading of ISO 11783 |

## 3. What exists already

| Piece | Where | What it lacks |
|---|---|---|
| Seeder example: VT client with a real pool (`BasePool.iop`), section control TC client, diagnostics, about 2,600 lines | AgIsoStack++ `examples/seeder_example` (MIT) | One fixed machine; screens tied to that pool; nothing drives it from outside |
| Section control implement simulator (DDOP, process data callbacks) | AgIsoStack++ `examples/task_controller_client` | One boom, fixed widths, no rates |
| A pool load tester, version 3 test pool, auxiliary functions and inputs | AgIsoStack++ `examples/virtual_terminal/` | Separate programs, not one implement |
| A simulated implement and a stand-in terminal, used by probes | AgOpenWeb-HAT `tests/isobus_implement.cpp`, `tests/isobus_vt.cpp` (GPL-3) | Test helpers for one project |
| Terminals to test the screens against | AgIsoVirtualTerminal (GPL-3, desktop), espVT (MIT, ESP32) | |

## 4. Structure

```
profile.json ─┐
              ├─ implement ── CAN (SocketCAN) ── task controller / terminal under test
  commands ───┤     │
  (stdin/TCP) │     ├─ TC client: DDOP from the profile, process data, section and rate models
              │     ├─ VT client: the profile's pool, soft keys and masks, alarms
   events  <──┘     ├─ fault injector: the commands in section 7
                    └─ diagnostics: product and software identification, DM1 on command
```

## 5. Profiles

```json
{
  "name": "Sprayer 24 m",
  "type": "sprayer",
  "identity": 101,
  "booms": [
    { "offsetX": -1.2, "sections": [
      { "width": 2.0, "offsetY": -11.0 }, { "width": 2.0, "offsetY": -9.0 }
    ] }
  ],
  "sectionDelay": { "on": 0.3, "off": 0.2 },
  "rate": { "unit": "l/ha", "default": 150, "noise": 0.02 },
  "tank": { "capacity": 3000, "level": 3000 },
  "pool": "pools/seeder.iop"
}
```

Shipped profiles: the stack's seeder as it is; a 24 m sprayer with 12 sections; a 48-row planter
(one section per row); a spreader with variable width; a two-boom machine. Each one is a test case
for a task controller.

Detailed plans (2026-10-22): the shared model in [MODEL.md](MODEL.md), the machines in
[SEEDER.md](SEEDER.md) and [SPRAYER.md](SPRAYER.md). The two-boom machine is the seeder's
`airseeder-12m`.

## 6. Commands and events

**Commands** (one per line): `switch <section> on|off`, `master on|off`, `auto on|off`,
`rate <value>`, `tank <level>`, `alarm <name>`, `alarm clear`, `key <softkey>`, `mask <id>`,
`value <object> <n>`, `fault <name> [args]` (section 7), `status`, `quit`. Added by MODEL.md:
`speed <km/h>` and `speed bus` for a bench with no tractor ECU; per-machine commands in
SEEDER.md and SPRAYER.md section 7.

**Events** (one JSON object per line): address claimed, TC connected or lost, DDOP accepted or
rejected, section setpoints and actuals, rate setpoint and actual, VT connected, pool uploaded,
mask shown, key pressed on the terminal, value entered on the terminal, alarm acknowledged.
Probes read these to check what the system under test did.

## 7. Faults

| Fault | What happens on the bus | What it tests |
|---|---|---|
| `fault silent <s>` | Stops sending for s seconds, then carries on mid-session | TC and VT timeouts; recovery without a new handshake |
| `fault restart` | Leaves and rejoins: new address claim, maintenance messages with the init bit | Session restart; stored pools and DDOP |
| `fault resume` | Rejoins without the init bit, as after a terminal reboot it missed | The VT server gap espVT found |
| `fault version <label>` | Uploads a changed pool under a new version label | Stored-pool replacement |
| `fault bigpool <kB>` | Pads the pool to a size | Upload timing over ETP; a terminal's memory |
| `fault slow <ms>` | Delays every reply | Timeouts on the other side |
| `fault conflict` | Claims an address already held | Address claim handling |
| `fault ddop bad` | Sends a malformed DDOP | The TC's error path |
| `fault busoff` | Floods error frames on the bus (vcan or an adapter that allows it) | Recovery from bus-off |

Two implements at once is two processes, not a fault.

## 8. Build order

Each phase leaves something that runs and can be checked on `vcan0`.

| Phase | What | Done when |
|---|---|---|
| 0 | **Repository.** CMake fetching the stack at HEAD, CI building on Linux x64 and arm64, the seeder example built and run unchanged | CI is green; the seeder claims an address on `vcan0` |
| 1 | **Task controller client from a profile.** DDOP generated from booms and sections; section setpoint and actual with the delay model | Against AOG-TaskController (or `aow-hatd`): the shipped profiles all connect, and sections follow the setpoints |
| 2 | **Commands and events.** Stdin and TCP; a Python harness that runs a scenario and checks events | The phase 1 checks run as scenarios in CI against a task controller on `vcan0` |
| 3 | **Virtual terminal client.** The seeder's pool from the profile; soft keys, masks, values; alarms on command; any `.iop` loaded and shown, without behaviour | Against AgIsoVirtualTerminal: the pool loads, a key press arrives as an event, an alarm shows |
| 4 | **Faults.** Section 7 | Each fault has a scenario; the known server gaps (espVT's list) show up as failures until fixed upstream |
| 5 | **Rates and the rest of process data.** Rate setpoint and actual, tank, work state, total area | A task controller sending a rate sees the actual follow; totals add up over a scenario |
| 6 | **Capture and replay.** Read a `candump` log of a real implement: extract its pool and DDOP, replay its traffic | A capture from a group member's machine replays against a terminal. Captures stay private: manufacturers' pools are not ours to publish |
| 7 | **Auxiliary functions and inputs (AUX-N).** A simulated joystick and functions to assign | Assignment made on a terminal is stored and used |
| 8 | **Releases.** Binaries for Linux x64 and arm64, profile documentation | Someone outside the project runs a profile against their own TC or terminal from the release alone |

**Phase 0 record (2026-10-22).** Done. CMake fetches the stack at 42ea80e and builds
`examples/seeder_example` unchanged; CI builds on `ubuntu-24.04` and `ubuntu-24.04-arm` and
runs `tests/phase0_vcan.sh`, which sees the example claim address 0x80 on `vcan0`. The arm64
runner's Azure kernel has no `vcan` until `linux-modules-extra` is installed. Locally, macOS
compiles with the stack's virtual driver only; bus checks run in an OrbStack Ubuntu machine
(`isobus`), whose kernel has `vcan`, building into `build-linux/`.

**Phase 1 record (2026-10-22).** Done. `implement --profile <file>` builds the DDOP from the
profile (`src/ddop.cpp`), runs the model (`src/model.cpp`) at 50 Hz and answers the stack's
task controller client (`src/tc_client.cpp`); events are JSON lines on stdout. `test_model`
checks the model and the DDOP of every shipped profile without a bus; `tests/phase1_tc.py`
runs the profiles against AOG-TaskController 4ede403 headless on `vcan0`, in CI and in the
OrbStack machine. Results, all on `vcan0`:

- Every one-boom profile connects in 7 to 9 s and the sections follow the setpoints; the
  measured delays are the profile's within 20 ms (one tick). All three section control methods
  work: AOG-TaskController names the one it chose.
- Speed: AOG-TaskController's tractor ECU takes speed from DDI 397 in PGN 242, though its
  PROTOCOL.md says 156.
- AOG-TaskController's UDP parser (`udp_connections.cpp`, `handle_incoming_packets`) parses
  only the first frame in its buffer: after one frame it moves the rest to the front but keeps
  reading at the old offset, logs "Unknown start of message" and drops it. A frame under
  8 bytes (PGN 241 is 7) is not parsed until the next datagram arrives, and that datagram is
  the one dropped. The harness sends 241 only on change and every 2 s, so the loss is a
  routine PGN 229 and never the speed; on GitHub's runners the loss was every speed frame.
  To raise upstream.
- Section control off from the app sends DDI 160 = 0 about ten times a second, for as long
  as it is off. The implement goes to manual, where the operator's switches rule and default
  on, so every section is on: what a real machine does, and what an app should expect.
- `airseeder-12m` (two booms): the task controller registers 12 sections as one list, sends
  the 12-section condensed value to the second boom's element only, and the first boom never
  moves. It declares one boom; two should be refused or driven separately.
- `sprayer-36-nozzle` (72 sections): registered as 72, driven through the app's 64-bit word,
  so sections 65 to 72 never switch while the heartbeat reports all 72 on. One DDOP upload was
  aborted (ETP, reason 3) and retried in one run of two.
- **AOG-TaskController's DDI-to-element map is read through a broken span.** Its pinned
  AgIsoStack++ (c0e1ebb, 2026-09-19) has the `DataSpan` bug fixed upstream in #783
  (2026-10-18): iterating `DeviceElementObject::get_child_object_ids()` yields twice the
  children, the second half read from whatever follows in memory. The task controller walks
  that list to map each condensed work state DDI to an element, so with larger pools it maps
  some chunks onto garbage elements ("Mapped DDI 162 ... to element 3", a row) and those
  chunks never move; which chunks differs from run to run and from machine to machine.
  `tools/ddop_check` built against that stack shows the garbage on the exact bytes the task
  controller received; the current stack parses the same bytes cleanly. Until its pin moves,
  the scenarios build AOG-TaskController against the stack this repository fetches
  (`-DFETCHCONTENT_SOURCE_DIR_ISOBUS`). To raise upstream, with the pin bump.
- Smaller, from the code: `on_client_timeout` erases the client but not its queue of
  uploaded pool chunks, and `activate_object_pool` parses every queued chunk into one pool.
  A client that leaves by timeout and uploads again would get both pools merged. The
  scenario restarts the task controller for every profile, and runs one back-to-back case
  recorded, not checked.
- The simulator's own departure: on exit it stops the stack without waiting for the
  deactivate exchange, so the task controller drops it by timeout, 4 to 5 s later. A clean
  leave belongs with phase 4's `fault restart`.

**Phase 2 record (2026-10-22).** Done. Commands are lines on stdin and on a TCP port
(`--port 0` picks one and reports it in the `listening` event); every connected client gets
every event, and each command is answered with a `command` event. Built: `switch`, `master`,
`auto`, `rate`, `bin` and `tank`, `fill`, `depth`, `speed`, `fault row|nozzle <n> blocked|clear`,
`status`, `help`, `quit`; the terminal commands come with phase 3, the other faults with phase 4.
`tests/harness.py` is the scenario library; `tests/scenario_commands.py` drives one implement
on the stack's virtual driver with no bus (runs on macOS too), and `tests/scenario_tc.py` is the
phase 1 check as a scenario, with operator overrides through commands. Both run in CI.

## 9. Who uses it

| Project | For |
|---|---|
| AgOpenWeb-HAT `aow-hatd` | The section control task controller (its PLAN section 8); its probes move from `tests/isobus_implement` to this |
| AgOpenWeb-HAT `aow-vtd` | Every phase of the terminal plan (its PLAN_VT) before a real implement is needed |
| AOG-TaskController | The same task controller, on the bench |
| espVT, AgIsoVirtualTerminal | Pools, faults and two-implement cases for their terminal servers |

## 10. Open items

1. **Identity on the bus.** Manufacturer code: 1407 (the stack examples') is a placeholder.
2. **Name and home.** Under the author's account to start. It could belong in Open-Agriculture
   beside the stack; ask them once phase 2 works.
3. **How real is it?** Phase 6 captures are the check: where the simulator and a real machine
   differ, the simulator is wrong.
4. **Pools to ship.** Only pools we made, or the stack's (MIT). A designer for new ones:
   AgIsoTerminalDesigner.
5. **A web page** to drive it by hand, beside the command line. After phase 2 if people ask.
