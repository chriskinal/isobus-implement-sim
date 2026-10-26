# The implement model

> What every simulated implement shares: the object tree, section control, rates, bins, totals,
> speed, and the profile keys that describe them. Written 2026-10-22. **Nothing here is built.**
> The machines: [SEEDER.md](SEEDER.md), [SPRAYER.md](SPRAYER.md). The project: [PLAN.md](PLAN.md).

## 1. Objects

A profile describes a tree. The simulator builds the DDOP from the tree and runs the behaviour
on it; the implement type (seeder, sprayer) only chooses the product units and the behaviour
model. Element numbers are assigned in tree order, device element first, as the stack's examples
do.

| Object | From the profile | DDOP element | Process data and properties |
|---|---|---|---|
| Implement | `name`, `type`, `identity`, `connector` | Device (element 0) | Actual work state (141), effective total time (119), request default process data (0xDFFF) |
| Connector | `connector.type`, `connector.x`, `connector.y` | Connector | Offset X and Y (134, 135) as settable process data, as the stack does; connector type (157) as a property |
| Boom | `booms[]`: `offsetX`, `offsetY`, `offsetZ`, `sections[]` | Function, child of the device | Offsets (134, 135, 136) as properties; actual working width (67); setpoint work state (289); section control state (160); total area (116); effective and ineffective time (119, 120); the condensed states of section 2 |
| Section | `width`, `offsetX`, `offsetY` (centre of the section) | Section, child of its boom | Offset X and Y, working width, all as properties. A seeder row adds per-row process data (SEEDER.md section 4) |
| Product | `products[]`: `name`, `unit`, `rate`, `min`, `max`, `bin`, `boom` | Bin, child of the device | Capacity and content, setpoint and actual rate, minimum and maximum rate, prescription control state (158), application total, lifetime total, cultural practice (179) as a property. Which DDIs depends on `unit`, section 3 |

Each boom carries its own condensed work state objects and its own totals, so a two-boom
machine is two independent section sets to the task controller.

**Coordinates.** Millimetres. X positive forward, so a boom behind the hitch has a negative X.
Y positive to the right, seen from above with the machine pointing up (open item 1). In the
profile a section's offsets are relative to its boom; the DDOP sends every element's offset
from the device reference point, so a section's is the boom's plus its own. Section Y offsets
are the section centres; section 1 is the first in the list and the profiles list them left to
right, so section 1 has the most negative Y, as in the stack's examples.

## 2. Section control

The profile's `sectionControl` key chooses how the task controller is allowed to drive the
sections. Each value matches one of the three paths in AOG-TaskController's
`task_controller.cpp`, which picks the first it finds in the DDOP in this order.

| `sectionControl` | DDOP | Sent by the TC | What it tests |
|---|---|---|---|
| `"setpoint"` (default) | Setpoint condensed work state (290 and up), settable; setpoint work state (289), settable; actual condensed work state (161 and up), read-only | 290 and 289 | The modern path, what the stack's examples do |
| `"actualSettable"` | Actual condensed work state (161 and up) marked settable; no 290 | 161 | Older implements |
| `"perElement"` | Actual work state (141) on every section element, settable; no condensed objects | One 141 per section, plus 289 | The oldest path; many small messages |

The condensed value is two bits a section, section 1 in the lowest bits: `00` off, `01` on,
`10` error, `11` not installed. Slots past the last section are `11`. A faulted section
(SEEDER.md and SPRAYER.md section 7) reports `10` and is off.

**From setpoint to actual.** Each section has a setpoint (from the TC), an operator switch
(from the terminal or the `switch` command, default on, `switchesDefault` in the profile) and a
master (289 from the TC, or the terminal's master key, or the `master` command). The commanded
state is

- in auto (section control state 1): TC setpoint and switch and master,
- in manual (0): switch and master.

The actual state follows the commanded state after `sectionDelay.on` or `sectionDelay.off`
seconds, measured from the first tick that sees the command (so up to 20 ms after it arrived on
the bus); a command reversed within the delay cancels the pending change. The delay is per machine (clutches, valves, air delivery); the per-type
documents give the values. The boom's and the device's actual work state (141) is 1 when any
section is on. A change of actual state is sent on change (the stack's OnChange trigger), never
more often than once a tick.

**Modes.** Section control state (160) and prescription control state (158) are settable; the
terminal's auto/manual key and the `auto` command set the same values. Mode changes are events.

## 3. Rates

A product's rate has a target and an actual.

**Target.** With prescription control on (158 = 1), the TC's setpoint (settable rate DDI); with
it off, the operator's value from the terminal, the `rate` command, or the profile's `rate`.
A TC setpoint outside `min` and `max` is clipped and an event says so; the clipped value is
what is reported back.

**Actual.** Zero when no section is on or speed is zero, and the regulator starts afresh when
application resumes. Otherwise a first-order lag toward the lesser of the target and the
machine's limit at this speed (meter speed, pump flow: per type) with time constant `tau`
(seconds), then noise: a gaussian of `noise` times the target, fresh every tick. Bins and
totals use the lagged rate without the noise. Reported in the DDI's unit at the TC's chosen
interval, default 1 s.

| `unit` | Setpoint, actual, minimum, maximum | DDI unit | One profile unit is |
|---|---|---|---|
| `l/ha` | 1, 2, 4, 5 (volume per area) | 0.01 mm³/m² | 10 000 |
| `kg/ha` | 6, 7, 9, 10 (mass per area) | mg/m² | 100 |
| `seeds/ha` | 11, 12, 14, 15 (count per area) | 0.001 /m² | 0.1 |

Default rate (3, 8, 13) is the profile's `rate`. The unit scalings are from our reading of the
DDI database and are checked once in phase 5 against the TC's display (open item 2).

## 4. Bins

Each product has a bin: `bin.capacity` and `bin.level` in the profile's unit (`l`, `kg` or
`seeds`). The level drops every tick by the actual rate times the area worked in that tick
(section 5). At `bin.low` (default 5 % of capacity) an alarm; at zero, the actual rate is zero,
the alarm changes to empty, and the sections stay as commanded: a real machine keeps its valves
open with nothing to spray. The `bin <level> [product]` command and `fill` refill. Content is
settable in the DDOP, as the stack's examples have it, so a TC that writes a level is accepted.

| `unit` | Capacity, content (DDI unit) | Application total, trip (DDI unit) | Lifetime total |
|---|---|---|---|
| `l` | 73, 72 (ml) | 80 (l); 351 is the same in ml | 325 |
| `kg` | 76, 75 (g) | 81 (kg); 352 is the same in g | 266 |
| `seeds` | 79, 78 (count) | 82 (count) | 267 |

The trip totals in whole litres and kilograms (80, 81) are the ones every task controller
knows; the millilitre and gram ones (351, 352) are newer and ship beside them.

## 5. Totals

Per boom: total area (116, m²) adds the sum of the on sections' widths times the distance
travelled each tick; effective time (119, s) while any section is on, ineffective time (120)
while connected with none on; effective and ineffective distance (117, 118, mm) likewise. Per
product: the trip total of section 4. All are reported with the Total trigger, so the TC
collects them when it asks. Trip totals are settable: a TC that writes zero resets them, and
the reset is an event. Lifetime totals are not settable and survive a restart in a small state
file beside the profile (`<profile>.state.json`), so a `fault restart` does not zero them.

## 6. Speed

From the bus, as the stack's seeder example reads it: machine selected speed (PGN 65096),
then ground based (65097), then wheel based (65098), the best source present. Distance for
totals and bins is integrated from it. Two seconds without any speed message: speed unknown,
nothing accrues, actual rates hold, and after `alarms.noSpeed` seconds (default 10) the
no-speed alarm. The simulator never sends speed: that is a tractor ECU's job, and
AOG-TaskController and `aow-tcd` both have one.

For a bench with no tractor ECU: `speed <km/h>` fixes the speed, `speed bus` returns to the
bus. This is an addition to PLAN.md section 6.

## 7. Process data triggers

When the TC asks for defaults (0xDFFF), the simulator answers as the stack's examples do:

| Data | Trigger |
|---|---|
| Section states, work states, working width, modes | On change, threshold 1 |
| Actual rate, bin content, pressure | On change, threshold 1, and every 1 s |
| Totals | Total only |

A measurement command from the TC (time interval, distance interval, thresholds) is honoured by
the stack's client and reported as an event, so a harness can check what the TC asked for.

## 8. Timing

The model ticks every 20 ms on its own thread; the stack's threads read a snapshot. Delays and
lags are exact to a tick, so a scenario can expect a 300 ms valve to show on the bus between
300 and 340 ms after the command (the tick, plus the stack's transmit).

## 9. The model without a bus

The behaviour is a plain class: inputs are time, speed, setpoints and switches; outputs are
actual states, rates, levels and totals. Unit tests step it without CAN: a section delay is
exact to a tick; a rate step settles within 2 % after five time constants; one simulated hectare
drains the bin by the rate times one hectare; the area total equals width times distance. These
run in CI from phase 1, before any scenario on `vcan0`.

## 10. Profile keys

```json
{
  "name": "Sprayer 24 m",
  "type": "sprayer",
  "identity": 101,
  "connector": { "type": "drawbar", "x": 0, "y": 0 },
  "sectionControl": "setpoint",
  "sectionDelay": { "on": 0.3, "off": 0.2 },
  "switchesDefault": "on",
  "booms": [
    { "offsetX": -4500, "offsetY": 0, "offsetZ": 500,
      "sections": [ { "width": 2000, "offsetX": 0, "offsetY": -11000 } ] }
  ],
  "products": [
    { "name": "Tank mix", "unit": "l/ha", "rate": 150, "min": 50, "max": 400,
      "tau": 1.0, "noise": 0.02, "boom": 0,
      "bin": { "unit": "l", "capacity": 3000, "level": 3000, "low": 0.05 } }
  ],
  "speed": { "source": "bus" },
  "alarms": { "noSpeed": 10, "noTaskController": 30 },
  "pool": "pools/sprayer.iop",
  "sprayer": { "...": "type-specific keys, SPRAYER.md section 3" }
}
```

`type` is `seeder`, `sprayer` or `spreader` ([SEEDER.md](SEEDER.md), [SPRAYER.md](SPRAYER.md),
[SPREADER.md](SPREADER.md)). `elementNumbers`, optional, gives the DDOP's element numbers in the
order they are built: the device (must be 0), the connector, each boom followed by its
sections, then the bins; without it they are 0, 1, 2 and on. Real ECUs leave gaps
(SPREADER.md section 3), so a task controller has to look element numbers up, not count them.
`connector.type` is `drawbar` (ISO 6489-3, DDI 157 value 1), `threePoint` (ISO 730 mounted,
value 3) or a number. `identity` goes into the NAME's identity number; the manufacturer code is
PLAN.md open item 1. Lengths are millimetres, rates and levels in the product's unit, times in
seconds.

## 11. Open items

1. **Y sign.** Settled 2026-10-22 from two secondary sources, since neither AOG-TaskController
   nor the stack states it and the ISO text is not to hand: AgGateway's ADAPT equipment
   discussion (which says its frame is shared with ISO 11783) and CNH's ISOXML implementation
   guide both have positive Y to the right of the direction of travel, negative to the left.
   The stack's examples agree (section 1 at the most negative Y, called the left). Still worth
   a check against ISO 11783-10's own figure when someone has it; the `sprayer-120ft` profile
   (unequal widths) is the test that shows a mirrored implement either way.
2. **DDI unit scalings.** Checked against <https://www.isobus.net/isobus/dDEntity> on
   2026-10-22: 6 and 7 are mg/m², 11 and 12 are 0.001 /m², 72 and 73 ml, 75 g, 78 count, 80 l
   (not ml). Still to check there: 1 and 2 (0.01 mm³/m² from the stack's comments), 193 and 194
   (Pa), 415 to 420 (ppm), and what the TC under test displays for each.
3. **Error state.** No open task controller handles the `10` (error) section state today.
   The seeder's blocked row and the sprayer's blocked nozzle will show what each does with it.
