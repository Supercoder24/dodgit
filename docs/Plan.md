# Plan: rejoinable game + simplified ball FSM (v2)

Revises [Changes.md](Changes.md) into an implementation plan. READMEs and [FSMs.md](FSMs.md) are updated alongside each implementation step (see [Steps](#implementation-steps)).

> **Status: implemented** (steps 1-5; see the git log, one commit per step). This document is kept as the design record; [FSMs.md](FSMs.md) and the READMEs describe what the code does now. Differences from the plan, found while implementing it and checking it against the recorded data (no hardware was available, so everything below that needs a board is still to be measured, see the last list):
>
> **Added or changed**
> * Listening also falls back to Joining when it hears no POLL for `LOST_TIMEOUT_MS` (a ball that rebooted into Roll Call would otherwise never see this pouch).
> * "Someone joined" (Reset -> Roll Call) counts **new** pouches only; a known pouch that rejoins does not send the ball to Roll Call.
> * RESET is not only sent until every pouch has acknowledged: it is repeated every `RESET_KEEPALIVE_MS` for the rest of the window, because a late (Listening) pouch reacts to RESET only and one lost broadcast left it out of the game. The pouch shows / logs the reset effect once per Reset.
> * The Reset window is `RESET_WINDOW_MS` (3 s), no longer derived from the LED blink timing; the hold time is `BUTTON_HOLD_MS`.
> * The pouch hit effect also plays when a REMEMBER reveals a lost HIT (lives dropped while Active).
> * Missing pouches in a ranging round are treated as missing information, not as evidence (the recorded data shows a pouch within 300 mm is absent from about 8% of the rounds): `Classify()` ranges `CLASSIFY_ROUNDS` times and merges per pouch; a hand-over streak is only broken by evidence (`THROWER_SWITCH_GRACE_MS`); one silent round of the thrower does not start Thrown (`THROWER_SILENT_MS`); a missing pouch does not break the Thrown return streak.
> * The ball holds a REMEMBER back after a ranging round that heard nobody (the pouches are still waiting for a FINAL then).
> * The optional Thrown shortcut is implemented (`THROWN_REST_MS`: Rest for 1 s -> `Disarm()`).
> * Settings renamed: `CLASSIFY_ATTEMPTS` -> `RANGE_ATTEMPTS` (+ new `CLASSIFY_ROUNDS`), `ARMED_RECHECK_MS` -> `THROWN_RECHECK_MS`, `READY_STILL_TIMEOUT_MS` -> `READY_NOT_HELD_MS`, `INFORM_TIMEOUT_MS` -> `HIT_ACK_TIMEOUT_MS`.
> * The launch spike needs no filter in Thrown: every state except Thrown drains the accelerometer events each pass.
> * Raw accelerometer capture is started by a short button press outside Roll Call (only in builds with `LOG_ACCEL_RAW`).
> * The decision logic is extracted into pure C so it can be tested on the host: `shared/members.h`, `shared/pouch_fsm.h`, `shared/led_patterns.h`, `ball/src/ball_logic.h`, `ball/src/accel_class.[ch]` (`tests/run_tests.sh`); `tests/sim/` runs the real firmware against a simulated radio.
>
> **Still to do on hardware** (nothing here could be measured without boards): the new labeled accelerometer captures listed in [Accelerometer.md](Accelerometer.md); the LED legibility checks of E5; the short-throw detection window of Risk 6 (`LOG_RANGE` on a real 3 m throw); the Bluetooth log throughput of F1 (all `LOG_*` on, watch `log_dropped=`); tuning of every distance and `ACCEL_*` threshold.

Items marked **[Added]** are not in Changes.md: they close gaps found while checking it against the current code (`ball/src/main.c`, `pouch/src/main.c`, `shared/uwb.c`) and the recorded data in `data/`. Items marked **[Revised]** change something Changes.md said. Everything else is Changes.md as written. All of these are listed in [Decisions](#decisions-to-confirm) so you can veto them.

---

## 1. What changes, at a glance

| Area | Today | After |
|------|-------|-------|
| Lives | Pouch owns them, reports in every RESPONSE | **Ball owns them** (table). Pouch only displays; ball keeps it in sync |
| Joining | Roll Call only; late or rebooted pouch is permanently "join failed" | Pouch can rejoin after reboot (REMEMBER) or join late (via Reset) |
| Reset | Broadcast RESET, tolerate silent pouches | Also the **join window**; pouches that never ACK are **deleted** |
| Ball states | Disarmed, Ready, Armed, Classify, Inform | **Unheld, Ready, Thrown, Hit** + `Disarm()` / `Classify()` as functions |
| Throw detection | Out of range **and** free fall (\|a\| < 700 mg); impact only counts within 150 ms after free fall | Accelerometer classified as Rest / Held / Spinning + Hit event. Thrown = Spinning **and** the **thrower** is beyond the throw range (2 m); other pouches are not checked (Q19). The FSM state supplies the context instead of the free-fall gate |
| Pouch FSM | Joining, Joined, Active, Join-failed (terminal) | Joining, **Listening**, Joined, Active; nothing is terminal |
| LEDs | Pouch: N red/green LEDs lit = N lives, binary slot, blue identity. Ball: state number in binary, blue identity, strip colors for later | **One color at a time**, color + steady/blink instead of counting LEDs, no binary codes, no identity LED, no ball strip (Part E) |
| Logging | Fixed set of lines, always on (console + Bluetooth) | **Per-category compile-time flags** in `settings.h`; the high-rate ones are off by default (Part F) |

Unchanged: DS-TWR ranging and timeslot mechanics, button semantics (short press ends Roll Call, 3 s hold = Reset, never in Roll Call), BLE debug log.

### Invariants (from your "Reasoning")
1. A pouch power-cycled mid-game comes back with the **same timeslot and lives**.
2. Adding a player never requires turning anything else off.
3. Power-on **order of ball and pouches doesn't matter**.

### Gameplay assumptions
These are deliberate scope limits, not bugs. Step 5 must carry them into the root README (Gameplay) and FSMs.md so players and future developers see them.
* **No catches.** Players don't catch the ball. The ball is only picked up from the ground or handed over deliberately. A catch is not detected as such: its impact spike would most likely be scored as a Hit on the catcher.
* **No point-blank throws (Q5, option C).** Ready never classifies a Hit. **[Revised, Q19]** Thrown is entered only when the ball is Spinning **and** farther than the throw range (2 × pickup range = 2 m today) from the **thrower**; other pouches are not checked, so players standing near the flight path no longer matter. A throw must therefore travel more than `THROW_RANGE_MM` before it lands: the minimum throw distance is a bit over **2 × `PICKUP_DISTANCE_MM`** (about 2 m today; the detection window is short for 2–3 m throws, see Risk 6). A shorter throw never enters Thrown: its impact is ignored, no life is deducted, and the ball times out to Unheld. To lower the minimum distance, reduce `PICKUP_DISTANCE_MM` (the throw range follows it) or decouple the two.
* **No passes.** Players don't pass the ball to each other in the air. Possession changes only by a deliberate hand-over, or when the ball ends up closer to another player's pouch than to the thrower's for a timeout (`THROWER_SWITCH_MS` in Ready, `THROWN_RETURN_MS` in Thrown; see sections D3 and D4 of Part D).

---

## 2. Part A: membership and persistence (protocol)

### A1. Ball table
Per pouch: `uid`, `slot`, `lives`, plus a per-Reset `acked` flag. RAM only, so a ball reboot starts a new game (see Q10).

### A2. Messages

| Message | Direction | Payload | Change |
|---------|-----------|---------|--------|
| JOIN | pouch → ball | u32 uid | unchanged. Ball only *listens* for it in **Roll Call and Reset** |
| **REMEMBER** | ball → broadcast | u32 uid, u8 slot, u8 lives | **new**; also replaces ASSIGN **[Revised]**, because ASSIGN carries the same information (the immediate reply to a JOIN is just a REMEMBER) |
| RESPONSE | pouch → ball | ~~u8 lives~~ (none) | **[Revised]** ball is the source of truth, so the field is redundant |
| HIT | ball → pouch | u8 **lives remaining** (replaces u16 hit_seq) | **[Revised]** see below |
| HIT_ACK | pouch → ball | u8 lives | echoes the value |
| RESET / RESET_ACK | unchanged | - | - |
| POLL / FINAL / REPORT | unchanged | - | - |

**Why drop `hit_seq`:** HIT now carries the absolute lives value, so a retry sets the same number again and is harmless; the pouch only plays its "lost a life" effect when the value *decreases*. Removing it also removes the pouch's de-duplication state. (Ball log lines can keep a hit counter.)

### A3. REMEMBER schedule (ball, "Always")
* Every pouch gets one REMEMBER per second. **[Revised]** Send it as a **trickle** (one pouch every `1000 / n` ms, between ranging rounds) rather than a burst "wave", so no ranging round is ever delayed by a burst of up to 8 frames. A wave also works; this is only a scheduling choice (Q9).
* Sent in every state between exchanges, including Roll Call and Reset (it doubles as the ball's heartbeat there, see A5).
* Broadcast addressed by `uid` in the payload, because a rebooted pouch has no slot address yet.
* Cost is negligible: ~0.3 ms of airtime per frame, 8 frames/s at most.
* **Known, harmless miss [Added].** While a pouch is inside `pouch_handle_poll()` waiting for FINAL (up to ~10 ms, e.g. after a round in which the ball got no RESPONSE and therefore sent no FINAL), it ignores other frames, so a REMEMBER sent at that moment is lost. The next one arrives within `REMEMBER_PERIOD_MS`, far inside `LOST_TIMEOUT_MS`, so nothing is done about it.

### A4. What a pouch does with REMEMBER
Ball is authoritative: **any REMEMBER whose `uid` matches overwrites the pouch's slot and lives**, in any phase.
* Pouch rebooted mid-game: Joining/Listening → Joined → Active with the old slot and lives. (Invariant 1.)
* ASSIGN lost during Roll Call or Reset: the next REMEMBER fixes it, so no extra retry logic.
* HIT never acknowledged: ball already decremented its table, and the pouch's display is corrected within ~1 s.

### A5. Pouch loss detection **[Added]**
Heartbeat = a REMEMBER with *my* uid. If none arrives for `LOST_TIMEOUT_MS` (proposed 5 s), the pouch goes Active/Joined → **Joining** (stops answering POLLs).
This is required for two cases that would otherwise break the invariants:
* **Ball reboot:** the ball forgets everything and reassigns slots first-come-first-served, while old pouches keep answering in their stale slots, so slots collide. With the timeout, pouches fall back to Joining and re-register during the new Roll Call. This is also what makes "pouches on first, ball later" (invariant 3) work.
* **Deleted-but-alive pouch:** if the ball deletes a pouch that missed the Reset (A6), the pouch must stop using a slot that may now belong to someone else. "Any frame from the ball" is *not* a usable heartbeat here, since the deleted pouch still hears POLLs. It has to be REMEMBER addressed to it.

### A6. Reset becomes the join window
Ball, entering Reset (3-second hold, any state except Roll Call):
1. All table lives := `LIVES_MAX`; clear `acked` flags.
2. **[Revised] Listen for the whole 3 seconds (all 3 blinks).** The receiver is armed at all times except while the ball is transmitting, and handles whatever arrives: a RESET_ACK from a known slot, or a JOIN from a pouch that has no slot. Listening does **not** stop early when every known pouch has ACKed (today's code stops re-sending RESET at that point), and it runs the full 3 seconds even when the table is empty.
3. Broadcast RESET every `RESET_RETRY_MS` (200 ms, existing) until every pouch in the table has ACKed (a joined pouch counts as ACKed, step 4).
4. **[Added] JOIN handling:** assign the lowest free slot (known uid → same slot), reply with REMEMBER (lives = max) and **mark that pouch `acked`**. A JOIN proves the pouch is alive and already has the new round's lives, so a pouch that joins late in the window (even in the last 200 ms, before it could ACK a RESET) is never deleted in step 5.
5. At the end of the blinks, **delete every pouch that never ACKed** (free its slot; recompute the highest used slot, which also shortens ranging rounds).
6. Next state:
   * table empty → **Roll Call** **[Added]** (otherwise the ball sits Unheld with nobody to talk to),
   * someone joined during this Reset → **Roll Call** (restart its timer; short press or timeout ends it) **[Revised]**: Changes.md says "maybe". This gives several late players time to join and costs nothing when nobody does (Q2),
   * else → `Disarm()`.

Deleting is safe because everyone's lives were just reset to the maximum, so nobody loses progress by being forgotten.

**Implementation note [Added].** Today's code cannot do steps 2 and 4 by a small tweak: `uwb_ball_send_reset()` returns immediately when the table is empty, `collect()` ignores every frame that does not come from an assigned slot (a JOIN comes from `UNASSIGNED_ADDR`), and `collect()` switches the radio off when its window ends. Step 2 of the implementation needs a Reset-specific service loop (e.g. `uwb_ball_reset_step()`, in the style of `uwb_ball_rollcall_step()`) that sends RESET when due and otherwise keeps polling for frames. ACK windows use the *current* highest slot, because a pouch that joins during the Reset may raise it. A JOIN that collides with an ACK costs nothing: the next RESET asks for the ACK again, and the pouch retries its JOIN after 100–300 ms.

Side effect to accept (Q3): **adding a player resets everyone's lives**. That is natural as a "new round", but it is a game rule, not just plumbing.

### A7. Roll Call (ball)
Unchanged: leave on short press or `ROLLCALL_TIMEOUT_MS` after the last *new* pouch, never before one has registered. On leave call the `Disarm()` function, not a direct transition to Unheld: the ball may be in the player's hand, in which case it goes straight to Ready.

---

## 3. Part B: pouch FSM

Not in FSMs.md today; add it there.

| State | Radio | LEDs (details in Part E) | Entered from |
|-------|-------|--------------------------|--------------|
| **Joining** | sends JOIN at random 100–300 ms intervals; listens | blue fast blink | power-up; Listening after RESET (after a random delay); Joined/Active after loss (A5) |
| **Listening** | silent (no TX) | blue short flash every 2 s (replaces the all-LED "failure" flash, Q12) | Joining when a POLL is heard (ball already ranging, so no ASSIGN is coming) |
| **Joined** | answers nothing yet | blue slow blink | REMEMBER with my uid (from Joining or Listening) |
| **Active** | answers POLL, HIT, RESET; applies REMEMBER | lives pattern | first POLL/HIT/RESET while Joined |

Transitions:
* Joining → Joined: REMEMBER with my uid.
* Joining → Listening: POLL heard.
* Listening → Joined: REMEMBER with my uid (rebooted mid-game).
* Listening → Joining: RESET heard (after a random delay `REJOIN_DELAY_MAX_MS`, ~0–1 s, so several late pouches don't collide).
* Joined/Active → Joining: no REMEMBER for `LOST_TIMEOUT_MS`.
* Joined/Active on RESET: lives := max, send RESET_ACK in my slot (unchanged), then wait.

"Rejoining" (Changes.md's *"wait random delay and try to join until hear ranging requests again"*) is not a separate state: it is Joining entered from Listening, and it ends the same way (POLL heard → Listening).

To avoid noise on ranging, the first JOIN after power-up should also wait a random 100–300 ms (today it fires immediately), so a rebooting pouch can hear whether the ball is already ranging.

### A8. User-facing flows
| Goal | Steps |
|------|-------|
| Normal start | Ball and pouches on in any order; short press (or 15 s timeout) |
| Pouch rebooted / battery bumped | Nothing; it rejoins within ~1 s with its lives |
| Add a player mid-game | Switch their pouch on (it shows "waiting"), **hold the ball button 3 s**; everyone's lives reset; Roll Call gives time for more pouches; short press to resume |
| Remove a player | Switch off; they are deleted at the next Reset |

---

## 4. Part C: accelerometer classification

### C1. What the data says
The recorded flights (`data/ball_20261119_150548.csv`, BURST #8 and others) do **not** match "free fall = \|a\| < 700 mg":
* Before a landing, \|a\| is ~1000–1340 mg and drifts smoothly: a ball spinning in flight, never below 700, so the old free-fall gate can't see the flight.
* Then a 33 g spike (sensor clipped at 32767), then ~3.8 g held **flat** for 150+ ms (spinning/rolling after the bounce).
* In that log, 8 of 11 threshold crossings were rejected for "no free fall just before" (ffage 173–2038 ms), including 2.5–33 g events during flights. Some were probably real landings and bounces.

This supports the new design (the FSM state, not a free-fall timer, says "the ball is flying") and shows the main risk: **Spinning cannot be defined by "≠ 1000 mg"**. A resting ball reads ~1000 mg with only sensor noise (tens of mg); a flying ball can read ~1000 mg too, but moves smoothly. The reliable separators are **variance / smoothness over a short window**, plus **FSM gating** (Spinning only reachable from Held or after a Hit; Rest only after things settle).

### C2. Interface (replaces the free-fall gate in `ball/src/impact.c`)
* **Level state:** `REST | HELD | SPINNING | UNKNOWN` (`UNKNOWN` = sensor stalled; keep the existing stall recovery), plus time-in-state.
* **Edge event:** `HIT` (sharp spike/edge), queued as today. A Hit is an *event*, not a persistent state; your example trace
  `Rest → Held → (Hit) → Spinning → Hit → Spinning → Hit → Rest` is a sequence of states interleaved with events.
* **Keep:** sampling thread, stall recovery, BURST logging, `impact_flush`.
* **Drop:** `IMPACT_AFTER_FREEFALL_MS` gating and the `impact_freefall_*` helpers. Free fall (|a| ≈ 0, lobs) becomes one way to be `SPINNING`.
* Classification uses windowed features (~100–200 ms of \|a\|: mean, peak-to-peak or stddev, per-sample jump). Exact thresholds come from recorded data, not guesses.

### C3. Build it data-first
Write the classifier as **pure C** (like `shared/twr.h`) and test it on the host against recorded traces (`data/ball_*.csv` BURST lines, `data/accel_*.csv` raw x/y/z), plus a few new labeled captures (rest, hand-held, walking, throw, bounce chains, bump while held). The example trace above becomes a regression test.

---

## 5. Part D: ball FSM v2

States: Off, Roll Call, **Unheld**, **Ready**, **Thrown**, **Hit**, Reset.
(Off is still not a code state. **Unheld** means nobody is holding the ball, so hits can't register. A pouch with 0 lives is called "out of lives", Q11.)

### D1. Functions
**`Disarm()`**: range once (retry like Classify if nobody answers). If an alive pouch (lives > 0) is within pickup range **and the accelerometer is Held** → **Ready** (thrower = closest). Otherwise → **Unheld**.
* **[Added] The Held requirement.** Without it, after a hit the ball (still bouncing within ~1 m of the pouch it hit) would go straight back to Ready, and a second bounce could register as a second hit. Requiring Held means a bouncing or resting ball waits in Unheld until a player actually picks it up. After Roll Call (ball in hand) it still works; if the ball is on a table it simply goes Unheld and is picked up normally.

**`Classify()`**: range once (up to `CLASSIFY_ATTEMPTS`), take the **closest pouch, alive or not** (Q6), then:
* no pouch within hit range (`HIT_DISTANCE_MM`) → miss → **Unheld**
* closest pouch is **out of lives** → miss → **Unheld**. It is deliberately not skipped in favour of the next-closest living pouch: hitting a player who is already out must not cost the players around them a life.
* closest is the thrower → **Ready**
* closest is another pouch with lives → **Hit** (that pouch)
* Keep the single-pouch bench rule (`SELF_HIT_IF_SINGLE_POUCH`): with exactly one pouch, the thrower can be hit.

### D2. Unheld
Ranging constantly, as today. An alive pouch within pickup range **and** accel `HELD` → **Ready**. Keep the existing multi-round confirmation (`TRANSITION_CONFIRM_ROUNDS`) unless you want it gone.
Replaces the `PICKUP_MOTION_MS` motion check with the Held state.

### D3. Ready
Ranging constantly. Thrower := closest alive pouch at entry.
* **[Revised, Q19] Only the thrower's range matters for throwing.** Ready does not check where the other pouches are, so a player standing near the flight path cannot block a throw.
* **[Revised, Q19] Possession changes by timeout, not by distance flip.** The thrower is replaced by another alive pouch only if that pouch has been closer to the ball than the current thrower for `THROWER_SWITCH_MS` (1 s initial) without a break. This replaces the "N rounds or margin" hysteresis, still stops two kids standing close together from flipping the thrower every round, and covers a deliberate hand-over. Mid-air passes are out of scope (see Gameplay assumptions).
* **Silent after very close** (ranging fails when ball and pouch nearly touch): keep the existing `TOO_CLOSE_MM` rule: treat the thrower as still in range at ~0 mm. This feeds the Thrown test and the "thrower beyond throw range" test below, or a held ball would drop to Unheld.

Transitions, in this priority order:
1. `SPINNING` **and** the thrower is farther than `THROW_RANGE_MM` (2 × pickup range = 2 m) → **Thrown**.
2. No alive pouch within throw range for `READY_ORPHAN_MS` without Spinning (someone without a pouch is carrying the ball away), or accel not `HELD` for `READY_NOT_HELD_MS` → **Unheld**. Your "and/or" is read as OR with a debounce; it must not override rule 1 (Spinning also means "not Held"). `READY_NOT_HELD_MS` can reuse `READY_STILL_TIMEOUT_MS` (5 s).

Ready ignores `HIT` events entirely (the launch spike, bumps in the hand). Unlike today's code, an impact while still in Ready is **not** classified: point-blank throws are out of scope (Q5, option C; see [Gameplay assumptions](#gameplay-assumptions)).

### D4. Thrown
Ranging only every `ARMED_RECHECK_MS` (250 ms) as today.
* `HIT` event, **or** accelerometer unresponsive (stall; it often follows a hard shock) → `Classify()`.
* `THROW_TIMEOUT_MS` (3.5 s, replaces 6 s) without a Hit → `Disarm()`.
* **[Revised, Q19]** The closest alive pouch stays within throw range for `THROWN_RETURN_MS` (1 s, 4 re-checks) with no Hit → **Ready** with that pouch as thrower. If it is the thrower, the throw is cancelled or the ball came back; if it is another pouch, **possession changes** because the ball ended up nearer to someone else (the only way possession changes in Thrown; there are no passes). Replaces the old `REARM_*` rule (thrower within 700 mm for 2 re-checks). Catches are out of scope (see Gameplay assumptions). **Decided (Q19): straight to Ready, with no Held check** (matches Changes.md). Accepted consequence: a ball that simply lies on the ground near a pouch sits in Ready until `READY_NOT_HELD_MS` (5 s) sends it to Unheld. It cannot re-trigger Thrown meanwhile, because Thrown needs the (new) thrower to be beyond the throw range.
* Optional shortcut: accel `REST` for N ms → `Disarm()` without waiting out the 3.5 s.

### D5. Hit
No ranging.
* On entry: **decrement the ball's lives for the hit pouch (if > 0), immediately**, regardless of whether the pouch ever acknowledges.
* Send HIT(lives remaining) until HIT_ACK with that value, or `INFORM_TIMEOUT_MS` (1 s) → `Disarm()` in both cases.
* If the pouch missed it, REMEMBER (A4) fixes its display within ~1 s.

### D6. Reset
As in A6. Entered by a 3 s hold from any state except Roll Call.

### D7. LEDs
See Part E. The binary state number on led2..led0 and the strip-color column are dropped.

---

## 6. Part E: LED patterns

### E1. Problems being solved
* **Pouch:** the two red LEDs are hard to tell apart (1 vs 2 lives), the green LED is so bright it hides the others while lit, and counting lit LEDs doesn't work from a distance.
* **Ball:** the state is shown as a binary number on three LEDs, plus an identity LED, plus flashes. That is too much to decode. The ball no longer gets a strip, so its four LEDs must carry the state clearly.
* **Hardware:** four plain GPIO LEDs per board: led0 green (D9), led1 red (D12), led2 red (D11), led3 blue (D10). There is no dimming without extra work (Q18). Effectively three colors, because the two reds look alike.

### E2. Principles
1. **One color at a time.** Different LEDs never compete, so green's brightness stops mattering. Where "red" is used, light **both** red LEDs together (twice the light, closer to green's brightness). Only the explicit effects below (Hit, Reset) light several colors.
2. **Color is never the only cue.** About 8% of boys are red-green color blind and the target user is a 10-year-old boy, so patterns also differ by **steady vs. blinking** wherever possible. One known exception: out of lives (red steady) and 3 lives (green steady) differ by color alone, which a red-green color-blind player may not tell apart (see Q13).
3. **Steady = a lasting condition. Blinking = needs attention or is in progress.**
4. **No binary codes.** Ball state number and pouch slot number leave the LEDs (both stay in the log).
5. **Table-driven.** Patterns are defined as data in one shared header and driven by timestamps, so remapping a color is a one-line change. The pouch drives its LEDs through a small display layer (inputs: phase, lives, hit event), so the LED strips can replace the LED backend later without touching the FSM.

Timing: slow = 1 Hz (500 ms on / 500 ms off), fast = 4 Hz (125 / 125), "flash" = 100 ms on.

### E3. Pouch (temporary until the strips arrive)
| Situation | Pattern | Note |
|-----------|---------|------|
| Self-test pass | green blinks 3 times | unchanged |
| Self-test fail | both reds blink slowly, forever | unchanged |
| Joining | blue fast blink | searching |
| Joined | blue slow blink | in the table, waiting for the game to start |
| Listening | blue flash every 2 s | not in the game; waiting for someone to Reset (Q12) |
| Active, 3 lives | **green steady** | |
| Active, 2 lives | **blue steady** | never confused with Joined (that one blinks) |
| Active, 1 life | **red (both) fast blink** | last life: the blink is the cue even if red and green look alike |
| Active, out of lives | **red (both) steady**, like a flatline | solid red = out = don't throw at them (matches Q6); a steady red cannot be mistaken for the blinking 1-life pattern |
| Just hit (lives went down) | all four LEDs flash at 5 Hz for 1 s, then the new lives pattern | the "toy confirms the hit" moment; opponents see it too |
| RESET received | all four on for 0.5 s, then the 3-lives pattern | optional |

Blue therefore means connection status when it blinks and 2 lives when steady. Green and red mean lives only. The patterns assume `LIVES_MAX` = 3; the strips will show N pixels for N lives and remove this limit. Alternative scheme for Q13: traffic-light colors (green / amber = green + red / red).

### E4. Ball
Players look into the ball only in **Roll Call, Reset, Ready and Unheld** (the last when they aren't wearing a pouch), and from an angle. Those four get one simple, mutually distinct pattern each. Nobody reads Thrown or Hit, so they stay minimal, but still differ so benches and onlookers can follow.

| State | Pattern | Note |
|-------|---------|------|
| Startup self-test | pass: green 3 blinks. Fail: both reds blink forever | unchanged |
| **Roll Call** | **blue steady**; green flashes 150 ms for each new pouch (existing flash); red flash if a short press is ignored because nobody has registered | |
| **Unheld** | **green steady** | idle, safe |
| **Ready** | **red (both) steady** | armed |
| Thrown | red (both) fast blink | same family as Ready: steady = in hand, blinking = in flight |
| Hit | red + blue steady while informing (at most 1 s) | |
| **Reset** | **all four LEDs blink together 3 times** (0.5 s on / 0.5 s off, existing timing) | the only pattern that uses all four, so it can't be mistaken |

Color families: green = idle, red = armed, blue = setup. The identity LED goes away: ball vs. pouch is obvious from the hardware.

### E5. What code can't tell us: check on hardware
1. **Viewing angle.** Which LEDs can be seen from inside the ball at the player's angle? The green and blue states each depend on a single LED (red has two). If one is hidden, swap state colors in the table (Q17).
2. **Relative brightness** of green, blue and red at 5–10 m outdoors. If steady green (3 lives) is far more visible than steady blue (2 lives), dim green with PWM (Q18).
3. **Fast blink legibility** in sunlight at distance.
Do this as soon as the pouch patterns exist (step 1) and again with the ball assembled.

---

## 7. Part F: optional logging

### F1. Why it needs switches
Today every `LOG()` line goes to **two** places ([ble_log.c](../ball/src/ble_log.c)): the console (`printk`, 115200 baud, about 11 KB/s at most) and, if a laptop is subscribed, a queue of 48 lines (160 bytes each) that a low-priority thread sends over Bluetooth. When the queue is full the line is **dropped and only counted** (BURST numbers show gaps). The new design adds more lines (classifier levels, membership events, REMEMBER traffic), and step 3 would like raw accelerometer samples, so "log everything" will not fit.

Two cost sources, both to be measured before choosing defaults:
* **Bluetooth throughput:** how many lines per second the link really sustains. Measure with every flag on and watch the dropped counter.
* **The console may block the ball's own loop.** At 115200 baud a ~100-character line takes roughly 9 ms to print, about the length of a Ready ranging round (~20 ms). If `printk` is synchronous here (to be verified), heavy logging slows ranging and can itself make detection worse. The flags must therefore gate the console print too, not only the Bluetooth queue.

Rough rates today (line lengths estimated, not measured): `STATUS` 4 lines/s; `RANGE` up to 5 rounds/s × one line per pouch, so about 40 lines/s with 8 pouches; `BURST` about 11 lines per threshold crossing.

### F2. Design
* A new **Logging** section in `shared/settings.h`: one `0/1` flag per category plus the period of the periodic ones (`STATUS_LOG_PERIOD_MS`, `RANGE_LOG_PERIOD_MS` move there from `ball/src/main.c`).
* A `LOG_CAT(flag, fmt, ...)` macro replaces `LOG()`. A disabled category compiles to nothing: no formatting, no console print, no queue slot.
* Categories (defaults are initial guesses, to be set after the F1 measurements):

| Flag | Lines | Default | Rate |
|------|-------|---------|------|
| `LOG_STATE` | `State: A -> B`, Roll Call registrations, Reset summary, `Classify:` decisions, `HIT` / `HIT_ACK` / `HIT_UNACKED` (the game events) | on | a few per minute |
| `LOG_STATUS` | the `STATUS` line (gets a `log_dropped=` field so loss is visible) | on | 4/s |
| `LOG_RANGE` | `RANGE` lines | off | up to 5/s per pouch |
| `LOG_MEMBERSHIP` | JOIN, slot assigned, pouch deleted, pouch phase changes; **events only**, not every REMEMBER sent | on | rare |
| `LOG_REMEMBER` | every REMEMBER sent/received (protocol debugging) | off | up to 8/s |
| `LOG_ACCEL_CLASS` | classifier level changes (Rest/Held/Spinning/Unknown, with time in state) and Hit events | on | on change |
| `LOG_ACCEL_BURST` | existing `BURST` windows | off | ~11 lines per crossing |
| `LOG_ACCEL_FEATURES` | the classifier's window features (mean, peak-to-peak, ...) for threshold tuning | off | per window |
| `LOG_ACCEL_RAW` | continuous raw samples for the labeled captures of step 3 | off | 200 Hz |

* **Raw capture must not go through the line queue.** At 200 Hz even a compact line format overflows it. Instead, record into a RAM ring buffer (200 Hz × 6 bytes for x/y/z is about 1.2 KB/s, so a 10 s capture is about 12 KB) and dump it after the capture at a rate the link can take, as the existing BURST dump already does for its short windows. Exact mechanism is decided in step 3.
* **Pouch:** has no Bluetooth, so its flags only gate console output. It logs **events only** (phase changes, hits, resets), never periodically, so a console print cannot delay a reply in its timeslot.
* **Tools:** keep the existing line prefixes (`State:`, `STATUS`, `RANGE`, `HIT`, `Classify`, `BURST`) so `tools/ball_monitor.py` and its tests keep working. New lines get a prefix of their own (e.g. `ACCEL`, `MEMBER`), and the monitor learns to colour them in step 5.

---

## 8. Settings

| New | Value (initial) | Note |
|-----|-----------------|------|
| `THROW_RANGE_MM` | 2 × `PICKUP_DISTANCE_MM` | derived, not independent; measured from the **thrower** only (Q19) |
| `THROW_TIMEOUT_MS` | 3500 | replaces `ARMED_TIMEOUT_MS` |
| `THROWN_RETURN_MS` | 1000 | closest pouch in range → Ready (same thrower, or possession change) |
| `READY_ORPHAN_MS`, `READY_NOT_HELD_MS` | ~1000, 5000 | Ready → Unheld |
| `THROWER_SWITCH_MS` | 1000 | another pouch closer than the thrower this long → new thrower (replaces `THROWER_SWITCH_ROUNDS`) |
| `LOG_*` flags and `*_LOG_PERIOD_MS` | see F2 | new "Logging" section |
| `REMEMBER_PERIOD_MS` | 1000 | per pouch |
| `LOST_TIMEOUT_MS` | 5000 | pouch fallback |
| `REJOIN_DELAY_MAX_MS` | 1000 | |
| LED timings and pattern table | slow 500/500, fast 125/125, flash 100, hit effect 1000 | one shared header (E2) |
| accel classifier thresholds | from data | window length, rest/spin variance limits |

Removed: `LEAVE_DISTANCE_MM`, `ARM_CONFIRM_ROUNDS`, `REARM_*`, `ARMED_TIMEOUT_MS`, `FREEFALL_*`, `IMPACT_AFTER_FREEFALL_MS`, `PICKUP_MOTION_MS`.

---

## Implementation steps

Each step leaves a working, bench-testable system. Update the relevant READMEs and FSMs.md **in the same step**.

1. **Ball-authoritative lives + REMEMBER + new pouch FSM** (keeps the old ball states), **plus the pouch display layer and new pouch LED patterns (E3), plus the logging flags (Part F: `LOG_CAT` macro, existing ball log lines sorted into categories, `log_dropped=` in STATUS)**. Touches `shared/uwb.[ch]`, `shared/settings.h`, a new shared LED-pattern header, ball and pouch `main.c`. Bench: reboot a pouch mid-game; any power-on order; unacked hit corrected by REMEMBER; ball reboot recovery (A5); LED legibility check (E5) from a distance.
2. **Reset as join window + deletion + Roll Call re-entry** (a Reset-specific service loop that listens for the full 3 s, A6), **plus the ball's Roll Call and Reset LED patterns (E4)**, since those states already exist. Bench: late pouch → hold button → joins; a pouch that joins in the last blink is not deleted; silent pouch deleted and its slot reused; ranging round shortens; all-silent → Roll Call; Reset with an empty table still accepts a JOIN.
3. **Accelerometer characterization → pure-C classifier + host replay tests.** *Can start immediately and in parallel with 1–2:* the new captures take the longest. Includes the `LOG_ACCEL_*` categories and a raw-capture mode (RAM buffer, dumped afterwards, F2). No FSM change yet.
4. **Ball FSM v2** (Unheld/Ready/Thrown/Hit, `Disarm`/`Classify`, HIT with lives, remaining ball LED patterns from E4; check the viewing angle with the ball assembled). Bench: pick-up, throw, hand-over (thrower changes after `THROWER_SWITCH_MS`), hit then bounce (must be a single hit), miss, hit on a pouch that is out of lives (expected: miss, nobody loses a life), and a throw shorter than the throw range (expected: no hit, ball times out to Unheld). Record real flights with `LOG_RANGE` on to measure the detection window (Risk 6).
5. **Docs and tools:** FSMs.md rewrite (ball v2 and new pouch FSM), root README (gameplay: replace the "turn everything off" paragraph and the roll-call TODO, and state the no-catch / no-pass / minimum-throw-distance assumptions), `ball/`, `pouch/`, `shared/` READMEs (messages table, state tables, LED tables; drop the identity-LED, binary-state and binary-slot text, and the ball's "strip color" columns and "Light Strip is ..." lines in FSMs.md; the root README still says *both* devices use WS2812 strips, which is no longer true for the ball), `tools/ball_monitor.py` and `tests/test_ball_monitor.py` (they key off `State:`, `STATUS ... state=`, `HIT`, `Classify` strings: keep those formats where possible).

### Host tests to add (pure C, same pattern as `tests/twr_test.c`)
* Membership table: join / repeat / delete / slot reuse / highest-slot recompute / REMEMBER round-robin.
* Accel classifier replay on recorded traces and the example trace.
* `Disarm()` / `Classify()` decision logic extracted from `main.c`.

---

## Risks
1. **Accelerometer classification** is the main unknown. Spin vs. rest overlap in magnitude, the sensor clips at 32767 mg and stalls after hard shocks. Mitigation: step 3 is data-first and independent.
2. **Removing the free-fall gate** could admit false hits from bumps. Mitigation: a Hit only counts in Thrown (which requires Spinning first), never in Unheld or Ready.
3. **Radio contention.** REMEMBER and JOIN share the channel with ranging. Mitigation: ball sends only between exchanges; pouches only transmit in Joining, and the first JOIN is delayed so a rebooting pouch hears POLLs first.
4. **Two-board bench limits.** Multi-pouch behaviours (thrower hysteresis, classify with a non-thrower, deletion with several pouches) need 3+ boards to be exercised fully.
5. **LED legibility is unverified.** The scheme is reasoned from the hardware (three colors, one very bright green) but not seen in sunlight, at distance, or from inside the ball. Mitigation: table-driven patterns (a color swap is a one-line change) and the E5 checks early in step 1.
6. **Short-throw detection window (Q19).** A throw is detected only while the ball is Spinning and more than `THROW_RANGE_MM` (2 m) from the thrower. For a target 3 m away that is the last ~1 m of the flight, roughly 0.1 s at an assumed 8 m/s (an estimate, not measured), i.e. a few ranging rounds (a Ready round takes ~20 ms). Short or slow throws may be missed and their hit ignored. Mitigation: measure real flights in step 4 with `LOG_RANGE` on; if needed, lower `THROW_RANGE_MM` or latch Spinning for ~300 ms (the old free-fall check used such a timestamp).
7. **Logging can disturb what it observes.** Heavy logging may block the ball's loop (console) or drop lines (Bluetooth queue). Mitigation: per-category flags with the heavy ones off by default, `log_dropped=` in STATUS, events-only logging on the pouch (Part F).

---

## Decisions to confirm

Recommendation first; all are defaults in the plan above unless you say otherwise.

| # | Question | Recommendation | Choice |
|---|----------|----------------|--------|
| Q1 | Merge ASSIGN into REMEMBER (one message)? | Yes | Yes |
| Q2 | Reset ends in Roll Call if anyone joined (vs. straight to `Disarm()`)? | Yes | Yes |
| Q3 | Accept "adding a player resets everyone's lives"? | Yes (new round) | Only possible during a reset, which already resets lives |
| Q4 | `Disarm()` requires accel Held before going Ready? | Yes (prevents double-hit on bounce) | Yes |
| Q5 | Add Ready → `Classify()` on Hit-after-Spinning (point-blank hits)? | Yes | **No (option C):** point-blank throws are not wanted. Documented in Gameplay assumptions; with the thrower-only rule (Q19) the minimum throw distance is a bit over 2 × pickup range (about 2 m) |
| Q6 | Pouches with 0 lives ignored for pickup **and** hit classification? | Yes (today only pickup ignores them) | If closest pouch is a pouch with 0 lives, treat as a miss (→ Unheld, like any `Classify()` miss), so hitting a player who is out doesn't deduct lives from those around them |
| Q7 | Pouch falls back to Joining after 5 s without its REMEMBER? | Yes (needed for ball reboot and deleted pouches) | Yes, make 5s a variable in settings |
| Q8 | Drop `hit_seq` now that HIT carries absolute lives? | Yes | Yes |
| Q9 | REMEMBER as a trickle, not a wave? | Trickle | Trickle |
| Q10 | Ball reboot loses all lives (RAM only; no flash persistence)? | Accept | Accept |
| Q11 | Naming: keep "Dead" for the ball state? | Keep; say "out of lives" for pouches | **Unheld** (state). `Disarm()` keeps its name for now; pouches with 0 lives are "out of lives" |
| Q12 | Pattern for the Listening pouch (replaces the "failure" flash)? | Calm, distinct pattern; your call on the look | Yes |
| Q13 | Pouch lives scheme (E3): green = 3, blue = 2, red fast blink = 1, red steady = out? (Alternative: green / amber / red traffic light) | Green / blue / red blink / red steady: no mixed colors. Known gap: out (red steady) vs. 3 lives (green steady) is color-only for a red-green color-blind player | **Red steady for out of lives** (flatline) |
| Q14 | Ball patterns as in E4 (Roll Call blue, Unheld green, Ready red, Reset all four blinking)? | Yes | Yes |
| Q15 | Drop the identity LED and the binary state/slot codes? | Yes (state stays in the log) | Yes |
| Q16 | Add the "just hit" flash on the pouch (and the ball's red + blue Hit pattern)? | Yes (it is the hit confirmation the game promises) | Yes |
| Q17 | Which of the four LEDs are visible from inside the ball at the player's angle? | Needs your answer; it decides which state gets which color | All (plate reflects them all out the opening) |
| Q18 | Dim green with PWM? | Not yet: one color at a time already removes the glare problem. Only if the E5 brightness check shows green far outshining blue/red. It needs an overlay/pinctrl change | Not yet |
| Q19 | Thrown is entered when Spinning and the **thrower** is beyond the throw range (2 m); other pouches are not checked. The thrower changes only when another pouch has been closer for a timeout (`THROWER_SWITCH_MS` in Ready, `THROWN_RETURN_MS` in Thrown); no mid-air passes | Yes: removes the "bystander near the flight path" problem and halves the minimum throw distance (about 4 m → 2 m) | Your proposal. **Decided:** Thrown → Ready on a possession change goes straight to Ready, with no Held check (Part D, section D4). See Risk 6 for the shorter detection window |
| Q20 | Per-category logging flags in `settings.h` (Part F)? | Yes | Yes |

### Interpretations of Changes.md to confirm - All Correct
* Pouch "if fail" = hears ranging (POLL) before receiving a slot.
* Pouch "RESET" = the ball's Reset broadcast, used as the signal that joins are accepted.
* "No ACK for 3 s → delete" = no ACK at all during the Reset state (3 s = the 3 blinks).
* "Dead if no pouch in range and/or accel ≠ Held" (Changes.md's original wording; the state is now Unheld) = OR, debounced, lower priority than Thrown.
* "Close pouch stops replying → stay Ready" keeps the existing `TOO_CLOSE_MM` memory logic.
