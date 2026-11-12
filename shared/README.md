# shared
Code compiled into both the ball and the pouch. Nothing here is a standalone app.

The DWM3001CDK board carries a Qorvo **DW3110** UWB transceiver (DW3000 family) next to the nRF52833. The Qorvo driver and its names (`dw3000_*`, `CONFIG_DW3000`) cover the whole DW3000 family and work with the DW3110.

| File | Purpose |
|------|---------|
| `settings.h` | every tunable (distances, thresholds, timeouts, timeslot lengths, lives) |
| `twr.h` | pure-C double-sided two-way-ranging distance math (host test: `tests/twr_test.c`) |
| `uwb.h` / `uwb.c` | DW3110 setup and the whole radio protocol (ball and pouch roles) |

## Addresses
Ball `0x0001`; broadcast `0xFFFF`; pouch without a timeslot `0xFFFE`; pouch in timeslot *n* `0x0100 + n`. Every frame is a short IEEE 802.15.4-style data frame (PAN `0xD048`) with a 1-byte message type.

## Messages
| Type | From -> to | Payload | When |
|------|-----------|---------|------|
| JOIN | pouch -> ball | u32 hardware ID | roll call, repeated at random 100-300 ms intervals |
| ASSIGN | ball -> pouch | u32 ID, u8 timeslot | reply to JOIN (also for a repeated JOIN: same timeslot) |
| POLL | ball -> all | u8 number of slots | start of a ranging round |
| RESPONSE | pouch -> ball | u8 lives | in the pouch's timeslot after POLL |
| FINAL | ball -> all | u8 mask of slots that answered | after the response window |
| REPORT | pouch -> ball | 3 x u32 timestamps | in the pouch's timeslot after FINAL |
| HIT / HIT_ACK | ball -> pouch / pouch -> ball | u16 hit number | Inform state; ACK in the pouch's timeslot |
| RESET / RESET_ACK | ball -> all / pouch -> ball | - | Reset state; ACK in each pouch's timeslot |

## Ranging round (double-sided two-way ranging)
```
Ball                                     Pouches (timeslot s = 0..7)
POLL (broadcast) ----------------------> poll_rx
                  <--- RESPONSE(lives) -- sent at poll_rx + SLOT_BASE + s*SLOT   (resp_tx)
   (ball records resp_rx per slot)
FINAL (mask) --------------------------> final_rx
                  <--- REPORT ---------- sent at final_rx + SLOT_BASE + s*SLOT
                       (poll_rx, resp_tx, final_rx)
```
Because every pouch answers at a time derived from its own timeslot (radio delayed-transmit), pouches never collide. The ball only listens through the highest assigned slot, so 4 pouches means a shorter round than 8. The ball combines its own timestamps (poll_tx, resp_rx, final_tx) with the pouch's and computes the distance (`twr.h`); each radio's clock is separate and never synchronized. Only pouches that answered the POLL are listed in FINAL, and only those send a REPORT.

## Hit and reset
* **HIT**: unicast to the pouch's address; it acknowledges in its timeslot and repeats are recognized by the hit number.
* **RESET**: broadcast; every pouch acknowledges in its own timeslot, so the ball can tell exactly which pouches heard it.

## Not covered
Pouches that join late, reboot mid-game, or collide while joining (see [Pouch README](../pouch/README.md)). There is no authentication: this is a prototype protocol with fixed PAN and addresses.
