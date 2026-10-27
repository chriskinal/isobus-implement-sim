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
| `fault resume` | Not an implement command after all: it is a terminal reboot the implement never noticed, so the scenario restarts the terminal within the client's timeout instead (phase 4 record) | The VT server gap espVT found |
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
runs the profiles against AOG-TaskController headless on `vcan0`, in CI and in the OrbStack
machine: 4ede403 at first, its `develop` head 4af9d6f since 2026-10-22. Results, all on
`vcan0`:

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
  Raised as AOG-TaskController #112.
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
  controller received; the current stack parses the same bytes cleanly. Until its pin moved,
  the scenarios built AOG-TaskController against the stack this repository fetches
  (`-DFETCHCONTENT_SOURCE_DIR_ISOBUS`). Raised there as #110 before us and fixed on `develop`,
  which CI builds against its own pin since 2026-10-22 (see the upstream filings under
  phase 4).
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

**Phase 3 record (2026-10-22).** Done against a headless terminal, and in the evening against
AgIsoVirtualTerminal on a PC over a real bus (the PEAK adapter on the Mac, hardware note below).
`src/vt_client.cpp` loads the
profile's pool, uploads it under a version label hashed from its bytes, and shows the stack's
seeder screen with behaviour when the pool is `pools/base.iop` (MIT, shipped unchanged with its
ID header): six section toggles, the auto/manual button, speed, the statistics numbers, the
no-speed and no-task-controller alarms after the profile's seconds, acknowledged by the
terminal's key number 0 or the acknowledge soft key. Any other `.iop` is shown as it is, keys
and values still coming out as events. Commands `alarm`, `key`, `mask`, `value` and the `vt`,
`mask`, `key`, `value` and `alarm` events. `tools/vt_stub` is a VT version 3 server on the
stack's server base, 480 x 480, pools in memory, driven from stdin; `tests/scenario_vt.py`
runs the planter against it on `vcan0`, in CI too. Every profile shows `base.iop` until the
machines' own pools exist (SEEDER.md and SPRAYER.md section 6). Mask changes go straight to
the client's send, not through the stack's update helper: the helper skips a change when its
tracked active mask, taken from the terminal's 1 Hz status message, already names the mask
asked for, and after an alarm acknowledge that is the stale case; on GitHub's runners the
screen then stayed on the alarm (two CI runs), never in the OrbStack machine.

**Phase 3 on the bench (2026-10-22, evening).** AgIsoVirtualTerminal on a PC, its adapter and
the Mac's PEAK on one cable, the sprayer with `--driver peak`. First the bus was silent on
both sides for an hour: `tools/can_sniff` (written for this; candump for the stack's drivers,
with a request-for-address-claim probe) heard nothing until the cable was found unplugged.
Then the terminal, at address 0x26, answered every query but one: the stack's client sent
Extended Get Versions (0xD3) because the 16-character hash label the stack's own
`hash_object_pool_to_version` makes is longer than 7, the terminal never answered it, and the
client timed out, reset and tried again forever, never falling back to an upload. With the
label cut to 7 characters the client asks with Get Versions (0xE0), the terminal answers,
the 39 kB pool goes up over ETP in 10 s, "Stored object pool with no error", connected at
11 s, the terminal reporting VT version 6 (the event said 4 that evening: the enum's index,
fixed since). Seen from the PC: the no-speed alarm 10 s later, acknowledged with
the soft key from the terminal (the key event came back), `speed 12` clearing it, sections
switched by command, the settings mask and back, the no-task-controller alarm. From the
terminal's side, confirmed by the operator at the PC and matched by the events: both alarms
seen and acknowledged there, the statistics selection changed, auto/manual toggled (auto with
no task controller took every section off, manual brought the switches back), and single
sections toggled from the screen, each one a `key` event and a changed `sections` event
within the same second. Phase 3's by-hand check is done. Two findings
for upstream, filed 2026-10-22: AgIsoVirtualTerminal 1.6.0 does not answer Extended Get
Versions because its stack pin (1a037ee, 2026-10-07) predates the stack's #768 that added the
command to server and client,
[AgIsoVirtualTerminal #266](https://github.com/Open-Agriculture/AgIsoVirtualTerminal/issues/266);
and the stack's client has no fallback when the versions query goes unanswered, so every
example client in the stack (same hash label) meets the same wall against any terminal
without the command, [AgIsoStack++ #799](https://github.com/Open-Agriculture/AgIsoStack-plus-plus/issues/799). `implement --trace`
prints every frame in and out on stderr, candump style, which is what showed it.

**Phase 4 record (2026-10-22).** Done. `src/fault_driver.cpp` wraps the CAN driver: `fault
silent <s>` drops every outgoing frame, `fault slow <ms>` holds each one, `fault busoff [n]`
writes error frames. `fault restart` stops the terminal client, then the task controller
client, deactivates the control function and rejoins with a fresh address claim; `fault
version <label>` and `fault bigpool <kB>` upload the pool again under another label, padded
with unreferenced number variables; `fault ddop bad` re-uploads a truncated, scrambled DDOP;
`fault conflict [address]` sends an address claim for the task controller's address from a
second identity. `tests/scenario_faults.py` runs them all against AOG-TaskController and
`vt_stub` on `vcan0`, in CI too. What each showed:

- 150 ms on every frame: the sessions survive, section latency grows by about it.
- Silence: AOG-TaskController drops the client after 2.6 s. After 3 s of silence the
  implement carried on and the sections caught up; after 12 s the task controller had
  dropped it, and when the implement resumed, the stack's client never noticed (it watches
  the task controller's status message, which keeps coming) and the task controller ignored
  it: no section followed until a restart. Both sides' finding.
- Restart: the task controller re-registers in 7 to 8 s (the 6 s start-up delay); the
  terminal answers the load-version with the stored pool and the client needs no upload.
- A new version label is stored by the terminal as `v2test ` (padded to 7); the store
  arrives twice. A pool of 240 kB uploads in 30 s on `vcan0`.
- The corrupted DDOP: the stack's client deactivates, deletes, uploads and asks to activate;
  AOG-TaskController logs "Binary device property object has invalid length ... Failed to
  deserialize" and stays up; the client ends disconnected and a restart recovers it.
- Error frames: nobody cares, as expected without an adapter that goes bus-off.
- Conflict: a claim for the task controller's address by another identity changed nothing
  visible; sections followed 0.3 s later. An earlier version of this fault sent the
  implement's own NAME from that address, which the task controller's stack read as the
  implement moving to its own address and lost it: an address change, not a conflict.
- The implement sometimes stopped answering commands after a restart that followed the bad
  DDOP: once in four runs here, every run on both GitHub runners. The backtraces the harness
  takes with `HARNESS_GDB=1` showed the command thread inside the stack's
  `TaskControllerClient::terminate()`, in `pthread_join`, first waiting for the worker thread
  (tid 3663, no longer in the process) and then for a tid of -1, which glibc writes when a
  join has completed and freed the thread's descriptor. Two joins of one thread: when the
  task controller rejects the DDOP, the stack sets Disconnected and calls `terminate()` on
  itself from the PGN callback on the hardware update thread; the harness sees the
  `disconnected` state and sends `fault restart`, which calls `terminate()` too, within the
  50 ms the first join needs for the worker's sleep. Both callers see the worker pointer,
  both join; the kernel wakes one, the other re-reads -1 and waits forever, and the main
  loop waits on `sessionMutex` behind it. A first patch let exactly one caller join and the
  other skip: the implement then died with a segmentation fault instead, the owner destroying
  the client while the stack's own `terminate()` was still inside it. Fixed in
  `cmake/agisostack-restart-races.patch`, applied to the fetched stack by FetchContent's
  patch step: callers of `terminate()` take a mutex around the join, one at a time, so the
  second returns once the first has joined and freed the worker; the worker itself never
  waits there, or it could not exit while the owner joins it. TC and VT client alike. On our
  side `restart()` destroys the old client only after its 300 ms pause.
- With that in, the implement died instead, in one run of two, inside the hardware update
  thread: `process_can_message_for_global_and_partner_callbacks` walking the network
  manager's partner list while `restart()`, in its pause after `deactivate_control_function`,
  had erased the task controller's partner from it. The erase takes `controlFunctionsMutex`;
  the walk, and the one over the internal control functions in
  `process_can_message_for_address_violations`, took nothing. The same patch copies both
  lists under the mutex before walking them, as the stack already does for its global PGN
  callbacks. Both races are the stack's: raised as
  [AgIsoStack++ PR #798](https://github.com/Open-Agriculture/AgIsoStack-plus-plus/pull/798)
  (the patch's two commits, 243 of 243 stack tests passing with them); the patch stays here
  until it lands and the pin moves.

**The `vt_stub` crash in the faults (2026-10-27, fixed).** `scenario_faults.py` failed 2 of 29
now and then, on the OrbStack VM and on CI's arm64 runner: `vt_stub` died just after
`fault busoff 50`, with "pure virtual method called" (SIGABRT) or SIGSEGV, each time just after
the server logged a client's working set maintenance ("version 6 is higher than our reported
version"). It was `vt_stub`'s own race, not the error frames: the stack runs the VT server's
message handlers on the hardware interface's thread, where a client that comes back after
`fault restart` gets a new working set by `emplace_back` on the server's list, while
`vt_stub`'s main thread called the server's `update()` and copied and walked the same list and
the working sets' object trees. The server has no lock. ThreadSanitizer showed both races (the
list's reallocation; an object tree cleared for a pool sent again while it was scanned).
`vt_stub` now does all of it on the stack's thread, as a listener of the hardware interface's
periodic update, and a key's release goes 50 ms later from there instead of a sleep. With the
fix ThreadSanitizer reports nothing over the faults, vt and aux scenarios. Anything else that
uses the stack's VT server outside its thread has the same hazard.

**Upstream filings (2026-10-22).** On AOG-TaskController, from the phase 1, 2 and 4 records:
[#112](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/112) the UDP parser,
[#113](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/113) the pool queue
left behind on client timeout,
[#114](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/114) DDI 156 in
PROTOCOL.md against 397 in the code,
[#115](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/115) two booms,
[#116](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/116) more than 64
sections, [#117](https://github.com/AgOpenGPS-Official/AOG-TaskController/issues/117) the
client that times out and is never taken back. The DataSpan pin was already raised and fixed
there as #110: `develop` (4af9d6f) pins the stack at 1d081d9, past #783. CI checks out that
commit and builds it against its own pin since 2026-10-22; the override is gone.

**Phase 5 record (2026-10-22).** Done. The model's rates follow their target with the
product's time constant, within the machine's limit at the current speed (the planter's
meter above its speed, the sprayer's pump flow), with gaussian noise of the profile's
fraction; the rate is zero whenever nothing is applied and the regulator starts afresh. Row
singulation falls above the knee speed and wanders a little, row by row. Bins drain by the
lagged rate, trip and lifetime totals accumulate, a task controller's zero resets a trip
total. `tools/tc_stub` is a headless task controller on the stack's server base (version 4,
pools parsed at that version) that sends set-value, request and measurement commands from
stdin and reports every value the implement sends; `tests/scenario_rates.py` drives the
sprayer with it on `vcan0`, in CI too: the actual reaches 147 l/ha in 4.3 s (five time
constants), a 200 l/ha setpoint is followed and the pressure goes to 4.7 bar, 900 l/ha is
clipped to the profile's 400, prescription control off hands over to the operator's rate,
and one simulated hectare at 12 km/h gives 10 001 m², 150.0 l applied, the tank down by the
same, with the task controller reading the totals within 2 %. No open task controller sends
a rate yet; this is the tool until one does.

**Phase 6 (2026-10-22).** Not built in this session. The pieces it needs are known: the
stack's clients send a DDOP as process data with the first byte 0x61 and a pool as ECU-to-VT
messages with the first byte 0x11, both over the (extended) transport protocol, and
`build-linux/capture-sprayer.log` (not committed) is a `candump -L` recording of the simulator
against both stubs to test an extractor on. The simulator already takes an extracted pool
with `--pool <file.iop>`.

**Phase 7 record (2026-10-22).** Built as far as a headless terminal allows. The terminal
client reports every auxiliary function event (`aux`: function, input, type, values) for any
pool, and with `--pool pools/aux_functions.iop` (the stack's example pool, MIT) acts on them:
the on/off function is the master, the toggle is auto/manual, plus steps the operator's rate.
`tools/aux_input` is a joystick with a button and a slider on the stack's example input pool,
driven from stdin. `tests/scenario_aux.py` checks that both reach `vt_stub` and that the
joystick takes its inputs; the stack's VT server base has no AUX-N, so the assignment itself,
"made on a terminal, stored and used", is checked by hand on AgIsoVirtualTerminal.

**Phase 7 hardware (2026-10-22).** For the by-hand check, a real stick: an I2C joystick module
(address 0x5A, one stick, buttons A to D and OK, its own microcontroller doing the reading) on
the bench Raspberry Pi CM4, which has the CAN HAT. `aux_input --joystick /dev/i2c-1` reads it
(`tools/i2c_joystick.hpp`), the X axis as the slider and button A as the button; every change
prints a `joystick` event with the raw registers. The register map, first from
github.com/jitsengupta/PicoLibrary `JoystickHandle.py` and confirmed by the vendor's Arduino demo
that came with the board (2026-10-26): X and Y at 0x10 and 0x11, a byte each; the buttons at
0x20 to 0x24 (OK, C, A, B, D) as the last event of each (0 down, 1 up, 2 repeat, 3 click,
4 double click, 5 and 6 long press, 8 nothing). They are events, so a button counts as held on
0, 2, 5 or 6 only; a release reads 1 and then a click before it reads 8. Wiring, from the
module's schematic (nulllaborg/mini-joystick-module, a CH32V003 at 0x5A): no regulator, and the
4.7k I2C pull-ups go to VCC, so on a Pi it is powered from 3.3 V (pin 1), never 5 V; SDA pin 3,
SCL pin 5, ground pin 6. The stick's pots run off the same VCC as the ADC, so the readings do
not depend on the supply. The bench CM4 carries the AOW HAT, which drives GPIO2 and GPIO3 as
outputs (motor direction and watchdog enable) and has every other header GPIO taken, so there
the module hangs off an ESP32 instead (SDA GPIO21, SCL GPIO22, 3V3) running `tools/i2c_bridge`,
a register read over USB serial and nothing else, and `aux_input --joystick-serial
/dev/ttyUSB0` reads it; the register map and button handling stay in `i2c_joystick.hpp`.
Checked against a fake bridge on a pty (handshake, a reset banner mid-stream, button codes,
wrong address). Measured on the module through the bridge (2026-10-26): both axes rest at
exactly 0x80 with no jitter and reach 0 and 255; a press reads 0 while held, a release 1 for
about 300 ms, then 3 (the click) for about 40 ms, then 8, so the old "not 8 is held" would let
go about a third of a second late; a hold reads 0 for a second, then 6 until released, then 1
and 8 with no click. The ESP32 then became SharpIsoTerm's AOW link gateway
(`gateway/esp32`, with a VP230 on its own CAN controller), which keeps the bridge on its serial
port and serves it on TCP port 2324 as well; `--driver aowlink` (any of our programs) reaches
the bus through such a gateway. So `aux_input --driver aowlink --interface
aow-gateway-d430.local --joystick-tcp aow-gateway-d430.local` on the Mac has the stick and the
bus with nothing wired to the Mac: checked 2026-10-27, it read the stick and claimed address 0x80
on the bench bus (seen on the HAT's `hatcan3`). `pools/joystick.iop` (ours, from
`joystick.xml`) is the module as an AUX-N device: both axes as analogue inputs that return to
50 % and the five buttons as non-latching booleans, with its own NAME identity (903) and model
code (2), since the stack's VT server refuses a second pool under the same NAME ("multiple
working set objects"). `scenario_aux.py` checks it on `vt_stub`. The i2c-dev path (`--joystick /dev/i2c-1`) is not yet
run against the module; OrbStack's kernel has no I2C, so only its build and error paths are
checked here. A firmware for a Feather M4 CAN Express was built and ran
(address claimed, the stack on `millis()`) but was set aside for the CM4, which needs no second
codebase. The spreader's pool has AUX-N functions of the joystick's types (SPREADER.md section
4): five buttons as the soft keys and a rate trim for an axis. `vt_stub assign` sends the
assignment a terminal would, so `scenario_aux.py` checks them all through the joystick pool
(30 checks): a step, a reset, a master held past the input's one-second repeat that toggles
once, the trim (2000 to about 2550 kg/ha in a second at full deflection, still when let go),
a stick never moved leaving the rate alone, unassigning. The first press after the first
assignment failed once in five runs on the VM and was not reproduced; the check prints the
stack's AUX-N log if it happens again.

Hand test on the bench (2026-10-27): the real stick on the ESP32 gateway's I2C bridge, by USB
to the CM4 (`--joystick-serial /dev/ttyUSB0`), the spreader and `aux_input` on `hatcan3`. With
AgIsoVirtualTerminal as the VT both pools loaded and the bus carried every input's status
(both axes 0 to 64255, the buttons 0/1), but neither it nor SharpIsoTerm assigns AUX-N (espVT
does, not on the bench). So `vt_stub` was the VT and made the assignments, and the stick worked
every function by hand: A and B stepped the rate 2000 to 2200 and back, Y trimmed it to 2742 and
down to 1982 and held at rest, OK toggled the master off and on, C switched to manual. It found
a bug: the stack keeps its own copy of each input from 0 and takes updates only while connected,
and a stick at rest never moves to send one, so Y went out as 0 (full deflection) and an
assigned trim drained the rate to its floor; `aux_input` now sends every input's value on
connecting. On the bench: `aow-hatd` listens on every USB serial device it has not identified,
cycling the port's rate, so it must be stopped while `aux_input` uses the ESP32's port (or the
stick read over Wi-Fi with `--joystick-tcp`); a client bound to one VT's NAME does not take a
different VT at the same address until restarted; `aux_input` ignores SIGTERM while its stdin
stays open (it waits on the reader). Still to do: the assignment on a terminal that does AUX-N.

**Phase 8 record (2026-10-22).** The packaging is built and `v0.1.0` is cut (2026-10-22,
from 50a4a98, tarballs for x64 and arm64 on the GitHub release). `package.sh` makes `dist/isobus-implement-sim-<version>-linux-<arch>.tar.gz` from a build:
the five binaries, profiles, pools, the scenario harness, the documents and a `RELEASE.md`
with the three lines to start; `.github/workflows/release.yml` runs it on both runners for a
`v*` tag and uploads to the GitHub release. Profile documentation is MODEL.md section 10 and
the profile tables of SEEDER.md and SPRAYER.md. Done when someone outside the project has run
a profile from a tarball. `v0.1.0` went out before phase 6 and the by-hand checks of phases 3
and 7, so that the CI pin move and the stack patch have a release behind them; the next
release follows those.

**Hardware note (2026-10-22).** A PEAK PCAN-USB (0c72:000c, product string "XCAN-USB") on this
Mac: OrbStack's USB passthrough shows it to the Linux machine, but the OrbStack kernel ships
only `can_dev`, `vcan` and `gs_usb`, with no headers to build `peak_usb`, so it gets no `can0`
there; a candleLight-class adapter would. On macOS the stack's MacCAN PCAN driver (UV
Software's PCBUSB library, bundled by the stack under its own EULA) opens it at 250 kbit/s:
`build/implement --driver peak --interface 1`, now the macOS default build. That is the
bench setup for a real terminal or task controller; the by-hand checks of phases 3 and 7 can
use it.

**Spreader (2026-10-26).** From a capture of a real Raven lime spreader that an ISOBUS group
member sent (kept out of the repository, `traces/` is ignored): a third machine type,
`spreader`, with the profile `spreader-9m` (one 9.14 m section, lime, delays 0.2 / 0.02 s as
measured), a profile key `elementNumbers` for DDOPs whose element numbers have gaps, as the
Raven's had (0, 5, 9, 823, 1148), and the project's first pool of its own, `pools/spreader.xml`,
written in PoolEdit's XML and built with PoolEdit's parser (GPL, used as a build tool only,
the `.iop` committed). AOG-TaskController (develop) maps the gapped numbers correctly.
`scenario_vt.py` drives the spreader screens on `vt_stub`, `scenario_tc.py` runs the profile
against AOG-TaskController. Details in SPREADER.md.

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
4. **Pools to ship.** Only pools we made, or the stack's (MIT), never one taken from a capture.
   A designer for new ones: PoolEdit (the spreader's, `pools/README.md`) or
   AgIsoTerminalDesigner.
5. **A web page** to drive it by hand, beside the command line. After phase 2 if people ask.
