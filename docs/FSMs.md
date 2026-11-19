# Finite State Machine Diagrams
Transcribed from Felix's iPad

## Ball Main FSM
States:
* Off
* Roll Call
* Disarmed
* Ready
* Armed
* Classify
* Inform
* Reset

Descriptions:
### Off
From:
* Initial State

To:
* Roll Call

During:
* Nothing

Transitions:
* When power switch flipped on, automatically transition to "Roll Call"

Note: not included in code as a state, but instead as the setup and selftest code run before entering the main state machine.

### Roll Call
From:
* Off

To:
* Disarmed

During:
* Listen for roll call from wearables
* Light Strip is White

Transitions:
* When the user button (SW0) is pressed (not held), transition to Disarmed
* Also automatic transition if no new wearable has registered for a set amount of time (15 seconds after the last *new* pouch checked in)
* Neither transition happens until at least one pouch has registered
* Holding the button does nothing in this state (Reset needs a game to reset)

Roll call details: each wearable sends its 32-bit hardware ID; the ball assigns the lowest free timeslot (up to 8) and replies with the ID + timeslot. A repeated ID gets the same timeslot; a ninth pouch is ignored and does not restart the timeout. Wearables turned on after this state ends cannot join. See `shared/README.md`.

### Disarmed
From:
* Roll Call
* Reset
* Classify
* Inform

To:
* Ready

During:
* Constantly ranging to see if near any pouches
* Using the accelerometer only to tell whether the ball is being moved (not for impacts)
* Light Strip is Green

Transitions:
* If a pouch *that has more than 0 lives* is detected within a certain distance for multiple consecutive ranging rounds **and the ball has moved recently**, this indicates a player has picked up the ball, so transition to **Ready**. A ball lying still near a pouch (e.g. dropped) stays Disarmed.
* A single round that fails the condition restarts the count, so occasional noisy measurements are discarded

Note: first state after Roll Call. (The software state machine itself starts in Roll Call, after setup and self test have concluded succesfully.)

### Ready
From:
* Disarmed
* Classify

To:
* Armed
* Classify
* Disarmed

During:
* Constantly ranging
* Constantly monitoring accelerometer for free fall, impacts and motion
* Light Strip is Yellow

Transitions:
* Transition to **Armed** only when BOTH hold:
  * The pouch that was detected earlier (on transition from Disarmed to Ready) is out of range (`LEAVE_DISTANCE_MM`) for `ARM_CONFIRM_ROUNDS` (1) ranging round, or not answering. An in-range round restarts the count.
  * Free fall has been detected very recently. Only free fall after entering Ready counts (the latch is cleared on entry). A timestamp is used, not a live reading, because free fall usually ends before the ranging check completes.
* Out of range without free fall: stay in Ready (keep waiting). Free fall while still in range: no transition.
* An impact (see the free-fall rule under Armed) while still in Ready means the ball hit something before Armed was reached (short throw): transition to **Classify**. (Ignored in the single-pouch bench test, where a drop would count as a hit on the thrower.)
* If the ball has not moved for `READY_STILL_TIMEOUT_MS` (5 s) it was dropped or put down: transition to **Disarmed**.

Note: if the thrower pouch goes silent right after a very close reading, that is treated as a ranging failure from the ball touching the pouch, not as being out of range.

### Armed
From:
* Ready

To:
* Classify
* Ready
* Disarmed

During:
* Ranging only once every `ARMED_RECHECK_MS` (250 ms), to see whether the thrower is back
* Monitoring accelerometer closely
* Light Strip is Red

Transitions:
* When an impact is detected by the accelerometer, transition to **Classify**
* An impact only counts if free fall was seen at most `IMPACT_AFTER_FREEFALL_MS` (150 ms) before it, i.e. the ball was flying. The launch spike of a throw (still in the hand) and bumps while holding the ball are ignored, and an ignored spike does not start the impact cooldown.
* If the thrower pouch is back within `REARM_DISTANCE_MM` for `REARM_CONFIRM_ROUNDS` re-checks in a row, the throw was cancelled (wrong arm, e.g. wind-up): transition to **Ready**
* If no impact happens for `ARMED_TIMEOUT_MS` (6 s): transition to **Disarmed**

### Clasify
From:
* Armed

To:
* Ready
* Disarmed
* Inform

During:
* Instantly perform a single quick ranging poll
* Filter to just the closest pouch
* Determine what to do next based on range to closest pouch and identity of closest pouch
* Light Strip is Blue

Transitions:
* If no pouch is within range, transition to **Disarmed**
* If closest pouch within range is the same one as threw the ball, transition to **Ready**
* If closest pouch within range is not the same one that threw the ball, transition to **Inform**

### Inform
From:
* Classify

To:
* Disarmed

During:
* Tell the pouch that was "hit" (see last transition state of **Classify**) that it was hit using the UWB radio
* Listen for radio acknowledgment
* Light Strip is Magenta

Transitions:
* After receive acknowledgment of hit notification, transition to **Disarmed**

### Reset
From:
* ANY state

To:
* Disarmed

During:
* Use UWB radio to tell all wearables to reset (set lives to 3)
* Listen for acknowledgment
* Lights Blink Cyan 3 times slowly before moving on

Transitions:
* After the blinks (acknowledgments are collected meanwhile), transition to **Disarmed**

Note: **Special Case**: holding the user button (SW0, not the hardware RESET button) for more than three seconds while in ANY state except Roll Call transitions instantly to the **Reset** state.

Implemented: the ball re-sends RESET every 200 ms during the 3 blinks until every registered pouch has acknowledged (each in its own timeslot), then moves on after the blinks regardless, so a silent (switched-off) pouch cannot lock the ball.