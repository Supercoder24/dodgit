# Host simulation of the ball and the pouches

Runs the **unmodified firmware** (`shared/uwb.c`, `shared/pouch_fsm.h`, `ball/src/main.c`, `ball/src/ball_logic.h`, `pouch/src/main.c`, `pouch/src/display.c`, ...) on your computer, against a simulated DW3110 radio, so the join / rejoin / reset / throw scenarios of [docs/Plan.md](../../docs/Plan.md) can be exercised without boards. Needs only `cc` and `python3` (no Zephyr, no board).

> **Status:** all 71 scenarios pass (about 20 s for `--all`). The scenarios and the simulator were written by an agent and have not been reviewed line by line, so treat a failure as "look at it" first: it may be a product bug (see [FINDINGS.md](FINDINGS.md), which lists the ones it already found) or a mistake in the simulator or in the scenario.

## Running it
```bash
tests/sim/run.sh                  # build + the CORE scenarios (21)
tests/sim/run.sh --all            # every scenario (71; includes the 5 virtual-minute soak test, ~20 s)
tests/sim/run.sh late_pouch       # scenarios whose name contains the argument (searches all of them)
tests/sim/run.sh --list           # list them, core ones marked with *
tests/sim/run.sh --build-only     # just build
SIM_VERBOSE=1 tests/sim/run.sh    # also print the notes / log excerpts of passing scenarios
SIM_JOBS=2 tests/sim/run.sh       # scenarios at the same time (default: number of CPUs)
SIM_BUILD=/some/dir tests/sim/run.sh   # keep the build products
```
Every scenario runs in its own Python process, so a crash or hang of one cannot hide the others (each has a timeout). A failure prints the assertion and the relevant slice of the nodes' logs and of the radio traffic. Exit status is non-zero if anything failed. The core list is `CORE` in `run_scenarios.py`.

## How it works
* `src/sim_core.c`, `src/sim_dw.c`, `src/sim_api.c` (`libsim`): **one process, several nodes**. Each node is the firmware compiled into its own dynamic library (so the file-scope statics of `uwb.c` exist once per node), running as a coroutine. The simulator owns **virtual time** (no real time anywhere, fully deterministic for a given seed) and the radio "air". Anything a node does that another node could observe first synchronizes with the global event order.
* `shim/`: replacements for the Zephyr kernel / GPIO / random / hwinfo APIs and the DW3000 driver API used by the product.
* `mocks/`: the ball's accelerometer service (`impact.c`: the scenario sets the level Rest / Held / Spinning and injects Hit / stall events), `ble_log.c`, and a hook that reports every ranging round (true distances can be compared with the measured ones).
* `simlib.py`: the scenario DSL (`World`: `power_on`, `reboot`, `place`, `long_press`, `set_accel`, `hit`, `drop(...)` frames, `run_until`, `grep` the logs, `check`, ...). `helpers.py`: shared building blocks (start a game, pick up, throw, ...). `scenarios/*.py`: the scenarios. The product's `settings.h` values are read, so scenarios follow the tuning.

### Radio model
40-bit device clocks with their own random offsets (so they wrap at odd moments), immediate and delayed TX (a delayed time in the past fails like the real chip), single-shot RX, `DWT_RESPONSE_EXPECTED`, frame airtime, propagation delay from the 3D distance, overlapping frames destroy each other at a receiver, random per-link loss, drop rules by message type, and a modelled CPU cost per SPI-like call. The DS-TWR distances the ball computes are checked against the placed positions.

### Fidelity limits (what it can NOT tell you)
* RF reality: antenna pattern, body shadowing, multipath, the real loss rate and its distance dependence (the data in `data/` says a pouch within 300 mm is missing from ~8% of the rounds; scenarios inject that loss, they do not predict it).
* The real accelerometer and classifier: levels are scripted. Throw detection thresholds belong to `tests/accel_class_test.c` and hardware captures.
* Real CPU timing, interrupts, the Bluetooth log, power. Race conditions that depend on exact microseconds of the real chip.
* The LED patterns are checked as masks over time, not as light you can see.

## Scenarios (by file)
| File | Behaviour |
|------|-----------|
| `s1_start` | ball and 3 pouches in different power-on orders; Roll Call ends by press or timeout; ranging distances correct |
| `s2_pouch_cycle` | a power-cycled pouch gets its slot and lives back (also lone pouch, all at once, long absence) |
| `s3_ball_reboot` | ball reboot: pouches fall back to Joining and re-register |
| `s4_lost_messages` | lost HIT / HIT_ACK / REMEMBER: the ball's table is authoritative |
| `s5_late_pouch` | late pouches join during Reset, also when RESET broadcasts are lost and at the last moment |
| `s6_reset_deletes` | silent pouches deleted, slots reused, empty table, deleted-but-alive pouch |
| `s7_table_full` | the ninth pouch is ignored |
| `s8_game_flow` | pickup, throw, hit, miss, out-of-lives, hand-over, Thrown timeouts / return, stall, bounce |
| `s9_soak` | 4 pouches, 10% frame loss, 5 virtual minutes: no stuck state, invariants hold |
| `s10_robust_decisions` | decisions survive missing rounds (Classify merging, hand-over under loss, silent thrower, REMEMBER timing) |
| `s11_gap_scenarios` | found by mutation testing: the launch spike is not a hit, close players do not swap the thrower, a Reset from Ready / Thrown / Hit |

## Adding a scenario
Create or extend a file in `scenarios/`, decorate a function `f(w)` with `@scenario("name")` (optionally `seed=`, `timeout=`, `known_bug=n`), build the situation with the `World` API, and assert with `w.check(...)` / `w.check_eq(...)`. `known_bug=n` marks a scenario that fails because of a product bug listed in [FINDINGS.md](FINDINGS.md); it stays failing (do not weaken the assertion) and is printed as `KNOWN BUG #n`. Add the name to `CORE` only if it is quick and covers something no other core scenario does.
