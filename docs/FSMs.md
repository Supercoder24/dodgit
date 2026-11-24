# Finite State Machine Diagrams
Originally transcribed from Felix's iPad; rewritten for the rejoinable game (see [Plan.md](Plan.md) for the reasoning and [Changes.md](Changes.md) for the original notes).

Every number below is a setting in [`shared/settings.h`](../shared/settings.h); the value shown is the current default. The decision rules of the ball are pure functions in [`ball/src/ball_logic.h`](../ball/src/ball_logic.h) (host test: `tests/ball_logic_test.c`), the pouch's in [`shared/pouch_fsm.h`](../shared/pouch_fsm.h) (host test: `tests/pouch_fsm_test.c`).

## Gameplay assumptions
Deliberate scope limits of the prototype, not bugs. They are why some impacts are ignored.
* **No catches.** Players do not catch the ball. It is picked up from the ground or handed over deliberately. A catch is not detected as such: its impact spike would most likely be scored as a Hit on the catcher.
* **No point-blank throws.** Ready never scores a Hit. Thrown is entered only when the ball is Spinning **and** the **thrower** is farther than `THROW_RANGE_MM` (2 × `PICKUP_DISTANCE_MM` = 2 m). A throw therefore has to travel more than 2 m before it lands: a shorter throw never enters Thrown, its impact is ignored, nobody loses a life, and the ball times out to Unheld. Only the thrower's range matters, so a player standing near the flight path does not block a throw. To allow shorter throws lower `PICKUP_DISTANCE_MM` (the throw range follows it) or decouple the two.
* **No passes.** Players do not pass the ball in the air. Possession changes only by a deliberate hand-over, or when the ball ends up closer to another player's pouch than to the thrower's for a while (`THROWER_SWITCH_MS` in Ready, `THROWN_RETURN_MS` in Thrown).
* **The short-throw detection window is short.** For a target 3 m away only the last ~1 m of the flight (about 0.1 s) counts as "beyond the throw range"; measure real flights with `LOG_RANGE` on and, if throws are missed, lower `THROW_RANGE_MM` or latch Spinning for a few hundred ms (Plan.md, Risk 6).

## Ball Main FSM
States: Off, Roll Call, **Unheld**, **Ready**, **Thrown**, **Hit**, Reset. `Disarm()` and `Classify()` are functions, not states: each ranges once and decides the next state.

```
                       short press / ROLLCALL_TIMEOUT_MS
  Off ──► Roll Call ───────────────────────────────────► Disarm() ──► Unheld  (not Held / nobody near)
             ▲                                              ▲   └───► Ready   (alive pouch near AND Held)
             │ table empty, or a NEW pouch joined           │
             │                                              │ timeout / Rest / Hit done / Reset
           Reset ◄── button held 3 s (any state but Roll Call)

  Unheld ──pickup (alive pouch < 1 m, Held)──► Ready ──Spinning and thrower > 2 m──► Thrown
  Ready  ──nobody in range / not Held──► Unheld         Thrown ──Hit event or sensor stall──► Classify()
  Thrown ──timeout / Rest──► Disarm()                   Thrown ──same pouch within 2 m for 1 s──► Ready
  Classify() ──miss──► Unheld   ──closest is the thrower──► Ready   ──closest is another pouch──► Hit ──► Disarm()
```

### Off
From: initial state. To: Roll Call. During: nothing.
When the power switch is flipped on, the ball runs its self-test (UWB radio probe, accelerometer init: green blinks 3 times, or both reds blink forever on failure) and enters Roll Call. Not a state in the code: it is the setup that runs before the state machine.

### Roll Call
From: Off, Reset. To: `Disarm()` (so Ready or Unheld).
* Listens for JOIN from pouches; each registers in the lowest free timeslot (up to 8) and is told its slot and lives (REMEMBER). A repeated ID gets the same slot; a ninth pouch is ignored and does not restart the timeout. See [shared/README.md](../shared/README.md).
* Leaves on a short press of the button, or `ROLLCALL_TIMEOUT_MS` (15 s) after the last *new* pouch registered (after Roll Call began, if it was re-entered from Reset). Neither happens before at least one pouch has registered (an ignored press flashes red).
* Holding the button does nothing here (Reset needs a game to reset).
* On leaving it calls `Disarm()`, not a direct transition: the ball may already be in a player's hand.
* LEDs: blue steady; green flash for each new pouch.

### Unheld
From: `Disarm()`, `Classify()` (miss), Ready, Reset. To: Ready.
Nobody is holding the ball, so Hit events cannot register (they are drained and ignored).
* Constantly ranging.
* An alive pouch (lives > 0) within `PICKUP_DISTANCE_MM` (1 m) **and** the accelerometer reads **Held** for `TRANSITION_CONFIRM_ROUNDS` (2) consecutive rounds → **Ready**, with that pouch as the thrower. A single failing round restarts the count. A ball lying or bouncing next to a pouch is never "picked up".
* LEDs: green steady.

### Ready
From: Unheld, `Disarm()`, `Classify()` (closest is the thrower), Thrown (return rule). To: Thrown, Unheld.
A player holds the ball. Constantly ranging. **Hit events are ignored entirely** (the launch spike, bumps in the hand and point-blank throws are not hits).
* **Thrown** (highest priority) when the ball is **Spinning** and the **thrower** is farther than `THROW_RANGE_MM` (2 m). Other pouches are not checked.
* **Unheld** when no alive pouch is within `THROW_RANGE_MM` for `READY_ORPHAN_MS` (1 s) without Spinning (somebody without a pouch is carrying the ball away), or when the ball is not Held for `READY_NOT_HELD_MS` (5 s: put down or dropped).
* **Possession changes by timeout, not by a flip:** another alive pouch that has been closer to the ball than the current thrower for `THROWER_SWITCH_MS` (1 s) becomes the thrower. Two players standing close together do not swap it every round; a hand-over does. Only *evidence* interrupts the streak (the pouch was heard and is not closer, or another pouch is, or it was not heard at all for `THROWER_SWITCH_GRACE_MS` = 300 ms): a dropped round does not. This matters because a pouch is missing from about 8% of the rounds when it is within 300 mm of the ball (measured in `data/`), which is exactly the distance of a hand-over.
* **Missing is not far:** a thrower that does not answer after a reading ≤ `TOO_CLOSE_MM` (300 mm) is treated as still right there (0 mm), because ranging fails when ball and pouch nearly touch: a held ball neither drops to Unheld nor counts as thrown. After a farther reading a short silence keeps the last range, and only a silence of `THROWER_SILENT_MS` (100 ms) or more counts as far away, so one dropped round while the ball spins cannot start Thrown.
* LEDs: red steady.

### Thrown
From: Ready. To: `Classify()`, `Disarm()`, Ready.
The ball is in flight. Ranging only every `THROWN_RECHECK_MS` (250 ms), otherwise the radio is idle.
* A **Hit** event from the accelerometer, **or** the accelerometer going silent (it often stalls after a hard shock) → `Classify()`.
* No hit for `THROW_TIMEOUT_MS` (3.5 s) → `Disarm()`.
* The accelerometer reads Rest for `THROWN_REST_MS` (1 s): the ball landed and lies still → `Disarm()` without waiting out the timeout.
* The same alive pouch has been the **closest** one within `THROW_RANGE_MM` for `THROWN_RETURN_MS` (1 s) without a hit (a re-check in which it is missing does not break the streak; three in a row do) → straight back to **Ready** with that pouch as the thrower (no Held check). If it is the thrower the throw was cancelled or the ball came back; if it is another pouch, **possession changes** because the ball ended up nearer to someone else (the only way possession changes in flight). A ball that simply lies on the ground near a pouch therefore sits in Ready until `READY_NOT_HELD_MS` sends it to Unheld; it cannot re-trigger Thrown meanwhile, because Thrown needs the new thrower to be beyond the throw range.
* LEDs: red fast blink (same family as Ready: steady = in hand, blinking = in flight).

### Hit
From: `Classify()`. To: `Disarm()`. No ranging.
* On entry the ball **decrements its own lives table** for the hit pouch (if > 0), immediately, whether or not the pouch ever acknowledges.
* Sends HIT with the lives remaining until the pouch acknowledges that value, or `HIT_ACK_TIMEOUT_MS` (1 s) → `Disarm()` in both cases. A pouch that missed the HIT is corrected by the next REMEMBER (within about a second).
* LEDs: red + blue steady while informing.

### Reset
From: any state except Roll Call (button held for 3 s, fires while still held). To: Roll Call, `Disarm()`.
The **join window**: how a pouch that was switched on late joins without turning anything else off.
1. All lives in the ball's table := `LIVES_MAX`; nobody has acknowledged yet.
2. For the whole `RESET_WINDOW_MS` (3 s; not only until everybody has answered, and also with an empty table) the receiver is armed and takes whatever arrives: a RESET_ACK from a known slot, or a JOIN from a pouch without a slot.
3. RESET is broadcast every `RESET_RETRY_MS` (200 ms) until every pouch in the table has acknowledged, then every `RESET_KEEPALIVE_MS` (500 ms) for the rest of the window: a late (Listening) pouch reacts to a RESET only, so one lost broadcast must not leave it out of the game. Known pouches just acknowledge again (and their display plays the reset effect once).
4. A JOIN registers the pouch in the lowest free slot (a known ID keeps its slot), answers with REMEMBER and counts as an acknowledgement, so a pouch that joins in the last 200 ms is not deleted.
5. At the end every pouch that neither acknowledged nor joined is **deleted** (slot freed, ranging rounds shorten). Safe, because everybody's lives were just reset: nobody loses progress by being forgotten.
6. Next state: **Roll Call** if the table is empty or a *new* pouch joined (it restarts its timeout, so several late players have time to join; a short press or the timeout ends it), otherwise `Disarm()`.

Side effect: adding a player resets everybody's lives (a new round).
LEDs: all four blink together (0.5 s on / 0.5 s off, 3 blinks in the 3 s window), the only pattern that uses all four.

### Disarm() (function)
Ranges once (up to `RANGE_ATTEMPTS` rounds if nobody answers). If an alive pouch is within `PICKUP_DISTANCE_MM` **and** the accelerometer reads Held → Ready (thrower = the closest alive pouch), otherwise → Unheld.
The Held requirement matters: after a hit the ball is still bouncing within about a metre of the pouch it hit. Without it the ball would go straight back to Ready and a second bounce could score a second hit. A bouncing or resting ball waits in Unheld until somebody actually picks it up.

### Classify() (function)
Ranges `CLASSIFY_ROUNDS` (3) times and merges the rounds per pouch (a pouch heard in any round counts, at its smallest distance: the pouch that was just hit is very close to the ball and is silent in a single round about one time in ten), takes the **closest pouch, alive or not**, and decides:
* nobody within `HIT_DISTANCE_MM` (900 mm) → miss → Unheld
* the closest pouch is **out of lives** → miss → Unheld. It is deliberately not skipped in favour of the next-closest living pouch: hitting a player who is already out must not cost the players around them a life.
* the closest is the thrower → Ready
* the closest is another pouch with lives → Hit
* bench-test rule (`SELF_HIT_IF_SINGLE_POUCH`): with exactly one pouch registered the thrower can be hit.

## Accelerometer inputs
The ball reads three things from [`ball/src/impact.h`](../ball/src/impact.h): the **level** (Rest ~1000 mg lying on the ground, Held = irregular values while carried, Spinning = smooth values in flight or rolling, Unknown while the sensor is stalled), **Hit events** (a sharp spike, one per physical impact) and **stall events**. Only Thrown acts on events; every other state drains and ignores them. How the classifier works and was tuned: [Accelerometer.md](Accelerometer.md).

## Pouch FSM
Implemented in [`shared/pouch_fsm.h`](../shared/pouch_fsm.h) (rules) and [`shared/uwb.c`](../shared/uwb.c) (radio glue). Nothing is terminal: a pouch can be switched on, or reboot, at any time. The ball is the source of truth for slots and lives.

| State | Radio | LEDs | Entered from |
|-------|-------|------|--------------|
| **Joining** | sends JOIN at random 100–300 ms intervals; listens | blue fast blink | power-up (first JOIN after a random 100–300 ms); Listening after a RESET (after a random 0–1 s); Joined / Active after loss |
| **Listening** | silent (no TX) | blue short flash every 2 s | Joining, when a POLL is heard (the ball is already ranging, so no answer to a JOIN is coming) |
| **Joined** | answers nothing yet | blue slow blink | a REMEMBER with its ID, from Joining or Listening |
| **Active** | answers POLL, HIT, RESET | lives pattern | first POLL / HIT / RESET while Joined |

Transitions:
* Joining → Listening: POLL heard.
* Joining / Listening → Joined: REMEMBER with my ID. A pouch that rebooted mid-game gets its **old slot and lives** back within about a second.
* Listening → Joining: RESET heard (the ball accepts new pouches), after a random delay `REJOIN_DELAY_MAX_MS` (up to 1 s) so several late pouches do not collide.
* Joined / Active → Joining: no REMEMBER addressed to this pouch for `LOST_TIMEOUT_MS` (5 s). Needed when the ball reboots (it forgets everything and hands out slots first come first served) and when the ball deleted the pouch (it still hears POLLs, so "any frame from the ball" cannot be the heartbeat).
* Listening → Joining: no POLL for `LOST_TIMEOUT_MS` (the ball rebooted into Roll Call).
* Joined / Active on RESET: lives := `LIVES_MAX`, RESET_ACK in its slot.
* Any phase on a REMEMBER with my ID: slot and lives are overwritten with the ball's values. This repairs a lost JOIN answer, a lost HIT (the pouch's display is corrected within about a second and still plays the hit effect) and a lost RESET.
* HIT carries the **lives remaining**, so a retry sets the same number again; the pouch only plays the lost-a-life effect when the value decreases.

User-facing flows:

| Goal | Steps |
|------|-------|
| Normal start | Ball and pouches on in any order; short press of the ball button (or 15 s timeout) |
| Pouch rebooted / battery bumped | Nothing; it rejoins within about a second with its slot and lives |
| Ball rebooted | Nothing; pouches fall back to Joining after 5 s and register in the new Roll Call (everyone's lives start over: the table is RAM only) |
| Add a player mid-game | Switch their pouch on (it flashes blue every 2 s), hold the ball button 3 s; everyone's lives reset; Roll Call gives time for more pouches; short press to resume |
| Remove a player | Switch their pouch off; it is deleted at the next Reset |

## LEDs
One color at a time (the two red LEDs always light together), and color is never the only cue: steady = a lasting condition, blinking = needs attention or in progress. Patterns are data in [`shared/led_patterns.h`](../shared/led_patterns.h).

| Ball state | Pattern |
|------------|---------|
| Self-test | pass: green blinks 3 times; fail: both reds blink forever |
| Roll Call | blue steady; green flash for a new pouch; red flash for an ignored short press |
| Unheld | green steady |
| Ready | red steady |
| Thrown | red fast blink |
| Hit | red + blue steady |
| Reset | all four blink together 3 times |

| Pouch | Pattern |
|-------|---------|
| Self-test | pass: green blinks 3 times; fail: both reds blink slowly forever |
| Joining | blue fast blink |
| Listening | blue flash every 2 s |
| Joined | blue slow blink |
| Active, 3 lives | green steady |
| Active, 2 lives | blue steady |
| Active, 1 life | red fast blink |
| Active, out of lives | red steady (do not throw at them) |
| Just lost a life | all four LEDs flash at 5 Hz for 1 s, then the new lives pattern |
| RESET received | all four on for 0.5 s, then the 3-lives pattern |

Known gap: out of lives (red steady) and 3 lives (green steady) differ by color alone, which a red-green color-blind player may not tell apart. The pouch patterns assume `LIVES_MAX` ≤ 3 (compile-time check); the LED strips will show one pixel per life.
