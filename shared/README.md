# shared
Code compiled into both the ball and the pouch. Nothing here is a standalone app.

The DWM3001CDK board carries a Qorvo **DW3110** UWB transceiver (DW3000 family) next to the nRF52833. The Qorvo driver and its names (`dw3000_*`, `CONFIG_DW3000`) cover the whole DW3000 family and work with the DW3110.

| File | Purpose | Host test |
|------|---------|-----------|
| `settings.h` | every tunable: game, distances, accelerometer, membership and reset timing, ball state machine, logging switches, UWB timing | - |
| `twr.h` | pure C: double-sided two-way-ranging distance math | `tests/twr_test.c` |
| `members.h` | pure C: the ball's table of pouches (ID, timeslot, lives, per-Reset `acked` flag): join, delete, hit, REMEMBER round-robin | `tests/members_test.c` |
| `pouch_fsm.h` | pure C: the pouch's membership state machine (Joining / Listening / Joined / Active) | `tests/pouch_fsm_test.c` |
| `led_patterns.h` | pure C: LED patterns of the ball and the pouch as data (color, on/off time), and the pouch's pattern for a phase and lives | `tests/led_patterns_test.c` |
| `log.h` | `LOG_CAT(flag, ...)`: a log line gated by a compile-time `LOG_*` switch in `settings.h` (a disabled category compiles to nothing); each app provides `dodgit_log()` | - |
| `uwb.h` / `uwb.c` | DW3110 setup and the whole radio protocol: the radio glue around `members.h` (ball) and `pouch_fsm.h` (pouch) | - (needs the radio) |

"Pure C" means no Zephyr and no radio, so the file also compiles on the host. `tests/run_tests.sh` builds and runs the C tests (`-Wall -Wextra -Werror`), then the Python tests in `tests/`.

The ball owns the game state: its table (`members.h`) holds each pouch's ID, timeslot and **lives**. The pouch only displays them and answers the ball's messages. The table is RAM only.

## Addresses
Ball `0x0001`; broadcast `0xFFFF`; pouch without a timeslot `0xFFFE` (the source of a JOIN); pouch in timeslot *n* `0x0100 + n`. Every frame is a short IEEE 802.15.4-style data frame (PAN `0xD048`): 10-byte header (frame control, sequence number, PAN, destination, source, 1-byte message type), the payload, and the radio's 2-byte CRC. A frame with a bad CRC, another frame control or PAN, an unknown type, or the wrong length for its type is dropped.

A pouch's ID is a 32-bit FNV-1a hash of the nRF52's factory-programmed 64-bit device ID (0 is never used).

Radio: channel 5, 128-symbol preamble, 6.8 Mbps, no STS / PDoA (`radio_config` in `uwb.c`). `ANTENNA_DELAY` (16385, a generic default) and `RANGE_OFFSET_MM` (0, subtracted from every range) in `settings.h` are not calibrated per board.

## Messages
`MSG_*` in `uwb.c` holds the type byte of each message.

| Type | Byte | From -> to | Payload | When |
|------|------|-----------|---------|------|
| JOIN | `0x20` | pouch (`0xFFFE`) -> ball | u32 ID | pouch in Joining, repeated at random `JOIN_RETRY_MIN_MS`-`JOIN_RETRY_MAX_MS` (100-300 ms) intervals; the ball only answers it in Roll Call and Reset |
| REMEMBER | `0x21` | ball -> broadcast | u32 ID, u8 timeslot, u8 lives | the answer to a JOIN, and a trickle for every pouch in the table (below) |
| POLL | `0x31` | ball -> broadcast | u8 number of slots | start of a ranging round |
| RESPONSE | `0x32` | pouch -> ball | none | in the pouch's timeslot after POLL |
| FINAL | `0x33` | ball -> broadcast | u8 mask of slots that answered | `FINAL_MARGIN_UUS` after the RESPONSEs are in (below) |
| REPORT | `0x34` | pouch -> ball | 3 x u32 timestamps | in the pouch's timeslot after FINAL |
| HIT | `0x35` | ball -> pouch | u8 lives remaining | after a hit, repeated until acknowledged |
| HIT_ACK | `0x36` | pouch -> ball | u8 lives (echo of the HIT) | in the pouch's timeslot after HIT |
| RESET | `0x37` | ball -> broadcast | none | Reset state, every `RESET_RETRY_MS` (200 ms) until every pouch has acknowledged |
| RESET_ACK | `0x38` | pouch -> ball | none | in the pouch's timeslot after RESET |

A pouch only accepts frames whose source is the ball: POLL and RESET as broadcasts, HIT addressed to its own timeslot address, REMEMBER with its own ID in the payload (the destination is not checked), FINAL only while it waits for it after answering a POLL (the destination is not checked either). The ball only accepts RESPONSE, REPORT and ACKs from pouches in its table (or, for a JOIN, from `0xFFFE`).

## Ranging round (double-sided two-way ranging)
```
Ball                                     Pouches in the game (timeslot s = 0..7)
POLL (broadcast) ----------------------> poll_rx
                  <--- RESPONSE -------- sent at poll_rx + SLOT_BASE + s*SLOT   (resp_tx)
   (ball records resp_rx per slot)
FINAL (mask) --------------------------> final_rx
                  <--- REPORT ---------- sent at final_rx + SLOT_BASE + s*SLOT
                       (poll_rx, resp_tx, final_rx)
```
`SLOT_BASE` is `SLOT_BASE_UUS` (2000 UUS) and `SLOT` is `SLOT_UUS` (1000 UUS). RESPONSE has no payload: the ball is the source of truth for the lives and reports its own table value with every range.

Because every pouch answers at a time derived from its own timeslot (radio delayed-transmit), pouches never collide. The ball only listens through the highest used slot, so 4 pouches in slots 0-3 mean a shorter round than 8.

The ball listens for RESPONSEs for the response window (`SLOT_BASE` + slots x `SLOT`, plus a few ms of slack), or until every registered pouch has answered, whichever comes first. It then schedules FINAL `FINAL_MARGIN_UUS` (1500 UUS) after that moment and listens for the REPORTs for the response window plus `FINAL_MARGIN_UUS` plus 2 ms. A pouch waits for FINAL for the response window of its POLL plus 10 ms. If the ball heard nobody, it sends no FINAL and the round gives no ranges.

The ball combines its own timestamps (poll_tx, resp_rx, final_tx) with the pouch's and computes the distance (`twr.h`); each radio's clock is separate and never synchronized. Only pouches that answered the POLL are listed in FINAL, and only those send a REPORT. Replies from slots that are not in the ball's table are ignored.

Only pouches in the Joined or Active phase answer a POLL (see the FSM below).

## Joining
1. A pouch without a slot (Joining) sends `JOIN(ID)` from `0xFFFE` after a random 100-300 ms (also the very first one after power-up, so a pouch switched on in the middle of a game hears the ball's POLLs before it transmits anything), then again at random 100-300 ms intervals.
2. The ball listens for JOINs in **Roll Call** and in **Reset** only (see below). `members_join()`: a known ID keeps its slot and lives, a new ID gets the lowest free slot with `LIVES_MAX` (3) lives. With all `MAX_POUCHES` (8) slots taken the JOIN gets no reply (the pouch keeps trying).
3. The ball answers at once with a REMEMBER. If it is lost, the repeated JOIN gets the same answer (same slot), and the next trickle REMEMBER also reaches the pouch.

## REMEMBER schedule (ball)
`uwb_ball_remember_tick()` is called once per main-loop pass in every state, between radio exchanges (it uses the radio). It sends one REMEMBER for one pouch, then waits `REMEMBER_PERIOD_MS / n` (n = pouches in the table), so every pouch gets one per `REMEMBER_PERIOD_MS` (1000 ms): with 4 pouches one every 250 ms, with 8 one every 125 ms. A round-robin cursor (`members_next_remember()`) serves every pouch once per cycle even while others join or are deleted. An empty table sends nothing.

* It is a trickle, not a burst of up to 8 frames: each REMEMBER is one short frame sent between two radio exchanges (the ball waits until it is out), so a ranging round starts at most one frame later.
* It is a broadcast addressed by ID in the payload, because a rebooted pouch has no timeslot address yet.
* **A short miss is harmless.** While a pouch waits for FINAL (up to the response window plus 10 ms, e.g. after a round in which the ball heard nobody and so sent no FINAL) or sends a reply, it ignores other frames, so a REMEMBER sent then is lost. The next one for that pouch comes `REMEMBER_PERIOD_MS` later, and it takes several misses in a row to reach `LOST_TIMEOUT_MS` (5000 ms).

## What a pouch does with REMEMBER
Any REMEMBER with the pouch's own ID **overwrites its slot and lives, in any phase** (the ball is authoritative; one with a slot >= `MAX_POUCHES` or lives > `LIVES_MAX` is ignored). A pouch in Joining or Listening becomes Joined. This repairs three things:

* **A rebooted pouch** gets its old slot and lives back within about `REMEMBER_PERIOD_MS` (Joined, then Active at the first POLL).
* **A lost answer to a JOIN** (slot and lives never arrived).
* **A lost HIT:** the ball already counted it, and an Active pouch whose lives dropped shows the hit effect, as if the HIT had arrived.

## Pouch loss detection
The heartbeat is a REMEMBER with the pouch's **own** ID (not "any frame from the ball": a pouch the ball deleted still hears its POLLs).

* Joined / Active: no such REMEMBER for `LOST_TIMEOUT_MS` (5000 ms) -> Joining. The pouch forgets its slot (it may belong to somebody else by now) and stops answering POLLs. This covers a **ball reboot** (the table is gone, nobody sends REMEMBERs) and a **pouch deleted** by a Reset it missed.
* Listening: the heartbeat is the POLL instead (the ball ranges constantly during a game), so no POLL for `LOST_TIMEOUT_MS` means the ball rebooted -> Joining. A REMEMBER with the pouch's ID moves it to Joined, as always.
* Joining has no timeout.

## Pouch state machine
`pouch_fsm.h` is the logic; `uwb.c` feeds it what the radio hears and sends what it says.

| Phase | Radio | LEDs (`led_patterns.h`, details in [pouch/README.md](../pouch/README.md#leds)) |
|-------|-------|--------------------------|
| Joining | sends JOIN at random intervals, listens | blue, fast blink |
| Listening | silent: the ball is already ranging and will not answer a JOIN | one short blue flash every 2 s |
| Joined | slot and lives known (REMEMBER); the first POLL / HIT / RESET it hears is answered and makes it Active | blue, slow blink |
| Active | answers POLL, HIT and RESET; REMEMBER keeps slot and lives in sync | lives pattern |

| From | Event | To | Also |
|------|-------|----|------|
| power-up | | Joining | first JOIN after a random 100-300 ms |
| Joining | POLL heard | Listening | stays silent; the time of this POLL starts the Listening timeout |
| Listening | POLL heard | Listening | stays silent; the time of the last POLL is refreshed |
| Joining, Listening | REMEMBER with my ID | Joined | slot and lives stored (also: rebooted mid-game, same slot and lives as before) |
| any | REMEMBER with my ID, slot >= `MAX_POUCHES` or lives > `LIVES_MAX` | same | ignored: nothing changes, no event |
| Listening | RESET heard | Joining | first JOIN after a random 0-`REJOIN_DELAY_MAX_MS` (1000 ms): the ball is in Reset, which accepts new pouches |
| Listening | no POLL for `LOST_TIMEOUT_MS` | Joining | |
| Joined, Active | no REMEMBER with my ID for `LOST_TIMEOUT_MS` | Joining | slot forgotten |
| Joined | POLL / HIT / RESET | Active | handled as below |
| Active | POLL | Active | one ranging exchange (RESPONSE, then REPORT if the FINAL mask has my slot) |
| Joined, Active | HIT | Active | lives := the HIT's value (at most `LIVES_MAX`), HIT_ACK in my slot echoing the value as received; hit effect if the value is lower than before |
| Joined, Active | RESET | Active | lives := `LIVES_MAX`, RESET_ACK in my slot |
| Joined, Active | REMEMBER with my ID | same | slot and lives overwritten; hit effect if Active and lives dropped |
| Joining | RESET heard | Joining | ignored: already sending JOINs |
| Joining, Listening | HIT | same | ignored, not acknowledged |

`uwb_pouch_step()` reports what happened (hit, reset, REMEMBER, or nothing) in its `struct pouch_event` out-parameter. The pouch's main loop drives the display and does the logging after the reply has been sent, so a slow console can never make a reply miss its timeslot; `uwb_pouch_step()` itself never logs.

## Hit
`uwb_ball_hit(slot)` takes one life off the ball's table at once (not below 0), whether or not the pouch ever answers. `uwb_ball_send_hit(slot)` then sends the table's value as HIT to the pouch's address; the pouch acknowledges in its timeslot by echoing it, and the ball only accepts an ACK with the same value. The ball's caller retries until it succeeds or `INFORM_TIMEOUT_MS` (1000 ms) passes.

* HIT carries the **absolute** lives, not "minus one", so a retry sets the same number again and is harmless; the pouch only plays its hit effect when the value is lower than what it had.
* If the pouch never hears it, nothing is lost: the table is already right, and the next REMEMBER corrects the pouch within `REMEMBER_PERIOD_MS`.

## Reset: the join window
A Reset is also how a pouch that was switched on late joins. `uwb_ball_reset_start()`, then `uwb_ball_reset_step()` on every main-loop pass for the whole Reset (the ball's main loop decides how long: 3 s, see [ball/README.md](../ball/README.md)), then `uwb_ball_reset_finish()`.

1. **Start:** every pouch's lives in the table := `LIVES_MAX`, nobody has acknowledged yet.
2. **RESET** is broadcast at once and again every `RESET_RETRY_MS` (200 ms) until every pouch in the table has acknowledged. Each pouch acknowledges in its own timeslot; the ACK window is sized from the highest slot at the time of that RESET, because a pouch that joins during the Reset may raise it.
3. **JOIN is accepted for the whole window**, also after every pouch has acknowledged and also with an empty table (no RESET is sent then, the receiver just stays armed). The ball registers the pouch (lowest free slot, or its old slot if it is known) and answers at once with a REMEMBER holding `LIVES_MAX`.
4. **A JOIN counts as an acknowledgement.** It proves the pouch is alive and has the new lives, so a pouch that joins in the last 200 ms, before it could acknowledge a RESET, is not deleted.
5. **Finish:** every pouch that neither acknowledged nor joined is **deleted**: its slot is free for reuse (the next newcomer gets the lowest free slot) and the highest used slot is recomputed, so ranging rounds get shorter. This is safe because everybody's lives were just reset, so nobody loses progress by being forgotten. `uwb_ball_reset_finish()` returns how many **new** pouches joined (a pouch that was already in the table and sends JOIN again is answered and counted as acknowledged, but not as joined), how many were deleted and how many remain.
6. **The ball chooses the next state** from that summary: Roll Call if nobody is left or a new pouch joined (so several late players have time to join), otherwise back to the game.

A JOIN that collides with an ACK costs nothing: the next RESET asks for the ACK again, and the pouch repeats its JOIN after 100-300 ms.

How a pouch that is switched on late joins: it sends JOINs, hears a POLL and goes Listening (the ball is not in Roll Call, so it would not answer). The players hold the ball's button, the ball enters Reset, the pouch hears the RESET, waits a random 0-1000 ms, sends its JOIN and gets its slot in a REMEMBER.

## Log lines
`uwb.c` logs with `LOG_CAT` (`log.h`), each gated by its switch in the "Logging" section of `settings.h`. Only the ball side logs; `uwb_pouch_step()` never does (the pouch's lines are in [pouch/README.md](../pouch/README.md#console-output)).

| Line | Flag (default) |
|------|----------------|
| `MEMBER join uid=0x... -> slot N (new\|known)` | `LOG_MEMBERSHIP` (1) |
| `MEMBER join uid=0x... ignored: all 8 timeslots taken` | `LOG_MEMBERSHIP` (1) |
| `MEMBER deleted uid=0x... slot N: no answer to Reset` | `LOG_MEMBERSHIP` (1) |
| `REMEMBER sent uid=0x... slot=N lives=L` | `LOG_REMEMBER` (0) |

## Limitations
* No authentication: anybody can send JOIN, HIT or RESET. The PAN and all addresses are fixed, so two balls in radio range would interfere with each other's games.
* Two pouches with the same 32-bit ID (a hash collision) are the same pouch to the ball: both use the same slot and garble each other's replies.
* JOIN frames can collide (two pouches joining at the same moment): nothing is lost, each pouch retries after a random 100-300 ms.
* A ninth pouch gets no answer.
* The ball's table is RAM only: a ball reboot starts a new game. The pouches notice (no REMEMBER for `LOST_TIMEOUT_MS`), fall back to Joining and register again in the new Roll Call with full lives.
* Players can only be added in Roll Call or by a Reset, and a Reset gives **everybody** full lives: adding a player resets everybody's lives.
