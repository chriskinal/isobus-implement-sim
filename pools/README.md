# Pools

Object pools the simulated implements show on a terminal (docs/PLAN.md phase 3).

| File | What | Origin |
|---|---|---|
| `base.iop` | The stack's seeder example screen: a run screen with six section toggles and an auto/manual button, statistics, settings and alarms pages, two alarm masks (no speed, no task controller) | AgIsoStack++ `examples/seeder_example/BasePool.iop`, MIT, unchanged; its object IDs in `base_pool_ids.hpp` |

Every shipped profile uses `base.iop` for now, the sprayers included, so a terminal under test has a
pool with real behaviour behind it. The machines' own pools (SEEDER.md and SPRAYER.md section 6)
come after, from AgIsoTerminalDesigner. Any other `.iop` named in a profile is loaded and shown
without behaviour: keys and values still come out as events.

| `aux_functions.iop` | A pool with three auxiliary functions (phase 7): an on/off function, an on/off toggle, a "plus" function | AgIsoStack++ `examples/virtual_terminal/aux_functions/aux_functions_pooldata.iop`, MIT, unchanged; IDs in `aux_functions_ids.hpp` |
| `aux_inputs.iop` | The pool of `tools/aux_input`, a simulated joystick with a button and a slider | AgIsoStack++ `examples/virtual_terminal/aux_inputs/aux_inputs_pooldata.iop`, MIT, unchanged; IDs in `aux_inputs_ids.hpp` |

With `implement --pool pools/aux_functions.iop`, the implement offers those functions to the
terminal and acts on them when an input is assigned: on/off is the master, the toggle is
auto/manual, plus is a rate step up. Assignment is the terminal's job; `vt_stub` does not do
AUX-N, so that part is checked by hand on AgIsoVirtualTerminal.
