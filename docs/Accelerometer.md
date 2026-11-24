# Accelerometer classifier
What the ball's accelerometer code decides, how, and on which data every threshold rests. Written so that you can change it without having seen how it was built. The code is pure C ([`ball/src/accel_class.h`](../ball/src/accel_class.h), [`accel_class.c`](../ball/src/accel_class.c)), the thresholds are the `ACCEL_*` macros in the "Accelerometer classifier" block of [`shared/settings.h`](../shared/settings.h) (each with a comment saying what it does and what it was derived from), the tests are in [`tests/accel_class_test.c`](../tests/accel_class_test.c). The plan it implements is Part C of [Plan.md](Plan.md); the definitions come from [Changes.md](Changes.md).

**Read this first: nothing here was recorded with labels.** No new data could be captured while this was written (the boards were not connected), so every threshold was derived from the older recordings in `data/`, which nobody annotated. What each stretch of a recording "shows" below is inferred from the numbers (and, for the BURST windows, from the ball's own state in the log). The thresholds fit these traces; they are not validated on new throws. [Captures still to take](#captures-still-to-take-on-hardware) lists exactly what is needed.

## What it does, in one paragraph
Every accelerometer sample (|a| in milli-g, one every ~6 ms) goes in; out come a **level** (the persistent state: `UNKNOWN`, `REST`, `HELD`, `SPINNING`) and, when it happens, a **Hit** (an edge event: |a| crossed `IMPACT_MG`). The level comes from the *shape* of the last 100 ms of |a| (how rough it is, how far from 1 g, whether it drifts), from how long that shape has lasted, and from which level the ball was in before. The classifier reports every Hit and every level; what they *mean* is the ball state machine's business ([How the ball uses it](#how-the-ball-uses-it)).

## The sensor
* LIS2DH12 on the DWM3001CDK, configured for +-16 g and 400 Hz ([`ball/src/impact.c`](../ball/src/impact.c) `accel_configure`). The ball polls it every `ACCEL_SAMPLE_PERIOD_MS` (5 ms); with the read time the samples arrive **5.8-7.5 ms apart** (mean spacing of the 30 recorded BURST windows). The classifier is tuned for that spacing: its roughness numbers scale with it, so a 400 Hz stream must be decimated to 200 Hz first (the raw recordings of 2026-11-03 were taken at 400 Hz, 2.5 ms apart, and the tools decimate them).
* |a| is computed in milli-g from x/y/z. A ball at rest reads **~1000 mg** (880-990 mg measured, a few percent below 1 g) with noise of **16-48 mg standard deviation**, which differs between recordings (surface, board).
* **Clipping.** The raw x/y/z saturate at +-23 906 mg per axis (5 samples in 2 recordings have all three axes at the limit, |a| = 41 406 mg). The BURST log clips |a| at **32 767 mg** (it is logged as an int16; several recorded impacts show exactly 32 767 for several samples in a row). The classifier clamps its input to `0..ACCEL_MG_MAX` (65 535) only so that its sums cannot overflow.
* **Stalls.** After a hard shock the LIS2DH often stops delivering data. The ball detects this (no fresh sample for `ACCEL_STALL_MS` = 100 ms), re-initializes the sensor and calls `accel_class_reset()`. In a log: `STATUS ... peak=14431 mg ... accel_errors=1`, then `peak=0` lines until the re-init (data/ball_20261112_151758.csv). The recordings also contain a 1.7 s hole in the samples (the BLE link dropped lines: `accel_20261103_135646.csv`, 140 rows flagged `dropped`). The classifier treats a gap of more than `ACCEL_STALL_MS` between two samples as a reset by itself, so replaying such a recording is safe.

## Definitions
**Levels** (persistent; `enum accel_level`):

| Level | Meaning | What \|a\| looks like |
|-------|---------|----------------------|
| `UNKNOWN` | no data yet, or just reset (boot, stall + re-init, gap in the data). Lasts ~145 ms, the time to fill one window. | n/a |
| `REST` | lying still on the ground, **settled** | ~1 g, only sensor noise, for at least `ACCEL_REST_MS` (1.5 s) |
| `HELD` | carried or handled by a player (and the default guess when nothing else is known) | irregular: sample-to-sample changes of 50-900 mg, not constant. Also what a ball held perfectly still looks like *until* it has been calm for 1.5 s, and after that it reads `REST` (see [Limitations](#limitations-and-failure-modes)) |
| `SPINNING` | in flight (or rolling right after an impact): the acceleration is **smooth** (no roughness) | a ball in free fall (\|a\| about 50-160 mg, lobs), a flat plateau from the spin (the recordings show 770, 1260, 2000-3000, 3800, 6400, 7700 mg), or a slow oscillation around 1 g (900-1340 mg at 3.5-4 Hz) |

`SPINNING` cannot be defined as "not 1000 mg": a flying ball reads ~1000 mg too. What separates it from `REST` is that it *moves smoothly* (a trend of 100+ mg over 50 ms where a resting ball has noise of tens of mg), and from `HELD` that it is smooth (a hand adds 50-900 mg of roughness) *for long enough* (>= 45 ms, or 350 ms near 1 g). On top of the shape, the level **gates** transitions: `REST` is entered only after 1.5 s of calm, `SPINNING` only from `HELD` (or `UNKNOWN`), or from `REST` when the ball is clearly falling or was just hit; a calm ball never drifts into `SPINNING`.

**Hit** (an event, *not* a level): one spike per physical impact, the first sample at or above `IMPACT_MG` (2500 mg). It must fall below 3/4 of the threshold (1875 mg) before another crossing counts, and `IMPACT_COOLDOWN_MS` (300 ms) must have passed since the last *reported* Hit; a crossing inside the cooldown is flagged `hit_suppressed` (it is the same impact: ringing, a double bounce) and not reported. This is the old `impact.c` rule, minus the free-fall gate: the classifier reports **every** Hit in every level and the ball state machine decides whether it matters (only in Thrown). For the first `ACCEL_SETTLE_MS` (500 ms) after a reset no Hit is reported (the sensor settles after a re-init). `hit_mg` is the magnitude of the *crossing sample*, which for a slowly rising impact is below the peak (see [Limitations](#limitations-and-failure-modes)).

The sequence of [Changes.md](Changes.md) - `Rest -> Held -> (Hit) -> Spinning -> Hit -> Spinning -> Hit -> Rest` - is a sequence of levels with Hits in between. `test_example_sequence` builds a synthetic trace of it (lying still, carried, launch spike, flat 3.8 g flight, bounce, 2 g flight, bounce, lying still) and checks the levels `HELD, REST, HELD, SPINNING, REST` and the three Hits, with Spinning within 200 ms of the release and Rest 1.2-2.3 s after the last landing. The same shape appears on a real recording: `accel_20261103_140116.csv` (rest, pick-up at ~4.7 s -> `HELD` at 4.99 s, toss at ~6.2 s -> `SPINNING`).

## How it works
Per sample (`accel_class_update`):

1. **Hit** (above): threshold, hysteresis, cooldown, settling time.
2. **Window.** The last `ACCEL_WINDOW_MS` (100 ms, ~15 samples) of |a| are kept in a ring of 64 samples (the classifier owns no memory beyond the `struct accel_class`, 620 bytes, that the caller allocates). Features over it, all in integer mg:
   * **mean**
   * **MASD**, the mean absolute difference of consecutive samples: *roughness*. It ignores slow drift, so a smooth swing of 400 mg over 100 ms scores ~30 while sensor noise scores 15-50 and a hand 100-900.
   * **TREND**, |mean of the newer half - mean of the older half|: slow drift with the noise averaged out. (Peak-to-peak and standard deviation measure the *spread*, which is as large for a smooth drift of 400 mg as for a hand; MASD ignores slow drift and TREND isolates it.)
3. **Shape** of the window, in this order (`enum accel_shape`): `NONE` (fewer than `ACCEL_MIN_SAMPLES` samples or less than half a window of time), **`SMOOTH`** (the *50 ms* window, `ACCEL_FAST_WINDOW_MS`, has its mean outside 600-1500 mg and MASD <= 80: free fall or a plateau; the shorter window makes a release visible ~50 ms earlier), **`ROUGH`** (MASD over 100 ms > 80), `MIXED` (mean far from 1 g on the long window only: the ball just changed course), **`CALM`** (mean near 1 g, TREND <= 60, MASD <= 60), **`DRIFT`** (near 1 g, TREND >= 100, MASD <= 40: a slow smooth oscillation), `MIXED` otherwise.
4. **Runs**: how long each shape has lasted (a run survives a gap shorter than a grace time: `ACCEL_REST_GRACE_MS` 150 ms for CALM, `ACCEL_SPIN_GRACE_MS` 100 ms for SMOOTH-or-DRIFT). Durations are the debounce: a level needs its evidence to last, so nothing flickers. In addition **no level lasts less than `ACCEL_MIN_DWELL_MS`** (50 ms), whatever the evidence.
5. **Level**, gated by the level the ball is in:

| From | To | When |
|------|----|------|
| `UNKNOWN` | `SPINNING` or `HELD` | after one full window plus `ACCEL_SPIN_MS` (~145 ms): `SPINNING` if the ball is flying (below), else `HELD` (never `REST` straight away) |
| `HELD` | `SPINNING` | **flying**: `SMOOTH` for `ACCEL_SPIN_MS` (45 ms), *or* within `ACCEL_AFTER_HIT_MS` (500 ms) of a Hit any `SMOOTH`/`DRIFT` stretch of 45 ms, *or* `SMOOTH`/`DRIFT` for `ACCEL_SPIN_DRIFT_MS` (350 ms: a spin drift near 1 g) |
| `HELD` | `REST` | `CALM` for `ACCEL_REST_MS` (1.5 s) |
| `REST` | `HELD` | not `CALM` for `ACCEL_LEAVE_REST_MS` (250 ms) |
| `REST` | `SPINNING` | flying (the first two conditions above; **never** by 350 ms of drift: a calm ball does not drift into `SPINNING`) |
| `SPINNING` | `REST` | `CALM` for 1.5 s |
| `SPINNING` | `HELD` | `ACCEL_SPIN_HOLD_MS` (500 ms) without a sign of life (a `SMOOTH`, `DRIFT` or `CALM` window, or an impact): flight is smooth, the ringing after an impact is rough, but not for long |
| any | `UNKNOWN` | `accel_class_reset()`, or a gap of more than `ACCEL_STALL_MS` between two samples |

`accel_class_level_ms()` is the time in the current level; `accel_class_features()` the latest window (mean, trend, masd, samples, shape), which the `LOG_ACCEL_FEATURES` lines print.

### How the ball uses it
(Plan.md D2-D4.) `Unheld -> Ready` needs an alive pouch in pickup range **and** level `HELD`. `Ready -> Thrown` needs level `SPINNING` **and** the thrower beyond `THROW_RANGE_MM`. `Thrown` acts on Hit events; in every other state they are ignored (and the event queue is drained). `REST` for `THROWN_REST_MS` in Thrown means the ball landed. Things to know when writing that code:
* `SPINNING` does **not** mean "released". An arm swing that pushes the ball smoothly above 1.5 g also reads `SPINNING` (`accel_20261103_140116.csv`: the level changes at 6.17 s, in the middle of a smooth rise of |a| from 0.4 g to 3.1 g (6.0-6.3 s), while |a| only reaches its minimum of 0.17 g at 6.6 s). That is why the transition to Thrown also needs the thrower out of range.
* It lasts for up to 500 ms after the last sign of life, so a landed ball is still `SPINNING` for a moment, and a bounce chain stays `SPINNING` through the ringing.
* `UNKNOWN` (right after a stall) is not `HELD`: treat it as "no information".
* Latency from release to `SPINNING`: **67 ms and 84 ms** on the two recorded clean releases ([Evidence](#evidence)); a drift flight near 1 g is recognized only after 350 ms of drift.

## Evidence
### The recordings
| Files | What they are | What they show (inferred, no labels) |
|-------|---------------|--------------------------------------|
| `data/accel_20261103_*.csv` (8) | raw x/y/z at 400 Hz from an earlier firmware, 3-37 s each | 134755: lying still 0-5 s (per-second sd 34-43 mg), shaken by hand 5-10 s (12 Hits at 2.5-3.8 g, mean 1.7-2.7 g), set down at ~10.3 s, lying still 11-15.4 s. 135847: handled for 24 s with two violent events (41 g and 40 g), then lying still 24-38.5 s (per-second sd 37-48 mg). 135212, 135409, 135646, 140101, 140116, 140513: picked up, tossed, bounced, in many combinations (impacts up to 41 g); 135212 ends lying still, 140116 has rest 2-4.7 s, pick-up and a toss at ~6 s. 135646 has the 1.7 s hole. |
| `data/ball_20261119_150326.csv`, `..._150548.csv` | ball log of 2026-11-19 with `BURST` windows: 140 samples of \|a\| (about 100 ms before ... 250 ms after, t=0 the crossing) around each threshold crossing, clipped at 32 767 mg. 30 complete windows. The old firmware also says what its free-fall gate decided and the ball's state | flights, bounces, a ball swung in a hand, a ball kicked while lying still |
| `data/ball_2026111[2,7,9]_*.csv` | the same logs' `STATUS` lines every 250 ms (peak, from the newer ones also min, and `still=`) | used only to find lying-still stretches (two: 2026-11-17 15:47 5.9 s, 2026-11-19 15:05 2.8 s) |
| `data/hit_*.csv` | the earlier hit prototype in `examples/` | not used |

`tools/accel_trace.py` reads all of these (`summary`, `extract`, `status`, `features`, `fixtures`). Small copies (136 KB) are committed in `tests/fixtures/accel/` so that the tests do not need `data/`.

### The features on labeled-by-inference segments
`python3 -I tools/accel_trace.py features` (sliding 100 ms windows ending at a sample, p10 / p50 / p90 / p99 of each feature, mg; the same definitions as the C code, in floating point):

```
class              segment                  windows               mean mg               masd mg              trend mg
rest               134755 0-4.5 s               889  878   897   910   922   26    37    54    74    2    12    35    56
rest               134755 11-15.3 s             860  898   909   919   958   23    31    43   119    2    11    28   124
rest               135212 8.5-10.7 s            440  931   942   957  1030   24    31    43    48    3    15    41   140
rest               135847 24-38.5 s            2889  949   968   992  1023   23    36    49    68    3    19    51   134
rest               150548#6 -700..-30 ms         88  920   928   934   947   18    21    23    26    2     9    18    32
drift flight       150548#8 -480..-30 ms         67 1062  1143  1263  1272   20    22    38    44   20   100   165   239
drift flight       150548#3 -575..-10 ms         88  927  1074  1219  1246   15    25    29    30   18   139   253   295
flat 3.8 g         150548#8 +190..+246 ms         9 3719  3773  3809  3809   18    25    43    43   67   111   117   117
flat 6.4 g         150548#3 +140..+190 ms         8 6407  6414  6429  6429   20    25    26    26   11   108   135   135
flat 7.7 g         150548#14 +170..+250 ms       13 7639  7654  7673  7680   22    26    28    28   19    42   107   113
flat 1.26 g        150548#20 -240..-60 ms        31 1262  1264  1267  1268   10    13    14    16    1     5    14    22
free fall          150548#25 -450..-120 ms       49   47    59    91   108   17    23    28    31    2     9    46    66
free fall          150548#27 -450..-120 ms       49   58    73   142   162   18    23    32    40    2    20    45    93
free fall          135409 2.5-3.0 s             100   50    56    75   255   20    26    37   110    2     8    21   413
shaken (held)      134755 5.3-9.9 s             920 1181  2195  3620  4160  118   261   567   854  263  1239  3452  4918
handled (inferred) 135847 14-24 s              1998  862   972  1110  1750   31    49   110   351   10    58   224   701
handled (inferred) 135409 25-37 s              2398  475   960  1443  3451   29    47   181  2328   12    88   457  4769
handled (inferred) 150326#1 -700..-60 ms         85  875   944  1008  1020   23    33    42    45   15    58   148   208
```
Reading it:
* **Roughness (MASD) is the one clean separator.** Everything that is "smooth" - lying still, drift flights, plateaus of 1.3-7.7 g, free fall - has a MASD p90 of 14-54 mg (p99 up to 74 mg; the exceptions are a few windows of 110-119 mg: the first second after the ball was set down in 134755, and one free fall in 135409); a hand shaking the ball has p10 118 and p50 261. `ACCEL_ROUGH_MASD_MG` = 80 sits between them. Handled-but-not-shaken segments overlap both (p50 33-49, p90 42-181): that is what `HELD` is for.
* **Rest versus flight near 1 g is only the trend.** Lying still: trend p90 18-51 mg. A drift flight at ~1 g: p50 100-139, p90 165-253. `ACCEL_CALM_TREND_MG` = 60 and `ACCEL_DRIFT_TREND_MG` = 100 are on either side, with a gap in between where the shape is `MIXED`, i.e. no evidence either way.
* **A hand and a drift flight overlap in trend** (hand p50 58-88, flight 100-139) and in MASD (p50 33-49 against 22-25). No instantaneous feature separates them; only *duration* does (below). This is the main weak spot.
* **Flat plateaus at 1.26 g and 0.77 g look like rest** (`CALM`: mean near 1 g, no trend). They only count as flight inside `ACCEL_AFTER_HIT_MS`, and after 1.5 s they would read `REST`.
* Rest means of 878-992 mg (p10-p90 over the five stretches) and free fall means of 47-162 mg define `ACCEL_G_MIN_MG` = 600; `ACCEL_G_MAX_MG` = 1500 keeps hand motion (0.6-1.5 g) out of the "smooth and far from 1 g" evidence.

### The timing thresholds
Each was chosen from a sweep; to repeat one see [Changing a threshold](#changing-a-threshold).
* **`ACCEL_REST_MS` 1500 / `ACCEL_REST_GRACE_MS` 150 / `ACCEL_LEAVE_REST_MS` 250.** On the four raw lying-still stretches 0-7% of the windows are not `CALM` (noise spikes on the noisier boards: 5.4%, 0%, 0.3%, 7.3%), and the longest non-`CALM` run is 95-100 ms. So a 150 ms grace keeps one spike from restarting the Rest timer, and 250 ms of "not calm" is real movement. Every stretch reaches `REST` ~1.5 s after it starts and keeps it; the shaken segment never does. A hand that holds the ball steady for 1.5 s also reaches `REST`: nothing in |a| tells the two apart. 1.5 s is a compromise between false Rest in a hand and a late Rest on the ground.
* **`ACCEL_SPIN_MS` 45** (with the 50 ms window, i.e. 95 ms of smoothness). The longest `SMOOTH` run inside the hand-shaken segment (134755 5.3-9.9 s, ~23 shakes) is **40 ms**. 30 and 40 ms called that ball `SPINNING` for 2 s; 45 and above do not. The **margin is thin** (one outlier run). Real flights give 100+ ms of `SMOOTH`, and the price of a longer time is latency: each +5 ms adds ~6 ms (the release latency was 74/90 ms at 50, 80/97 at 55, 80/103 at 60).
* **Release latency** (`test_release_latency`). BURST #25 and #27 of `150548` are tosses from a hand that was steady at ~0.85 g, then |a| falls to ~50 mg within ~80 ms (725 -> 350 -> 33 mg), then the landing impact (6 g, 4.8 g) about 0.5 s later. They contain only ~110 ms of steady hold, so the test replays the real pre-release samples 8 times first to bring the classifier to `HELD`. From the first sample below 300 mg to `SPINNING`: **67 ms (#25) and 84 ms (#27)**. The target of "well under 100 ms" is met only on average: expect 70-90 ms for a clean toss, 350+ ms for a drift flight, and *before* the release for an arm swing.
* **`ACCEL_SPIN_DRIFT_MS` 350** (`HELD` -> `SPINNING` without free fall or an impact). The ball of `150326` BURST #1 (the old firmware's state was `Ready`: in a hand, swinging 940 +-100 mg at 3-4 Hz, then bumped) has `DRIFT`/`SMOOTH` windows for 275-300 ms. 250 and 275 ms call it `SPINNING` 170 ms before the bump; 300 does not; 350 keeps 50 ms of margin. One window is not much evidence either.
* **`ACCEL_SPIN_HOLD_MS` 500.** After a Hit the ball rings rough until the first smooth window: **185 ms median, 415 ms p90, 926 ms max** (68 impacts in the raw recordings that no other Hit interrupted), 111 ms median in the BURST windows (which stop 250 ms after the crossing). 94% of them are covered by 500 ms; the rest fall back to `HELD` and are `SPINNING` again when |a| is smooth far from 1 g (or after 350 ms of drift).
* **`ACCEL_AFTER_HIT_MS` 500.** The example sequence says `SPINNING` is reachable "right after a Hit". Plateaus at 0.77 g and 1.26 g (BURST #24, #20) are only recognizable that way.
* **`ACCEL_SETTLE_MS` 500** is the old `SETTLE_MS` of `impact.c`.

## Results on the recordings
`python3 -I tools/accel_replay_data.py` (also run by the Python tests) replays all of `data/` through the same C file as the firmware and prints:

```
== Raw x/y/z recordings (decimated to 200 Hz)
file                       span s  REST%  HELD%  SPIN%  chg  hits  supp  shortest ms
accel_20261103_134755.csv    15.4     43     56      0    4    12    18          145
accel_20261103_135212.csv    10.8      9      9     82    3     9     4          148
accel_20261103_135409.csv    37.4     10      9     81   13    28    20          148
accel_20261103_135646.csv    10.5      0     11     84    6     7     5          145
accel_20261103_135847.csv    38.5     51     20     29   11    15     6          145
accel_20261103_140101.csv     3.2      0     60     35    2     1     0          150
accel_20261103_140116.csv     8.7     19     50     29    4     4     2          145
accel_20261103_140513.csv     5.4      0     32     66    2     7    12          148
```
(`chg` = level changes, `supp` = crossings suppressed by the cooldown, `shortest` = the shortest level that was left again; 145-150 ms is the UNKNOWN warm-up. **No level that was left again lasted less than 100 ms** after the warm-up, on any recording; the minimum dwell is 50 ms.) The BURST section lists, per window, the level just before the crossing, whether the crossing was reported as a Hit (all 30 are, exactly once, at the crossing) and the level 250 ms later. The sections of the tool's output are explained in its header.

**Where the classifier disagrees with the ball's own state or the old gate** (the tool lists these; they are not failures):
* **150548 #12** (old gate: accepted; ball state `Ready`): |a| swings between 320 and 1450 mg at ~3 Hz for 700 ms, then a 4.8 g bump. The old gate accepted it because |a| dipped below 700 mg for 30 ms. The classifier says `HELD` throughout, which matches the state (a ball swung in a hand). This is the false accept the redesign removes: a Hit in `Ready` is ignored.
* **150548 #7** (state `Armed` = thrown): |a| swings smoothly between 0.5 and 1.6 g for ~450 ms, then climbs to 2.5 g at 600 ms and 10 g at 720 ms. The classifier says `HELD` until the climb passes 1.5 g and `SPINNING` from the 2.5 g crossing on. A slow wave like this is neither free fall nor a plateau, so nothing in the windows says "flying". If the ball was in the air all along this is a miss; the log does not say.
* **150548 #25** (state `Ready`): the classifier says `SPINNING` (a clean free fall to ~50 mg) while the ball's state is still `Ready`: a toss with the thrower within range. The classifier is right; the ball's state machine does not call it a throw (Plan.md, Risk 6).
* The eight `150326` windows logged in state `Roll Call` (run 1 #4, run 2 #2-#9) look like flights (smooth 1.5-2.5 g plateaus, a free fall to 0.15 g in run 1 #4) and come out `SPINNING`; presumably the ball was tossed while it waited for pouches. Nothing in the log says so.
* 150326 #1 (state `Ready`) and #12 above are the only windows of a ball that is clearly in a hand; both come out `HELD`.

Of the 13 crossings of `150548` that the old gate rejected for lack of free fall, the classifier finds 11 in a `SPINNING` ball (the gate could not see these flights: |a| never fell below 700 mg) and 2 in `HELD` (#6, the kick of a ball lying still, and #7 above).

## Limitations and failure modes
* **A ball held perfectly still becomes `REST`** after 1.5 s: a hand is as calm as the ground. The ball state machine should not treat `REST` as "on the ground" without other evidence (it uses `REST` only to end Thrown early).
* **False `SPINNING` from a smooth arm movement.** Any smooth rise or plateau beyond 1.5 g (or below 0.6 g) lasting 45 ms gives `SPINNING`: lifting the ball briskly, the push of a toss. The thin margin of `ACCEL_SPIN_MS` (above) is the same failure on a shaken ball. It is harmless as long as Thrown also requires the thrower to be out of range.
* **Drift flights near 1 g are recognized late** (350 ms plus the window) and flat plateaus at 0.77-1.26 g only after a Hit. A short throw of that kind is not seen in time (Plan.md, Risk 6).
* **A Hit is the first sample at or above `IMPACT_MG`, not the peak.** 7 of the 30 recorded crossings are smooth ramps (at most 300 mg per sample, more than 60 ms above 1.5 g before the crossing) that continue to 4-11 g within 100 ms; the real peak falls inside the cooldown and is not reported, and `hit_mg` of such a Hit is 2.5 g. The same happens for a spin plateau above 2.5 g (in `140116` the push of the toss crosses 2.5 g at 6.2 s, a Hit that is not an impact). The ball ignores Hits outside Thrown, but in Thrown a plateau above 2.5 g would classify early. A later refinement could require a sharp edge or report the peak over the next ~20 ms; neither was done because there is no labeled data to decide it.
* **The sensor stall loses the impact that caused it**, and the classifier ignores Hits for 500 ms after the reset. The ball treats the stall as a Hit-like event in Thrown (`impact_get`).
* **A constant input looks like free fall or rest.** A stuck sensor that repeats one value reads `SPINNING` (any value far from 1 g) or `REST` (near 1 g). All-zero samples are screened as a stall in `impact.c`; other stuck values are not detected.
* **The sample spacing is part of the tuning.** MASD scales with it: at 400 Hz it would be about half, at 100 Hz about double. Retune (or decimate) if `ACCEL_SAMPLE_PERIOD_MS` changes.
* **The 32-bit millisecond clock** wraps after 49.7 days; all comparisons are wrap-safe except that, after 49.7 days of uptime without a reset the 500 ms settling time (no Hits reported) is applied once more. Immaterial for a battery toy.
* **Fitted to the data it is tested on.** Every threshold was chosen on these 8 + 30 + 5 segments, and the tests assert what the data show. They guard against regressions; they do not show that a new throw is classified right.

## Running the tests and tools
From the repository root (`tests/run_tests.sh` runs everything below):

```
cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/accel_class_test.c ball/src/accel_class.c -lm -o /tmp/accel_class_test && /tmp/accel_class_test
python3 -m unittest discover -s tests -p 'test_accel_trace.py'          # tools/accel_trace.py and accel_replay_data.py
cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/accel_replay.c ball/src/accel_class.c -o /tmp/accel_replay
/tmp/accel_replay -f 50 < tests/fixtures/accel/burst_20261119_150548_r1_s08.txt     # levels, Hits and features of one trace
python3 -I tools/accel_replay_data.py [-v]                            # the whole data/ directory
python3 -I tools/accel_trace.py summary | features | status | extract OUT_DIR | fixtures OUT_DIR
```
Also worth running after a change (both pass at the time of writing): the test with `-fsanitize=address,undefined -fno-sanitize-recover=all`, and the target build `arm-zephyr-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src -c ball/src/accel_class.c` (1.7 KB of code, no library call but `memset`).

`tests/accel_class_test.c`: **A** the synthetic example sequence; **B** the recorded flights and landings (BURST windows: BURST #8 of 150548 with its ~1000-1340 mg drift before the 33 g crossing and 3.8 g flat for 160 ms after it, #3, #14, #19, #20, #24, #6, 150326 #1, and the Hit at the crossing of every complete window) and the release latency; **C** resting (the raw lying-still stretches and the STATUS-derived ones must reach `REST`, never a Hit or `SPINNING`); **D** held (the shaken ball is `HELD`, never `REST`; the `140116` sequence; the recording with the hole); **E** reset and `UNKNOWN`, clipping at 32 767, timestamp jitter and wrap, a 33 g spike inside the cooldown, a gap, and garbage input.

## Changing a threshold
1. Look at the distributions: `tools/accel_trace.py features` (add a segment to `EVIDENCE` in the script for a new kind of motion), and a trace with `accel_replay -f 20`, which prints mean/trend/masd/shape every 20 ms.
2. Try a value without touching `settings.h`: copy it to a scratch directory, change the `#define`, and put that directory first on the include path: `cc -std=c11 -I/tmp/try -Ishared -Iball/src tests/accel_replay.c ball/src/accel_class.c -o /tmp/accel_try` (the same for `tests/accel_class_test.c`). This is how the sweeps above were made.
3. Run the whole suite and `tools/accel_replay_data.py`. The assertions on the recorded traces encode what the data showed when the thresholds were chosen; if one fails, decide whether the trace or the expectation was wrong and **write the reason into `settings.h` and this file**.
4. Update the comment of the macro with the new evidence, and the numbers in this file.

## Adding traces
1. Capture with `LOG_ACCEL_RAW` (and `LOG_ACCEL_CLASS`, `LOG_ACCEL_BURST` for context) on, save the log with `tools/ball_monitor.py`: it lands in `data/ball_<time>.csv`. See [Logging.md](Logging.md). Write down, per capture, what was done and *when* (the RAW dump header carries the ball's `up=` time of the capture; the button press that triggered it is its last instant).
2. `python3 -I tools/accel_trace.py extract /tmp/traces` writes one text trace per BURST window and per RAW capture (`t_ms mg` per line, plus `x y z` for RAW captures; `#` lines hold the metadata). Replay one with `accel_replay` and compare with what you did.
3. To make it a test: add the file's name to `FIXTURE_BALL_FILES` in `tools/accel_trace.py` (its complete BURST windows and RAW captures are then copied to `tests/fixtures/accel/` by `python3 -I tools/accel_trace.py fixtures tests/fixtures/accel`; a new recording in `data/` changes nothing until it is listed, so the committed-fixtures test keeps passing), or add a row to `RAW_FIXTURES` for an `accel_*.csv`. Then add assertions to `tests/accel_class_test.c` in the style of `test_resting` / `test_held` / `test_recorded_flights`, with a comment saying what the trace shows and how you know. Keep the fixtures small (the Python test limits them to 400 KB).
4. Re-run `tools/accel_replay_data.py` and read its DISAGREEMENTS section.

## Captures still to take on hardware
None of this could be recorded in the session that wrote the classifier. Each capture below is one `LOG_ACCEL_RAW` dump (the last 10 s, x/y/z at 200 Hz) with the button pressed right after the movement, plus `LOG_ACCEL_CLASS` on so that the log shows what the classifier said live. The ring is empty after a dump: wait for it to finish, then do the next movement. Do each at least 3 times, and with **both boards** if there are two (the noise differs: sd 16-43 mg). Note for each what was done and the time order.

| # | Capture | What it settles |
|---|---------|-----------------|
| 1 | Ball lying still on a hard floor, 10 s; again on carpet / grass, on a table | the rest noise and mean per surface and board; whether `REST` is reached within 1.6 s everywhere; whether `ACCEL_CALM_*` need to be higher on soft ground |
| 2 | Lying still on a table or floor while someone walks next to it, or the table is knocked | does floor vibration leave `REST` (it should not for light vibration) |
| 3 | Held steadily in one hand, standing still, 10 s; held in two hands; held against the chest | how long a still hand takes to read `REST` (expected: 1.5 s - the known weakness) and what the noise looks like |
| 4 | Carried: walking, jogging, 10 s each, in one hand and in two | `HELD` must stay `HELD`: count false `SPINNING` and false `REST` |
| 5 | Pick-up from the ground: slow, fast, 5 times each | `REST` -> `HELD` latency (~250 ms by design) and whether a slow pick-up with a smooth lift gives `SPINNING` |
| 6 | Wind-up without releasing: arm swing, 10 times, over and underhand | false `SPINNING` from a smooth push, and how often the 2.5 g threshold is crossed |
| 7 | Throws that the ball should call a throw: underhand 2-3 m, overhand 5+ m, a lob, a hard flat throw, each with a lot of spin and with little, 5 each, landing on the floor | release -> `SPINNING` latency (target < 100 ms), the shapes of real flights (free fall to 0 g? a centripetal plateau? a drift?), where drift flights near 1 g occur, time from release to landing |
| 8 | Bounce chains: a throw that bounces 3+ times and rolls out; a roll along the floor without a throw | `SPINNING` through the ringing between bounces (does it fall back to `HELD` after a long ring?), the plateaus after a bounce, `REST` after rolling out, Hit counts against the real number of bounces |
| 9 | A hit on a person: the ball thrown at a person wearing a pouch (foam/soft impact), and on a hand | the shape of a soft impact: is the 2.5 g crossing a ramp (a smaller peak, `hit_mg` low)? |
| 10 | A catch: thrown, caught with both hands, then held | the impact of a catch (Plan.md: out of scope but it will happen) and what the level does afterwards |
| 11 | Bump while held: knocking the ball against a table or a hand while holding it, 10 times | the Hit must be reported and the level must stay `HELD` |
| 12 | Drops: from the hand at 0.5 m and 1 m onto a hard floor, no throw | the impact followed by the ball at rest: `SPINNING` for a moment, then `HELD`/`REST` |
| 13 | A hard shock that stalls the sensor (a hard throw onto concrete), with `STATUS` on | what \|a\| looks like right before the stall, and what the ball logs; whether the stall comes with a recognizable level |
| 14 | A long carry in a bag or pocket, 60 s | false `SPINNING` and `REST` over a long time, and false Hits from jostling |

For all of them, as soon as there is data: replay with `tools/accel_replay_data.py`, add the traces ([Adding traces](#adding-traces)), and review the thresholds in the order they are listed in `settings.h`. Two questions the raw x/y/z may answer that |a| cannot: whether the *direction* of the acceleration (a spinning ball's vector rotates; a hand's does not) separates a drift flight from a hand-swung ball, which is the weakest point of the classifier, and whether a rolling ball can be told from a flying one.
