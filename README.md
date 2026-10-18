# pebble-render

Pixel-exact Pebble Time 2 (emery) screenshots of watchfaces and apps on your computer,
without the emulator.

`render.py` compiles the real PebbleOS graphics, font, UI and animation code for your
machine, runs your app's own C code against it, and writes the same PNG
`pebble screenshot` would, pixel for pixel. A render takes about half a second, any
number can run in parallel, and you choose the scene: time, time zone, battery, phone
connection, steps, heart rate, button presses and messages from the phone. It can also
record the whole run as a GIF.

![The kitchen-sink example on the emulator and in render.py, with an empty difference](docs/parity.png)

## Why

Iterating on a watchface or app with the emulator means build, install, wait, press your
way to the screen you're working on, screenshot, after every change, one app at a time.
The emulator also hangs now and then when you switch between apps ("X is not
responding").

Measured on an M3 Pro Mac with a production watchface
(`tests/benchmark.py`, median of 5):

| One iteration | Time |
|---|---|
| Emulator: `pebble build`, install, settle, screenshot (warm) | 4.9 s |
| Emulator: the same, starting with the emulator stopped | 6.4 s |
| render.py after a code change (no `pebble build` needed) | 0.52 s |
| render.py when only the scene changes (`--time`, `--battery`...) | 0.07 s |
| render.py, ten different scenes in parallel (total) | 0.13 s |

That's about 9x faster per code change, 70x for scene changes, and nearly 400x for a
batch of ten scenes, which the emulator can only do one after another.

Use the emulator as the final check, and for what it does that this doesn't: real-time
behaviour, logs, the phone side (PebbleKit JS) and other Pebble models.

## Accuracy

Every comparison below is a full-screen pixel diff against `pebble screenshot` from the
SDK's emery emulator, rendered at the same moment. All of them differ by zero pixels:

- `examples/kitchen-sink`: system fonts (Gothic, Bitham, LECO, Roboto), text alignment and
  ellipsis, a custom TTF, a PNG in plain and `GCompOpSet` bitmap layers, anti-aliased
  paths, lines, circles and arcs, nested layers with clipping and scrolled bounds
- the [pebble-watchface skill](https://github.com/coredevices/pebble-watchface-agent-skill)'s
  static and weather templates, and its Lightsaber Duel sample (including a star field
  drawn from `rand()` after `srand(time(NULL))`)
- a published production watchface with custom fonts, PDC icons, health and battery
- `examples/app-tour`, a watchapp with a status bar, menu, action bar, scroll layer,
  property animation, number window, action menu and simple menu, after each of 14
  sequences of button presses, including held buttons
- an activity-tracking watchapp's menu, before and after presses
- 45 renders of earlier watchface designs, through the lab mode described below

Check your own app with `python3 tests/parity.py path/to/project`, adding the same
`--press`, `--wait` and `--message` actions as render.py to check a screen further in. It
needs the emulator, and handles the app-switch hang by installing every project under one
test UUID.

## Requirements

- macOS. It's developed and tested on Apple Silicon. Linux should work, but hasn't been
  tested.
- The [Pebble SDK](https://developer.repebble.com/sdk/) with its emery emulator. The fonts
  come from it.
- `clang`, `git`, and Python 3 with Pillow (`pip install Pillow`).
- About 500 MB of disk for the PebbleOS checkout.

## Setup

```sh
git clone https://github.com/LiamCordelle/pebble-render
cd pebble-render
sh setup.sh
```

`setup.sh` does three things:
1. Fetches PebbleOS at the tag of the firmware inside your SDK's emulator (`v4.33.2` for
   SDK 4.33.1), plus the FreeRTOS headers and TinyMT.
2. Applies the patches in `hostgfx/patches`.
3. Extracts the system fonts and resources from your SDK's emulator image.

Everything stays in this directory. Nothing is installed system-wide.

The first render afterwards also compiles the firmware code, once. On an M3 Pro, setup took
10 seconds and that first render about a second.

If you upgrade the SDK and its emulator firmware changes, `render.py` warns you. Re-run
setup with the new tag, for example `PEBBLEOS_TAG=v4.34.0 sh setup.sh`. Set `PEBBLE_SDK`
if your SDK isn't in the default location.

## Usage

```sh
cd my-watchface
pebble build                                      # once, and after resource changes
python3 ~/pebble-render/render.py . shot.png      # writes shot.png and shot_3x.png
```

Fonts, images and `RESOURCE_ID_*` come from your last `pebble build`, so rebuild after
changing resources. Code changes don't need a rebuild. The project must include `emery`
in `targetPlatforms`.

```sh
render.py . night.png --time 23:41 --12h --battery 9 --no-phone
render.py . weather.png --message TEMPERATURE=14 --message CONDITIONS=Cloudy
render.py . nz.png --tz Pacific/Auckland --date 2026-12-25
render.py . walk.png --steps 8342 --hr 112
render.py . animated.png --run-ms 1500     # 1.5 s of timers and animations first
```

Apps: press buttons and send phone messages in order, as you would on the emulator.

```sh
render.py . third.png --press down --press down --press select   # the third menu item
render.py . reset.png --press select:1000                        # hold select for 1 s
render.py . locked.png --press select --message GPS_STATUS=2 --wait 2000
render.py . slide.png --press select --run-ms 1500 --gif slide.gif   # and a GIF of it all
render.py . resumed.png --persist 1=42 --persist 2=hex:0a0b0c         # start with stored values
```

| Option | Default | Effect |
|---|---|---|
| `--time HH:MM[:SS]` | `10:09` | the time on the watch |
| `--date YYYY-MM-DD` | `2026-10-17` | the date |
| `--tz ZONE` | `UTC` | the watch's time zone (IANA name), for faces that show UTC or other zones |
| `--12h` | 24-hour | `clock_is_24h_style()` returns false |
| `--battery N` | `80` | `battery_state_service_peek()` charge |
| `--charging` | | charging and plugged in |
| `--no-phone` | connected | the phone is disconnected |
| `--steps N` | `0` | `health_service_sum_today(HealthMetricStepCount)` |
| `--hr N` | `0` | `health_service_peek_current_value(HealthMetricHeartRateBPM)` |
| `--no-tick` | | don't deliver the first tick at launch |
| `--press BUTTON[:MS]` | | press `back`, `up`, `select` or `down`: held for 100 ms (or MS), then 500 ms pass, like `pebble emu-button click` |
| `--wait MS` | | let MS of time pass: timers, animations and ticks run |
| `--message KEY=VALUE` | | an AppMessage from the phone; `KEY` is a `messageKeys` name or a number; consecutive ones form one message |
| `--run-ms N` | `0` | let N ms pass after the presses, waits and messages, before the frame |
| `--persist KEY=VALUE` | | start with a value in persistent storage: an integer, a string, or `hex:` bytes |
| `--load-persist FILE` | | start with the storage `--save-persist` wrote |
| `--save-persist FILE` | | after the app exits, write its persistent storage (JSON) |
| `--gif FILE` | | also record everything from launch to the frame as an animated GIF |
| `--fps N` | `20` | the GIF's frame rate, in simulated time |
| `--scale N` | `3` | also write `OUT_<N>x.png` (and `.gif`), a nearest-neighbour upscale (`1` to skip) |
| `--no-correction` | | skip the LCD colour correction `pebble screenshot` applies |
| `--raw FILE` | | also write the raw 8-bit framebuffer |
| `-v` | | print firmware log lines |

`APP_LOG` output always goes to stderr.

## What it runs

**Your app's real code.** `main()` runs, `init()` builds the windows and layers, and the
frame is drawn by the firmware's own `window_render`.

**Then, in order:**
1. app focus, and the first tick, which the watch delivers within a second of launch
2. your `--press`, `--wait` and `--message` actions
3. `--run-ms`

Time is simulated. It moves only during presses, waits and `--run-ms`, and as it does, app
timers fire, animations advance and the clock ticks every second. After each event the
window is redrawn if it asked to be, as on the watch, so the PNG shows what the watch
would be showing.

**Drawing and UI (the firmware's own code):**
- `graphics_*`, `GPath`, PDC draw commands, bitmaps and PNGs, `GBitmapSequence`
- system and custom fonts
- `Layer` trees with clipping, `TextLayer`, `BitmapLayer`, `RotBitmapLayer`,
  `InverterLayer`, `Window`
- `MenuLayer`, `SimpleMenuLayer`, `ScrollLayer`, `ActionBarLayer`, `StatusBarLayer`,
  `NumberWindow`, `ActionMenu`
- click handling (single, repeating, multi and long clicks), `Animation` and
  `PropertyAnimation`

**Services (deterministic stand-ins):**
- time and time zone, clock style, battery, phone connection, health (steps and heart rate)
- persistent storage: in memory, empty at start like a fresh install unless you seed it
- AppMessage: outgoing messages are acknowledged after 100 ms, or fail with `--no-phone`;
  incoming ones come from `--message`
- accelerometer, compass, touch, vibes and backlight calls are accepted and ignored

**Not supported yet.** The render stops with a message naming the call:
- `AppSync`, dictation, wakeup, workers, data logging, app glances

Other limits:
- **Window transitions** aren't drawn: windows change at once. Wait for a transition to
  end before comparing a frame with the watch (presses already do).
- **The phone side:** PebbleKit JS doesn't run. Send what it would with `--message`.
- **Platform:** emery (Pebble Time 2) only. Use the emulator for other models.
- **Compiler:** your C is compiled by clang, so GCC-only extensions, such as nested
  functions, won't build.
- **Randomness:** faces that draw random elements on every redraw depend on how many frames
  have run, so they won't match a particular emulator frame. The random-number generator
  itself is the firmware's.

## How it works

`hostgfx/build.py` compiles part of PebbleOS for your machine with the firmware's own
headers and unit-test overrides:
- graphics, fonts and text layout
- PNG and PDC decoding
- `Layer`, `Window`, `TextLayer`, `BitmapLayer` and the menu, scroll, action bar and status
  bar layers
- click recognizers and the Animation framework
- the dictionary code

Your project's sources are compiled against the SDK's emery `pebble.h` with the defines
`pebble build` uses. `hostgfx/gen/sdk_rename.h` maps each SDK function to its firmware
implementation exactly as the watch's jump table does; `graphics_fill_rect`, for example,
becomes `graphics_fill_round_rect_by_value`. Both halves link into one small program per project.

Two host files stand in for the rest of the OS:
- `hostgfx/fw_host.c`: resources from your SDK's emulator flash image, the system font
  table, app state, and the clock.
- `hostgfx/os_host.c`: the window stack, event loop, buttons, simulated clock, app
  timers and services.

Any SDK call they don't implement becomes a stub that stops the render and names the call.

The host compiler is set up to behave like the ARM build: `unsigned char`, no
floating-point contraction, `-O0`. One firmware routine relied on a 32-bit shift whose
result ARM and other CPUs disagree on; `hostgfx/patches` makes the host match the watch.

Finally, `hostgfx/screenshot.py` applies `pebble-tool`'s colour-correction table, so PNGs
match `pebble screenshot`.

## Also included

- `glance.py out.png`: writes `out_glance.png`, the render at half resolution, roughly what
  the eye resolves when glancing at a 25 mm screen. If a number doesn't read there, it
  won't read on the wrist.
- **Watchface Lab projects** (`src/c/lab_designs.h`) get a lab mode. It draws a design's
  draw function directly, with `--scenario 0|1|2` (demo, stress, empty), `--taps N` to
  pick a design and `--detail` for its tap view.

## Licence

Apache License 2.0, matching PebbleOS. See [LICENSE](LICENSE), and [NOTICE](NOTICE) for
the third-party parts: pebble-tool's colour table (MIT), and the PebbleOS, FreeRTOS and
TinyMT sources that `setup.sh` fetches. The fonts and resources `setup.sh` extracts come
from your own SDK and stay on your machine.
