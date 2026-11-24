Changes:

Timeslot assignment/rejoining/leaving/etc.
Ball:
Storage: ball stores remaining lives of all pouches
Roll Call:
* Wait for button or short timeout, same as always

Always:
* Every second, send out wave of REMEMBER packets in case a pouch has rebooted - contain ID, timeslot, and current lives

Reset:
* If don't receive ACK from a tag for 3 seconds, delete
* If receive join request from tag, add to game, maybe move to Roll Call state before returning to game

Pouches:
Roll Call:
* If fail, listen for two other messages:
  * REMEMBER that matches ID - means turned off and back on again - apply timeslot and lives
  * RESET - wait random delay and try to join game until hear ranging requests again
Reset:
* If already joined, send ACK after each message and then wait
* If not, send join request like roll call

Reasoning:
* If a pouch is turned off and back on mid-game, it should be able to rejoin with the same lives and timeslot as before
* If a pouch wants to join, we shouldn't have to turn everything else off first - just use existing Reset state
* Order of turning ball and pouches on shouldn't matter

New Ball State Structure:
Dead
Ready
Thrown
Hit

Accelerometer States/Definitions for Semantics:
Rest - ~1000mg, sitting on ground
Spinning (or Freefall) - approximately constant acceleration, due to spinning while flying through the air - only comes after Hit or Held
Held - != 1000mg, sporadic values, ball being held by a player and moved around
Hit - Sharp acceleration spike/edge caused by hitting the ground, or a player

Example Accelerometer states in a pick up, throw, hit, and bounce off and land on ground:
Rest -> Held -> (Maybe Hit) -> Spinning -> Hit -> Spinning -> Hit -> Rest

New functions: 
"Disarm" (not to be confused with old "Disarmed" state):
* Range once: if there is a pouch within pickup range, move to ready. If not, move to dead
"Classify" (not to be confused with old "Classify" state):
* Range once:
  * If no pouches within hit range, move to dead
  * If closest pouch within hit range is thrower, move to ready
  * If closest pouch within hit range is not thrower, move to hit

On leave Roll Call:
* Call "Disarm" function to find next state

Dead:
* If a player comes within pickup range and the accelerometer reads a Held state, move to ready

Ready:
* If there is no longer a pouch within throw range (2x pickup range), and/or accel != Held for a period of time, move to Dead
* If another pouch is now closer, stay in ready but change the thrower to the new pouch
* If the closest pouch is really close, then stops replying, stay in ready (because ranging stops working if too close) (still transition to thrown on a throw or dead if accel != Held)
* If there is no longer a pouch within throw range and accel now reads Spinning/Freefall, transition to thrown

Thrown:
* If no impact is detected after 3.5s (throw timeout), call "Disarm" function to find next state
* If the same pouch remains in throw range for 1 second, without an impact, move to ready with that player as the thrower
* If the accelerometer stops responding, call "Classify" to find next state
* If the accelermoeter registers a Hit, call "Classify" to find next state

Hit:
* While in hit, instead of ranging send messages to hit pouch telling it it has been hit and the number of lives it has remaining, until the pouch acknowledges that it has been hit
* If the pouch acknowledges the hit and new lives number, call "Disarm" function to find next state
* If the pouch doesn't acknowledge the hit within hit ack timeout, call "Disarm" function to find next state
* Still update internal lives storage based on hit, even if pouch doesn't acknowledge