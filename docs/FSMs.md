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
* Not using accelerometer
* Light Strip is Green

Transitions:
* If a pouch *that has more than 0 lives* is detected with a certain distance, this indicates a player has picked up the ball, so transition to **Ready**

Note: first state after Roll Call. (The software state machine itself starts in Roll Call, after setup and self test have concluded succesfully.)

### Ready
From:
* Disarmed
* Classify

To:
* Armed

During:
* Constantly ranging
* Constantly monitoring accelerometer, only if transitioning based on freefall. Ignore for now.
* Light Strip is Yellow

Transitions:
* If leave proximity of pouch that was detected earlier (on transition from Disarmed to Ready), transition to **Armed**
* Maybe transition also on acceleration of a throw, if classifiable. Ignore for now.

Note: possibly transition back to disarmed if throw acceleration not detected on leave proximity. That might result in false negatives though. Currently false positives are handled in the upcoming Classify step, so aren't an issue.

### Armed
From:
* Ready

To:
* Classify

During:
* Not ranging at all
* Monitoring accelerometer closely
* Light Strip is Red

Transitions:
* When an impact is detected by the accelerometer, transition to **Classify**

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