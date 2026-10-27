# Pools

Object pools the simulated implements show on a terminal (docs/PLAN.md phase 3).

| File | What | Origin |
|---|---|---|
| `base.iop` | The stack's seeder example screen: a run screen with six section toggles and an auto/manual button, statistics, settings and alarms pages, two alarm masks (no speed, no task controller) | AgIsoStack++ `examples/seeder_example/BasePool.iop`, MIT, unchanged; its object IDs in `base_pool_ids.hpp` |

Every shipped profile but the spreader uses `base.iop` for now, the sprayers included, so a terminal under test has a
pool with real behaviour behind it. The spreader has its own (`spreader.iop`, below); the seeder's and
the sprayer's (SEEDER.md and SPRAYER.md section 6) come after, the same way. Any other `.iop` named in a profile is loaded and shown
without behaviour: keys and values still come out as events.

| `aux_functions.iop` | A pool with three auxiliary functions (phase 7): an on/off function, an on/off toggle, a "plus" function | AgIsoStack++ `examples/virtual_terminal/aux_functions/aux_functions_pooldata.iop`, MIT, unchanged; IDs in `aux_functions_ids.hpp` |
| `spreader.iop` | The spreader's screens (SPREADER.md section 4): run and totals pages, two alarm masks, soft keys for master, auto/manual and the rate; AUX-N functions for a joystick (master, auto, rate +/−/reset, rate trim) | Ours, MIT: built from `spreader.xml`; IDs in `spreader_ids.hpp` |
| `aux_inputs.iop` | The pool of `tools/aux_input`, a simulated joystick with a button and a slider | AgIsoStack++ `examples/virtual_terminal/aux_inputs/aux_inputs_pooldata.iop`, MIT, unchanged; IDs in `aux_inputs_ids.hpp` |
| `joystick.iop` | `tools/aux_input --pool pools/joystick.iop`: the I2C joystick module as an AUX-N device, two axes that spring back to the middle (Auxiliary Input Type 2, analogue return to 50 %) and five buttons (boolean, not latching), each labelled for the terminal's assignment screen | Ours, MIT: built from `joystick.xml`; IDs in `joystick_ids.hpp` |

With `implement --pool pools/aux_functions.iop`, the implement offers those functions to the
terminal and acts on them when an input is assigned: on/off is the master, the toggle is
auto/manual, plus is a rate step up. Assignment is the terminal's job; `vt_stub` does not do
AUX-N, so that part is checked by hand on AgIsoVirtualTerminal.

## Building a pool from PoolEdit XML

`spreader.xml` is the source of `spreader.iop`, and `joystick.xml` of `joystick.iop`. PoolEditParser
crashes on an `auxiliaryinput2` without `function_attributes_b`, so the joystick's inputs carry an
empty one. Open it in PoolEdit
(github.com/moehman/PoolEditDist, `java -jar PoolEdit.jar`) to change the screens; keep every
object's `id`, which `src/vt_client.cpp` uses through `spreader_ids.hpp`. Then

```sh
POOLEDITPARSER=/path/to/pooleditparser pools/build_pools.sh
```

writes `spreader.iop` and `spreader_ids.hpp`. `tools/pool_export.py` does PoolEdit's "Export
to Embedded XML" step (ids on every reference) and writes the header; PoolEdit's parser
(github.com/moehman/PoolEditParser, GPL) turns the export into the binary pool. Build the
parser with `g++ pooleditparser.cxx xml.cxx parser.cxx -o pooleditparser -lexpat`; on macOS
name the output something else than `pooleditparser` if its source folder is beside it, since
the file system ignores case. The parser is a build tool only: none of it is in this repository
or the release tarballs, and the built `.iop` and header are committed, so building the
simulator does not need it.
