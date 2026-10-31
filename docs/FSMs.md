# Finite State Machine Diagrams
Transcribed from Felix's iPad

## Ball Main FSM
States:
* Off
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
* Disarmed

During:
* Nothing

Transitions:
* When power switch flipped on, automatically transition to Disarmed

Note: not included in code as a state, but instead as the setup and selftest code run before entering the main state machine.

### Disarmed
From:
* Off
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

Note: initial state of software state machine, after setup and self test have concluded succesfully.

### Ready
From:
* Disarmed
* Classify

To:
* Armed

During:
* Constantly ranging
* Constantly monitoring accelerometer
* Light Strip is Yellow

Transitions:
* If leave proximity of pouch that was detected earlier (on transition from Disarmed to Ready), and/or detect acceleration of a throw, transition to **Armed**

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
* Listen for radio acknowledgement
* Light Strip is Magenta

Transitions:
* After receive acknowledgement of hit notification, transition to **Disarmed**

### Reset
From:
* ANY state

To:
* Disarmed

During:
* Use UWB radio to tell all wearables to reset (set lives to 3)
* Listen for acknowledgement
* Lights Blink Cyan 3 times slowly before moving on

Transitions:
* After receive acknowledgement from at least one pouch, transition to **Disarmed**

Note: **Special Case**: holding the reset button for more than three seconds while in ANY other state transitions instantly to the **Reset** state.

Also, may choose to not wait on acknowledgement or may choose to wait for all connected pouches to reply or may just wait a fixed time amount. Still to be determined.