# Spreader

> A trailed spreader of a dry product (lime, fertilizer) as a simulated implement. Written
> 2026-10-26 from a capture of a real one. What is shared with the seeder and the sprayer is in
> [MODEL.md](MODEL.md); the project in [PLAN.md](PLAN.md).

## 1. What it models

`"type": "spreader"`: one boom of one or more sections, one product in kg/ha from one bin.
Its NAME has device class 5 (fertilizers), its DDOP device element is called `Spreader`, and
the bin's operation type (DDI 179) is 1, fertilizing. Rates, the bin, totals, section delays,
the master and auto/manual are the shared model of MODEL.md: a spreader is the simplest
machine there is.

Not modelled: spinner speed, gate position, belt speed, spread pattern and border spreading.
Each is an open item if a task controller under test needs it.

## 2. Shipped profile

| Profile | Machine | Sections | Width | Bin | Delay on / off | What it tests |
|---|---|---|---|---|---|---|
| `spreader-9m` | 9.14 m (30 ft) trailed lime spreader | 1 | 9.14 m | 8 000 kg | 0.2 / 0.02 s | A one-section machine; element numbers with gaps; its own screens |

Default rate 2 000 kg/ha (250 to 6 000).

## 3. What the capture showed

The profile follows a capture made 2026-10-24 by an ISOBUS group member: a lime spreader with
a Raven controller (one boom, one section, 9.14 m) on AgIsoVirtualTerminal and
AOG-TaskController, and the same machine starting on a Trimble terminal and task controller.
The capture is not in this repository; these are the facts taken from it.

- **Element numbers with gaps.** The ECU's DDOP used elements 0, 5, 9, 823 and 1148, not 0 to
  4. A task controller that assumes element numbers count up from 0 breaks on it; the
  profile's `elementNumbers` (MODEL.md section 10) reproduces it.
- **Asymmetric section delays.** On a setpoint of DDI 290, DDI 161 followed in 100 to 290 ms
  when switching on and in 8 to 23 ms when switching off.
- **Rates stay on the terminal.** AOG-TaskController asked only for the work states (DDIs 141,
  160, 161, 289, 290, on change and every 1000 ms); the operator changed the rate with buttons
  on the ECU's own screen. The spreader screens' rate keys do the same (section 4).
- **A large pool.** The ECU sent a small pool first, then its working pool of about 550 KB in
  five transfers: about two minutes to a usable screen at 250 kbit/s. `fault bigpool` covers
  the size; the two-stage upload is not simulated.
- **The Trimble task controller asked for more:** element offsets (DDIs 134 to 136) and
  working widths (DDIs 67 and 70) at connection.

## 4. Screens

`pools/spreader.iop`, made from `pools/spreader.xml` (PoolEdit XML, our own; `pools/README.md`
says how to build it). Data masks are 480 by 480, soft keys 60 by 60.

- **Run:** target and actual rate in kg/ha, where the rate comes from (the task controller or
  the operator), a bar of the sections (green on, yellow waiting out its delay, grey off, red
  in error), master and auto/manual, speed, the bin in kg with a bar graph, and area.
- **Totals:** area, applied and lifetime kg, distance and working time.
- **Alarms:** no speed and no task controller, as on the base pool.
- **Soft keys:** master, auto/manual, rate + and rate − (steps of a tenth), rate 100 % (the
  profile's rate), totals; on the totals page, run and reset; on an alarm, acknowledge.

A rate key hands the rate to the operator: prescription control goes off (DDI 158 = 0), as on
the real machine, and the task controller can turn it back on.

- **AUX-N functions** (Auxiliary Function Type 2), for a joystick assigned on the terminal:
  MASTER, AUTO, RATE+, RATE− and RESET, buttons (non-latching) that do what the soft keys do,
  on the press only; and TRIM, an analogue input that returns to the middle, which moves the
  operator's rate by up to a quarter a second while held over (5 % deadband; an input silent for
  2.5 s counts as let go), taking it from the task controller as the rate keys do.
  `tools/aux_input --pool pools/joystick.iop` has inputs of the same types.
