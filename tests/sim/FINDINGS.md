# Findings of the simulation

Product-code problems the simulation exposed, with what was done about them. A scenario that fails because of a real product bug is marked `known_bug=n` in its `@scenario(...)` and keeps failing until the product is fixed.

## Fixed
1. **A late pouch could miss its only chance to join.** The ball stopped re-sending RESET once every known pouch had acknowledged, and a late (Listening) pouch reacts to RESET only, so one lost broadcast left it out of the game. Fix: RESET is repeated every `RESET_KEEPALIVE_MS` for the rest of the window (`shared/uwb.c`, `uwb_ball_reset_step`). Scenarios: `late_pouch_first_reset_lost`, `late_pouch_first_two_resets_lost`.
2. **A lone pouch (or all pouches together) that rebooted never got its slot back.** After a ranging round that heard nobody the ball held every REMEMBER back (the pouches may still be waiting for a FINAL), but when nobody answers EVERY round is such a round, so the REMEMBER was never sent. Fix: `uwb_ball_remember_tick()` waits out the hold (at most ~12 ms, once per REMEMBER) instead of skipping it. Scenarios: `pouch_cycle_single_pouch_game`, `pouch_cycle_all_pouches_at_once`.

3. **The RESET keepalive (fix 1) never left the antenna.** Found by the first run of `late_pouch_first_reset_lost`: every RESET after the first was `aborted` on the air. Once every known pouch had acknowledged, `reset_broadcast()` skipped its listen loop and called `radio_reset_state()` straight after starting the transmission, which forces the transceiver off while the frame is still going out (~0.19 ms). Fix: `wait_tx()` before anything else (`shared/uwb.c`). The pure-C tests cannot see this, only the radio model can.

Fixes 1-3 are confirmed by their scenarios (`late_pouch_*`, `pouch_cycle_single_pouch_game`, `pouch_cycle_all_pouches_at_once`).

## Simulator / scenario mistakes found and fixed
* `soak_4_pouches_10pct_loss` never finished: its driver looped `while w.now < t_end`, but the simulator's clock is whole picoseconds, so `now` stopped a hair below `t_end` and the loop spun forever (no product problem). Now compares with a tolerance.

## Open
None known. Status of the last full run: 66 of 66 scenarios pass (about 20 s). The simulator's fidelity limits are in [README.md](README.md); a pass here does not replace the hardware checks listed in docs/Plan.md.
