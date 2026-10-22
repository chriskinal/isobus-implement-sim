# Seeder

> Row-crop planters and seed drills as simulated implements. Written 2026-10-22. **Nothing here
> is built.** What is shared with the sprayer is in [MODEL.md](MODEL.md); the project in
> [PLAN.md](PLAN.md).

## 1. What it models

One type, `"type": "seeder"`, covering two kinds of machine. The product unit picks the DDIs;
the profile's `seeder` block picks the behaviour.

| Kind | Sections | Product | Delay | Why it is here |
|---|---|---|---|---|
| **Planter** | One row unit a section, electric or clutched meter a row | `seeds/ha` (count); seed sensors give singulation, skips and multiples a row | 0.1 to 0.3 s | Many sections, per-row process data, the stack's own seeder example |
| **Drill, air seeder** | A section is a group of openers fed by one meter | `kg/ha` (mass); often a second product (fertilizer) from a second tank | 1 to 4 s: the seed is in the air | Long delays, two products, two booms |

A planter tests a task controller's section count and geometry handling; a drill tests its
timing compensation and whether it copes with more than one product and boom.

## 2. Shipped profiles

| Profile | Machine | Sections | Width | Products | Delay on / off | What it tests |
|---|---|---|---|---|---|---|
| `planter-12` | 12 rows at 762 mm (30 in), mounted | 12, one a row | 9.14 m | seeds, 80 000/ha, hopper 1 500 000 seeds | 0.2 / 0.2 s | The stack's seeder as a profile: the baseline |
| `planter-24` | 24 rows at 762 mm, mounted | 24 | 18.29 m | seeds, 80 000/ha | 0.2 / 0.2 s | Two condensed messages (sections 17 to 24) |
| `planter-48` | 48 rows at 381 mm (15 in), mounted | 48 | 18.29 m | seeds, 300 000/ha (a narrow-row crop) | 0.15 / 0.15 s | Three condensed messages; 48 row elements with row data: a DDOP of about 8 kB |
| `drill-6m` | 6 m seed drill, mounted, 48 openers at 125 mm | 4 of 1.5 m | 6.0 m | seed 180 kg/ha, 1 200 kg; fertilizer 100 kg/ha, 800 kg | 1.0 / 0.8 s | Two products on one boom, mass units |
| `airseeder-12m` | 12 m air seeder, trailed, tank cart 6 m behind the openers | seed boom 8 of 1.5 m; fertilizer boom 4 of 3.0 m | 12.0 m | seed 90 kg/ha, 4 000 kg; fertilizer 60 kg/ha, 3 000 kg | 3.0 / 2.5 s | PLAN.md's two-boom machine; booms with different section counts; a TC that declares one boom (AOG-TaskController) must refuse or cope |

Planter widths follow the stack's `BOOM_WIDTH` of 9 144 mm. Row units are 300 mm behind the
hitch frame on the planters (`offsetX` −300); the air seeder's openers are 2 500 mm behind the
drawbar and its tank 8 500 mm behind, which puts the bin far from the boom for any TC that
draws geometry.

## 3. Profile

`planter-12` in full. The shared keys are in MODEL.md section 10.

```json
{
  "name": "Planter 12 row",
  "type": "seeder",
  "identity": 201,
  "connector": { "type": "threePoint", "x": 0, "y": 0 },
  "sectionControl": "setpoint",
  "sectionDelay": { "on": 0.2, "off": 0.2 },
  "booms": [
    { "offsetX": -300, "offsetY": 0, "offsetZ": 0,
      "rows": { "count": 12, "spacing": 762 } }
  ],
  "products": [
    { "name": "Seed", "unit": "seeds/ha", "rate": 80000, "min": 20000, "max": 200000,
      "tau": 0.5, "noise": 0.01, "boom": 0,
      "bin": { "unit": "seeds", "capacity": 1500000, "level": 1500000, "low": 0.05 } }
  ],
  "seeder": {
    "kind": "planter",
    "rowData": true,
    "meter": { "maxSpeed": 16 },
    "singulation": { "base": 0.985, "knee": 10, "slope": 0.01, "walk": 0.003 },
    "depth": { "setpoint": 50, "min": 20, "max": 90 }
  },
  "pool": "pools/seeder.iop"
}
```

`rows` is shorthand: `count` sections of `spacing` width, centred on the boom, numbered from
the first row. `sections[]` as in MODEL.md is accepted instead, for unequal rows. The
`drill-6m` profile has `"kind": "drill"`, two products both with `"boom": 0`, no `rowData`,
and `sectionDelay` 1.0 / 0.8. The air seeder has two entries in `booms` and one product on
each.

## 4. DDOP

The tree of MODEL.md section 1, with these additions.

| Element | Process data and properties | DDI | Unit on the bus | Notes |
|---|---|---|---|---|
| Bin (one a product) | Setpoint, actual, default, min, max rate | 11, 12, 13, 14, 15 (seeds) or 6, 7, 8, 9, 10 (kg) | 0.001 /m², or mg/m² | 80 000 seeds/ha is 8 000; 180 kg/ha is 18 000 |
| | Capacity, content | 79, 78 (seeds) or 76, 75 (kg) | count, or g | Content settable |
| | Application total, lifetime total | 82, 267 (seeds) or 81, 266 (kg) | count, or g | Trip total settable |
| | Prescription control state | 158 | | Settable |
| | Cultural practice | 179 | | Property, 2 (sowing and planting), as the stack's example |
| | Setpoint and actual seeding depth | 56, 57 | mm | Planters only. Setpoint settable; actual follows it in 2 s. A TC that sets depth is rare and the test is that nothing breaks |
| Row (planter section, `rowData` on) | Actual work state | 141 | | On, off, or error when blocked. Settable only with `sectionControl: perElement` |
| | Actual seed spacing | 17 | mm | From rate and row spacing: 80 000/ha at 762 mm is 164 mm |
| | Singulation, skips, multiples | 415, 417, 419 | ppm | 98.5 % is 985 000 |
| | Average singulation, skips, multiples | 416, 418, 420 | ppm | Since the trip total was last reset |

`rowData: false` drops the row process data and leaves the row as a plain section (offsets and
width only), for a TC that chokes on the object count. Drill sections never have row data.

Presentations (unit objects for the TC's display): seeds, seeds/ha, kg, kg/ha, mm, m, m², s.
The planters use the stack's example presentations as they are.

## 5. Behaviour

**Meter.** Seeds a second on a row = rate (seeds/m²) × row spacing (m) × speed (m/s): 16.9 a
second a row for `planter-12` at 80 000/ha and 10 km/h. Above `meter.maxSpeed` (km/h) the meter
cannot keep up and the actual rate falls as `maxSpeed / speed`; the terminal shows an alarm. A
drill's meter has the same limit, at `maxSpeed` 14 by default.

**Sections.** The planter's clutches follow MODEL.md section 2 with the short delay. A drill's
delay is the time for seed to travel from the meter to the opener, so it applies to the actual
rate as well as to the section state: when a section is commanded off, seed keeps arriving for
`sectionDelay.off` seconds, and the bin keeps draining for that long. This is the behaviour a
TC's look-ahead settings are for, and the scenario in section 8 measures it.

**Singulation.** Each row holds a singulation fraction: `base` below `knee` km/h, then minus
`slope` for every km/h above it, plus a random walk of `walk` a second so rows differ. Skips
and multiples share the remainder, two thirds skips. The averages (416 to 420) are weighted by
seeds since the last reset. Seed spacing (17) is the ideal spacing; its deviation DDIs are not
modelled.

**Two products** on one boom (the drill) apply through the same sections: both rates follow the
same section states, each with its own lag, bin and totals. Two booms (the air seeder) are two
independent section sets with one product each.

**Hopper.** MODEL.md section 4. The planter counts seeds; the drill weighs.

## 6. Screens

Start from the stack's `BasePool.iop` (MIT, in `examples/seeder_example`), edited in
AgIsoTerminalDesigner into `pools/seeder.iop`, ours to ship. Its masks stay: main run screen,
statistics (bus, terminal and task controller numbers), settings, alarms, and the two alarm
masks (no speed, no task controller). Changes:

| Mask | Shows | Keys and inputs |
|---|---|---|
| **Run** | A strip of 16 section indicators (more sections are grouped: on, off, or mixed), population target and actual, speed, hopper level bar, average singulation, auto or manual, master | Section toggles (a touch on the strip), Auto/Manual, Master, Rate +, Rate − (5 % steps), Rows, Settings, Alarms |
| **Rows** | One line a row: singulation, skips, multiples, spacing, state; 16 rows a page | Page up and down |
| **Settings** | Target rate (input number), depth (input number), delay on and off (read-only), row count and spacing (read-only), second product's rate on drills | Home |
| **Statistics** | As the stack's: bus load, addresses, terminal and task controller versions, booms and sections the TC supports | Home |
| **Alarms** | The active alarms list | Acknowledge, Home |

Alarms, as alarm masks with priority: no speed, no task controller, hopper low, hopper empty,
row blocked, speed too high for the meter. Each is an event when raised and when acknowledged.

Every key and input that changes the machine (toggle, auto, master, rate, depth) does the same
as the matching command and is reported as an event, so a harness driving the terminal under
test sees the press arrive.

## 7. Commands and events

Beyond PLAN.md section 6 and MODEL.md section 6.

| Command | Does |
|---|---|
| `rate <value> [product]` | Operator target; ignored while prescription control is on (an event says so) |
| `bin <level> [product]`, `fill [product]` | Set or refill the hopper |
| `depth <mm>` | Planter depth setpoint |
| `fault row <n> blocked` / `clear` | The row's sensor sees nothing: singulation 0, work state error, the boom's actual rate drops by one row |
| `fault bin empty [product]` | Level to zero now |
| `fault meter stall [product]` | Actual rate to zero with sections on; the TC sees sections on and nothing applied |

Events (JSON lines, with the shared ones): `product` (name, target, actual, level, trip total)
on change and every second; `row` (n, singulation, skips, multiples, spacing, state) on change;
`sections` (setpoint, commanded, actual, as strings of `0`, `1`, `E`) on change with the tick
time; `mode` (auto, master, prescription); `alarm` (name, raised or acknowledged).

## 8. Scenarios

By PLAN.md phase. Each is a scenario file for the phase 2 harness; the checks are what the
harness asserts from the event stream and, where it can, from the TC's own status.

| Phase | Scenario | Passes when |
|---|---|---|
| 1 | `planter-12` against AOG-TaskController and `aow-tcd` | Connects; the TC logs 12 sections and 9.14 m; a section setpoint shows as actual 200 to 240 ms later |
| 1 | `planter-48` | Connects; three condensed messages each way; the TC's 64-section PGN carries all 48 |
| 1 | `drill-6m` | Connects with two bins; the TC's geometry log shows four sections of 1 500 mm |
| 1 | `airseeder-12m` | The TC's answer to two booms is recorded: AOG-TaskController declares one boom. Whatever it does, it must not crash and must still drive a one-boom implement afterwards |
| 1 | `planter-12` with `sectionControl` `actualSettable`, then `perElement` | AOG-TaskController's log names the path it chose and sections follow on each |
| 3 | `planter-12` against AgIsoVirtualTerminal | The pool loads; a toggle on the terminal is an event and the section goes off; the hopper-low alarm mask shows at 5 % |
| 4 | `fault row 5 blocked` | The condensed state carries `10` for row 5; the TC's reaction recorded (MODEL.md open item 3) |
| 4 | `fault restart` mid-run on `planter-24` | Lifetime totals survive; trip totals restart at zero; the TC takes the implement back |
| 5 | One simulated hectare: `planter-12`, 10 km/h, 80 000/ha, fixed speed from the `speed` command | 1 094 m in 394 s; area total 10 000 m² within 1 %; seeds total 80 000 within 2 %; hopper down by the same |
| 5 | A drill's delivery delay: `drill-6m`, section 2 commanded off at 10 km/h | Its actual state and the rate's share drop 0.8 s later; the bin drains for 0.8 s more; the event times show it |
| 5 | Rate from a task controller | No open TC sends a rate today. Until one does, the harness sends the setpoint through the stack's `examples/task_controller_server` built as a harness tool; the actual settles within 2 % in 2.5 s (five times `tau`) |
| 7 | Master and Auto on an auxiliary input | The assignment made on the terminal moves the simulated joystick's button to the master |

## 9. Open items

1. **Hopper unit for planters.** Seeds (count), as the stack's pool shows, or kilograms, as
   most planter monitors do. Start with seeds; `bin.unit: kg` with a `seedsPerKg` key is a
   small addition if anyone needs it.
2. **Row data size.** `planter-48` with row data is about 240 process data objects. A TC that
   cannot take it is a finding, and `rowData: false` is the workaround.
3. **Tramlines** (DDIs 517 and up) and **variety or hybrid changes**: not modelled. Add when a
   TC under test does them.
4. **Drills without section clutches.** Many have only a half-width shutoff: a two-section
   profile covers it; nothing to build.
