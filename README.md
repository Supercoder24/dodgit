# dodgit
CSE 4990 - Toy Design Group Project

## Overview
### Technical Overview
The project can be divided into two major components: a singular ball and multiple pouches. Both serve different roles but also have many commonalities.

Both the ball and the pouches are powered by the Qorvo DWM3001CDK, an Ultra-Wideband (UWB) ranging development board that also supports other methods of radio communication. This board includes a user-assignable button (+ a non-assignable RESET button), 4 user-assignable single-color LEDs (+ a non-assignable J-Link programming LED), 1 dual-color LED, and a plethora of GPIO pins resembling a Raspberry Pi interface. See [Decadriver Guide](docs/Decadriver_Guide.md) for more details.

Additionally, both use WS2812 5050 LED strips, containing 8 individually addressed RGB LEDs each, to display feedback to players.

#### Ball (Throwable)
See [Ball README](ball/README.md) and [Ball FSM](docs/FSMs.md).

The ball contains a Qorvo DWM3001CDK, along with an 8x1 RGB LED strip, a single button, and a battery pack with an on/off switch. It's primary functions include:
* Game management/synchronization
* Detecting when it is thrown (via onboard accelermeter)
* Detecting and characterizing hits (via accelerometer + UWB ranging)

#### Pouches (Wearable)
See [Pouch README](pouch/README.md).

Each pouch contains a Qorvo DWM3001CDK, along with an 8x1 RGB LED strip and a battery pack with an on/off switch. It's primary functions include:
* Displaying a player's remaining lives
* Responding near-instantaneously to UWB ranging requests
* Updating state based on received messages from the ball

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
Markdown (`.md`) files containing more complicated documentation, structure, and planning resources than are found in the READMEs.
### `examples/`
Multiple externally loaded or AI-generated nRF Connect projects demonstrating basic tasks such as blinking an LED, reading accelerometer data, and connecting to the onboard DW3000 UWB Ranging chip. See [Notes](examples/NOTES.md) for key takeaways and summaries.
### `ball/` and `pouch/`
Main scripts for the ball (throwable) and pouch (wearable).
### `shared/`
Scripts and helpers used by both the ball and pouch.
### `tools/`
Python scripts for interfacing with devices over bluetooth and/or logging data to `.csv` files in the `data/` folder.
### `.vscode/`
Environment setup and Extensions settings
### `modules/`
Contains the sub-repository zephyr-dw3000-decawave for interfacing with the DW3000 chip from the Zephyr RTOS. Can also contain other important dependencies in the future.
### `data/`
Generated `.csv` files from running demos, examples, and tests
### `tests/`
Automated tests for tools, helpers, and shared scripts

## Gameplay
1. Turn on the ball
2. Turn on all pouches (make sure within timeout)
3. Either wait for timeout or press reset button once on ball to activate ready state
4. Begin throwing at each other
5. When only one player has any lives left, hold the reset button on the ball for three seconds to restart

If another player wants to join the game, turn all the wearables off, turn the ball off and back on again, then turn all the wearables on.

Maximum 8 players = 8 pouches + 1 ball = 9 devices total.

TODO: Find more elegant solution for adding wearables/coordinating roll call.