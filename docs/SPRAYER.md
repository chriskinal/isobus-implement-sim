# Sprayer

> Boom sprayers with a liquid product as simulated implements. Written 2026-10-22. **Nothing
> here is built.** What is shared with the seeder is in [MODEL.md](MODEL.md); the project in
> [PLAN.md](PLAN.md).

## 1. What it models

`"type": "sprayer"`: one boom of sections, each a group of nozzles at a fixed spacing; one
liquid product from one tank; a pump with a maximum flow; a regulating valve that holds the
rate; pressure that follows from flow and nozzle size. Section valves open and close with a
delay. That is enough for a task controller to see a sprayer that under-applies at speed,
over-pressures when sections close, and runs dry.

Not modelled: boom height, folding, agitation, rinsing, direct injection of a second product,
pulse width modulation. Each is an open item if a task controller under test needs it.

## 2. Shipped profiles

| Profile | Machine | Sections | Width | Tank | Pump | Delay on / off | What it tests |
|---|---|---|---|---|---|---|---|
| `sprayer-24` | 24 m trailed, boom 4.5 m behind the drawbar | 12 of 2.0 m, 4 nozzles each at 500 mm | 24.0 m | 3 000 l | 200 l/min | 0.3 / 0.2 s | The default sprayer |
| `sprayer-36-nozzle` | 36 m trailed, nozzle-level control | 72 of 0.5 m | 36.0 m | 6 000 l | 400 l/min | 0.1 / 0.1 s | More than 64 sections (AOG-TaskController's limit), five condensed messages |
| `sprayer-120ft` | 36.6 m trailed, US style | 7 unequal: 6.10, 4.57, 4.57, 6.10, 4.57, 4.57, 6.10 m | 36.6 m | 4 542 l (1 200 gal) | 380 l/min | 0.5 / 0.3 s | Unequal widths; imperial presentation units (gal/ac, gal); the mirrored-geometry test of MODEL.md open item 1 |
| `sprayer-mounted-12` | 12 m mounted, boom 1.2 m behind the hitch | 5 of 2.4 m | 12.0 m | 800 l | 100 l/min | 0.3 / 0.2 s | A small mounted machine; a short connector offset |

Default rate 150 l/ha everywhere; `sprayer-120ft` 15 gal/ac (140 l/ha) shown in its own
units. Nozzles are ISO 04 (red): 1.6 l/min at 3 bar.

## 3. Profile

`sprayer-24` in full. The shared keys are in MODEL.md section 10.

```json
{
  "name": "Sprayer 24 m",
  "type": "sprayer",
  "identity": 101,
  "connector": { "type": "drawbar", "x": 0, "y": 0 },
  "sectionControl": "setpoint",
  "sectionDelay": { "on": 0.3, "off": 0.2 },
  "booms": [
    { "offsetX": -4500, "offsetY": 0, "offsetZ": 500,
      "sections": { "count": 12, "width": 2000 } }
  ],
  "products": [
    { "name": "Tank mix", "unit": "l/ha", "rate": 150, "min": 50, "max": 400,
      "tau": 1.0, "noise": 0.02, "boom": 0,
      "bin": { "unit": "l", "capacity": 3000, "level": 3000, "low": 0.05 } }
  ],
  "sprayer": {
    "nozzle": { "spacing": 500, "flow": 1.6, "pressure": 3.0 },
    "pump": { "maxFlow": 200 },
    "pressure": { "min": 1.0, "max": 6.0 },
    "rateStep": 0.1
  },
  "pool": "pools/sprayer.iop"
}
```

`sections` as `{ "count", "width" }` is shorthand for equal sections centred on the boom;
`sections[]` with each width and offset, as MODEL.md has it, is for `sprayer-120ft`. Flows are
l/min, pressures bar, nozzle spacing mm. A `presentation` key on the product (`"gal/ac"`,
`"gal"`) changes only the DDOP's presentation objects, so the TC's display shows imperial
units while the bus carries the same metric values.

## 4. DDOP

The tree of MODEL.md section 1, with these on the tank (bin) element.

| Process data | DDI | Unit on the bus | Notes |
|---|---|---|---|
| Setpoint, actual, default, minimum, maximum rate | 1, 2, 3, 4, 5 | 0.01 mm³/m² | 150 l/ha is 1 500 000 |
| Capacity, content | 73, 72 | ml | Content settable |
| Application total, lifetime total | 80, 325 | ml | Trip total settable |
| Actual flow | 37 (actual volume per time) | mm³/s | 72 l/min is 1 200 000 |
| Setpoint and actual pressure | 193, 194 | Pa | 3 bar is 300 000. Setpoint settable, accepted and ignored: this machine regulates rate |
| Prescription control state | 158 | | Settable |
| Cultural practice | 179 | | Property, 3 (crop protection) |

Presentations: l, l/ha, l/min, bar (Pa × 0.00001), mm, m, m², s; or gal, gal/ac, gal/min and
psi when the product's `presentation` says so.

## 5. Behaviour

**Flow and rate.** The target flow (l/min) is rate (l/ha) × width on (m) × speed (km/h) / 600:
72 l/min for `sprayer-24` at 150 l/ha, 12 km/h, all sections on. The actual flow follows it
with the product's `tau` (the regulating valve), capped at `pump.maxFlow`. The actual rate
reported is the actual flow turned back into l/ha over the width that is on, so with the pump
at its limit the actual rate is below the target: the under-application a TC should flag.
Speed zero or no section on: no flow, rate zero.

**Pressure.** Flow a nozzle is the actual flow over the nozzles on. Pressure is
`nozzle.pressure × (flow a nozzle / nozzle.flow)²`: 2.6 bar at 72 l/min over 48 nozzles,
4.7 bar when the rate is stepped to 200 l/ha. Below `pressure.min` or above `pressure.max`
an alarm; the rate is held, as a real regulator would, so the TC sees the rate it asked for and
the pressure it costs.

**Section transients.** When sections close, the flow target drops at once but the actual flow
takes `tau` to follow, so the pressure on the remaining nozzles rises for a second and then
settles. Closing 6 of 12 sections at 72 l/min puts 3 l/min on each remaining nozzle for the
first tick: about 10 bar, above the alarm limit. This is on purpose: the pressure alarm after a
headland turn is what an operator sees on a real machine, and it exercises alarm handling.

**Tank.** MODEL.md section 4, in litres: drains at the actual flow. Empty tank: flow and rate
zero, sections stay as commanded.

**Operator rate.** Rate + and Rate − on the terminal step the operator target by `rateStep`
(10 %). While prescription control is on, the keys are shown but do nothing, and an event says
the press was ignored.

## 6. Screens

*Phase 3 (2026-10-22): every profile shows `pools/base.iop`, the stack's seeder screen, with its
behaviour (six section toggles, auto/manual, speed, the no-speed and no-task-controller alarms,
statistics), until the pool below exists. See `pools/README.md`.*

A new pool, ours, `pools/sprayer.iop`, made in AgIsoTerminalDesigner in the style of the
stack's seeder pool so the two look like one family. 480 × 480 data mask, 80 px soft keys.

| Mask | Shows | Keys and inputs |
|---|---|---|
| **Run** | A strip of 16 section indicators (grouped beyond 16), rate target and actual in l/ha, pressure, flow, tank level bar and litres, speed, auto or manual, master | Section toggles on the strip, Auto/Manual, Master, Rate +, Rate −, Settings, Alarms |
| **Settings** | Target rate (input number), minimum and maximum (input numbers), nozzle size (input list: ISO 02 to 06, which sets `nozzle.flow`), section count and width (read-only) | Home |
| **Statistics** | As the seeder's: bus load, addresses, terminal and task controller versions, booms and sections the TC supports | Home |
| **Alarms** | The active alarms list | Acknowledge, Home |

Alarms as alarm masks with priority: no speed, no task controller, tank low, tank empty,
pressure low, pressure high, pump at its limit. Each is an event when raised and acknowledged.
Every key and input that changes the machine is also a command and an event, as on the seeder.

## 7. Commands and events

Beyond PLAN.md section 6 and MODEL.md section 6.

| Command | Does |
|---|---|
| `rate <l/ha>` | Operator target; ignored while prescription control is on (an event says so) |
| `tank <l>`, `fill` | Set or refill the tank |
| `nozzle <iso size>` | As the settings page's list |
| `fault tank empty` | Level to zero now |
| `fault pump off` / `on` | No flow with sections on; pressure zero; the TC sees sections on and nothing applied |
| `fault pump limit <l/min>` | Lowers `maxFlow`: the actual rate falls below the target at the TC's chosen speed |
| `fault nozzle <section> blocked` / `clear` | The section reports error, its nozzles carry no flow, pressure on the rest rises |
| `fault flowmeter frozen` / `clear` | The actual rate stops following: the TC sees a target it set and an actual that never moves |

Events (with the shared ones): `product` (target, actual, flow, pressure, level, trip total)
on change and every second; `sections` (setpoint, commanded, actual, as strings of `0`, `1`,
`E`) on change with the tick time; `mode`; `alarm`.

## 8. Scenarios

By PLAN.md phase, as scenario files for the phase 2 harness.

| Phase | Scenario | Passes when |
|---|---|---|
| 1 | `sprayer-24` against AOG-TaskController and `aow-tcd` | Connects; the TC logs 12 sections and 24 m; a setpoint shows as actual 300 to 340 ms later on, 200 to 240 ms off |
| 1 | `sprayer-36-nozzle` | 72 sections: the TC's answer is recorded. AOG-TaskController declares 64; whatever it does, it must still drive `sprayer-24` afterwards |
| 1 | `sprayer-120ft` | The TC's geometry log shows the seven widths in order and the outer sections at ±15 240 mm; a mirror image is the finding of MODEL.md open item 1 |
| 1 | `sprayer-mounted-12` | Connects; the connector type and offset reach the TC |
| 3 | `sprayer-24` against AgIsoVirtualTerminal | The pool loads; a toggle on the terminal is an event; Rate + raises the target by 15 l/ha; the tank-low mask shows at 150 l |
| 4 | `fault nozzle 7 blocked` | The condensed state carries `10` for section 7; pressure rises on the rest; the TC's reaction recorded |
| 4 | `fault pump off` for 10 s, then on | The TC's handling of sections on with actual rate zero recorded; the rate returns within five `tau` |
| 5 | One simulated hectare: `sprayer-24`, 12 km/h, 150 l/ha, fixed speed | 417 m in 125 s; volume total 150 l within 2 %; tank 3 000 to 2 850 l; area total 10 000 m² within 1 % |
| 5 | A rate step 150 to 200 l/ha | Actual within 2 % of 200 after 5 s; pressure from 2.6 to 4.7 bar |
| 5 | Half the boom off at 72 l/min | Pressure spikes above 6 bar within one tick, the alarm raises, pressure settles to 2.6 bar within 5 s, the alarm is cleared by acknowledge |
| 5 | `fault pump limit 60` at 12 km/h, all on | Actual rate 125 l/ha against a 150 target; the TC's under-application handling recorded |
| 5 | Rate from a task controller | No open TC sends a rate today; the harness uses the stack's `examples/task_controller_server` as a tool, as for the seeder |
| 7 | Master and Auto on an auxiliary input | As the seeder's |

## 9. Open items

1. **Imperial presentations.** The DDOP's presentation objects carry a scale and a unit string;
   whether a TC shows `gal/ac` from them, or ignores them and shows l/ha, is what
   `sprayer-120ft` finds out. The bus values stay metric either way.
2. **More sections than the TC declares.** The simulator uploads the DDOP regardless and
   records the TC's answer. Refusing to upload would hide the case the test exists for.
3. **Boom height, folding, PWM, direct injection.** Not modelled until a task controller or
   terminal under test does something with them.
4. **Self-propelled sprayers** have no tractor and no connector; the stack's connector element
   is still required. Out of scope until someone asks.
