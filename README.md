# dodgit
CSE 4990 - Toy Design Group Project

## Overview
### Technical Overview
The project can be divided into two major components: a singular ball and multiple pouches. Both serve different roles but also have many commonalities.

Both the ball and the pouches are powered by the Qorvo DWM3001CDK, an Ultra-Wideband (UWB) ranging development board that also supports other methods of radio communication. This board includes a user-assignable button (+ a non-assignable RESET button), 4 user-assignable single-color LEDs (+ a non-assignable J-Link programming LED), 1 dual-color LED, and a plethora of GPIO pins resembling a Raspberry Pi interface. See [Decadriver Guide](docs/Decadriver_Guide.md) for more details.

The pouches will also carry WS2812 5050 LED strips (8 individually addressed RGB LEDs each) to display feedback to players; until they arrive the pouches use the board's plain LEDs through a small display layer, so only that layer changes. The ball has no strip: its four board LEDs show its state (see [Ball FSM](docs/FSMs.md#leds)).

#### Ball (Throwable)
See [Ball README](ball/README.md) and [Ball FSM](docs/FSMs.md#ball-main-fsm).

The ball contains a Qorvo DWM3001CDK, a single button, and a battery pack with an on/off switch. It's primary functions include:
* Game management/synchronization: it owns the table of pouches, their timeslots and their remaining lives
* Detecting when it is thrown (via onboard accelerometer + UWB ranging)
* Detecting and characterizing hits (via accelerometer + UWB ranging)

#### Pouches (Wearable)
See [Pouch README](pouch/README.md) and [Pouch FSM](docs/FSMs.md#pouch-fsm).

Each pouch contains a Qorvo DWM3001CDK, along with an 8x1 RGB LED strip and a battery pack with an on/off switch. It's primary functions include:
* Displaying a player's remaining lives (the ball is the source of truth; the pouch shows what it is told)
* Responding near-instantaneously to UWB ranging requests
* Updating state based on received messages from the ball
* Rejoining by itself: a pouch can be switched on or rebooted at any time and gets its place back

### Desired Experience
A fast, competitive throwing game with friends that:
* feels satisfying when you hit someone, because the toy confirms it with lights and sound;
* feels exhilarating when you dodge a throw;
* is fair, because the toy decides hits, so nobody argues over rules;
* needs no setup: grab the ball, put on wearables, and play with as many friends as show up.

### Target User
A 10-year-old boy who loves throwing things and playing outside with friends. He’s active, able to run, throw, catch, and dodge, and doesn’t mind getting hit by a ball if it doesn’t hurt. He has small hands, wants the toy to react when something happens, and gets frustrated by cheating or arguments over rules (“I got you!” / “No you didn’t!”).

## Navigation
### `DEV.md`
Starting point for setting up your local development environment.
### `setup/`
Abhi's MacOS setup scripts
### `docs/`
Markdown (`.md`) files containing more complicated documentation, structure, and planning resources than are found in the READMEs:
* [FSMs.md](docs/FSMs.md): the ball and pouch state machines, gameplay assumptions, LED patterns
* [Accelerometer.md](docs/Accelerometer.md): how the ball's accelerometer is classified (Rest / Held / Spinning / Hit), with the data behind the thresholds
* [Plan.md](docs/Plan.md) and [Changes.md](docs/Changes.md): the design of the rejoinable game and the simplified ball FSM, and the notes it came from
* [Decadriver_Guide.md](docs/Decadriver_Guide.md): the DW3000 driver
### `examples/`
Multiple externally loaded or AI-generated nRF Connect projects demonstrating basic tasks such as blinking an LED, reading accelerometer data, and connecting to the onboard DW3110 UWB ranging chip. See [Notes](examples/NOTES.md) for key takeaways and summaries.
### `ball/` and `pouch/`
Main scripts for the ball (throwable) and pouch (wearable).
### `shared/`
Code used by both the ball and pouch: UWB protocol (joining, REMEMBER, timeslots, ranging, hit/reset), the table of pouches, the pouch FSM, LED patterns, logging switches, DS-TWR math, tunable settings. See [shared/README.md](shared/README.md).
### `tools/`
Python scripts for interfacing with devices over bluetooth and/or logging data to `.csv` files in the `data/` folder. `ball_monitor.py` watches the ball's debug log over Bluetooth (see [Ball README](ball/README.md)); the accelerometer trace tools are described in [Accelerometer.md](docs/Accelerometer.md).
### `.vscode/`
Environment setup and Extensions settings
### `modules/`
Contains the sub-repository zephyr-dw3000-decawave for interfacing with the DW3110 chip (part of the DW3000 family) from the Zephyr RTOS. Can also contain other important dependencies in the future.
### `data/`
Generated `.csv` files from running demos, examples, and tests
### `tests/`
Automated tests for tools, helpers, and shared scripts. `tests/run_tests.sh` runs every host test (pure C: DS-TWR math, table of pouches, pouch FSM, LED patterns, the ball's decision logic, the accelerometer classifier; Python: the log monitor and trace tools) without any board or Zephyr. `tests/sim/run.sh` runs the real ball and pouch firmware on the host against a simulated radio (join / rejoin / reset / throw scenarios, no hardware): see [tests/sim/README.md](tests/sim/README.md).

## Gameplay
1. Turn on the ball and the pouches, in any order.
2. Wait for the roll-call timeout (15 s after the last new pouch registered) or press the ball's user button (SW0) once to end roll call. Roll call cannot end before at least one pouch has registered. The ball turns green (nobody is holding it) or red (somebody is).
3. Pick up the ball and throw it at each other. A hit costs the other player one life; their pouch plays a light effect.
4. When only one player has any lives left, hold the ball's user button for three seconds to reset every pouch's lives.

Joining and leaving never needs anything else to be turned off:
* **A pouch that is switched on or rebooted mid-game** rejoins by itself within about a second, with the same timeslot and lives (the ball remembers them and reminds every pouch once a second).
* **Adding a player:** switch their pouch on (it flashes blue every 2 s: waiting), then hold the ball's button for three seconds. This resets everybody's lives (a new round); the ball goes back into roll call so several late players can join; press the button once (or wait 15 s) to resume.
* **Removing a player:** switch their pouch off; the ball forgets it at the next reset.
* **Turning the ball off** starts a new game: pouches notice after 5 s and register again in the new roll call.

Maximum 8 players = 8 pouches + 1 ball = 9 devices total.

### Rules of thumb (deliberate scope limits)
* **No catches.** Pick the ball up from the ground or hand it over; a catch would most likely be scored as a hit on the catcher.
* **No point-blank throws.** A throw has to travel more than about 2 m (twice the pickup range) before it lands, otherwise it is not recognized as a throw and its impact is ignored.
* **No passes in the air.** The ball changes hands by a deliberate hand-over, or when it ends up closer to somebody else's pouch than to the thrower's for about a second.
* A player who is out of lives (red light, steady) is not hit again: a throw that lands closest to them counts as a miss, so nobody around them loses a life by accident.

Details and the reasoning: [FSMs.md](docs/FSMs.md#gameplay-assumptions).

Note: SW0 is the board's user button (it will be replaced by an external button later). It is unrelated to the hardware RESET button, which just reboots the board.
