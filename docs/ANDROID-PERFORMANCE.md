# Android performance: from 24 to 60 fps at 720 lines

This is the record of how the Android build of the PC port reached a steady 60 fps at 720 lines on a weak tablet: what
was measured, what turned out to be the cause each time, what was changed (in DXVK, in the runtime, in the game's own
code and in the Android layer), and how to measure it again. It is written so that someone can repeat every step,
undo any of them, or carry the same method to another game on DXVK.

Test device for everything below, unless said otherwise: **Samsung Galaxy Tab S6 Lite (Snapdragon 720G, Adreno 618,
2000x1200 screen, 5:3)**, Qualcomm's Vulkan 1.1 driver, the launcher at **720 lines** (the game renders 1200x720 on this
screen). The scene is the title screen ("the menu") for the GPU work, and the first level for play.

Contents

1. [Where it started](#1-where-it-started)
2. [How it was measured](#2-how-it-was-measured)
3. [Step 1: centroid interpolation (DXVK)](#3-step-1-centroid-interpolation-dxvk)
4. [Step 2: FSR's upscaling pass (DXVK present)](#4-step-2-fsrs-upscaling-pass-dxvk-present)
5. [Step 3: the display enlarges, the controls get their own layer](#5-step-3-the-display-enlarges-the-controls-get-their-own-layer)
6. [Step 4: the game's own letterbox (UbiArt code)](#6-step-4-the-games-own-letterbox-ubiart-code)
7. [Step 5: CPU in play, a framebuffer per render-target change (DXVK)](#7-step-5-cpu-in-play-a-framebuffer-per-render-target-change-dxvk)
8. [Step 6: the stick that cost frames (touch layer)](#8-step-6-the-stick-that-cost-frames-touch-layer)
9. [Smaller DXVK changes](#9-smaller-dxvk-changes)
10. [What was ruled out](#10-what-was-ruled-out)
11. [Every switch](#11-every-switch)
12. [Files](#12-files)
13. [Lessons for DXVK on mobile GPUs](#13-lessons-for-dxvk-on-mobile-gpus)
14. [Open items](#14-open-items)

---

## 1. Where it started

| | Title screen fps |
|---|---|
| PC port, DXVK, 1280x720 | **24** |
| The same game in the Xbox 360 port (its own Vulkan renderer), same tablet, same resolution, the same ~130 draws | 58–60 |

The menu draws almost nothing besides 2D sprites in layers (background, trees, logo, fog), yet ran at 24. Taking
elements away showed where the time went: without the background 36 fps, without the logo and trees 39, without all of
them 60. **The sprites were the cost**, and the same sprites in a renderer written for the engine ran at 60, so the cost
was in how DXVK drew them, not in how many there were.

End result of the work below, at 720 lines: **menu 60 fps, world map 59–60, first level 56–60 while playing with the
touch stick**, screen filled edge to edge, FSR available as an option.

| Step | Menu at 720 lines | Kind |
|---|---|---|
| Start (1280x720) | 24 | |
| No centroid on ps < 3 inputs | 35.5 | DXVK shader compiler |
| FSR off by default (bilinear, then display scaling) | 48.5–48.9 | DXVK present |
| Display enlarges + controls on their own layer | 57.5 | DXVK window + Android layer |
| Game's letterbox removed | **60** | game code (lifter) |
| In play: framebuffer cache, touch layer off the game thread | 43–57 → 56–60 in the level | DXVK + runtime |

---

## 2. How it was measured

Guessing was ruled out from the start: every change was A/B'd on the device with the same scene.

**`tools/android/perf_ab.sh`**: an automated A/B run on the phone.

- It wakes the screen, keeps it on while on USB (`svc power stayon usb`), and pushes `port_settings.txt` plus an
  `env.txt` (environment variables the runtime sets at start).
- It launches the game, taps through the launcher, waits for the title screen and measures for a fixed time.
- It saves the logs (`<out>_<label>_start.log`, `_measure.log`) and a screenshot per variant, then restores the
  settings and removes `env.txt`, also on Ctrl+C.
- Variants are written `"name|lines=720 env:KEY=VALUE"`.
- The default resolution is set with `LINES=`. With several devices attached, set `ANDROID_SERIAL`.

**GPU counters (`USF_GPU_COUNTERS`)**: the Adreno's performance counters read in-process (the runtime's KGSL context
copies them to memory with `CP_REG_TO_MEM`). From outside the app they read frozen, so they are read by the game itself
or by the micro-benchmark. Used: RB pixels, ALU cycles, busy %, GPU clock.

**`tools/android/vkbench/`**: a Vulkan micro-benchmark run from `adb shell`. It draws N blended full-screen layers at
1280x720 with a chosen fragment shader (centroid colour, plain colour, no colour). `vkbench2.c` adds the steps toward
the game, each one switchable:

- a real DXT texture;
- a grid of small triangles instead of a quad;
- ~130 draws;
- an attached depth buffer;
- the scene split in passes;
- a render target read back;
- the KGSL counters.

The method: start from the case known to be fast and add one difference with the game at a time; the step where the
time jumps is the cause.

**DXVK pass log and timer** (`DXVK_PASSLOG=1`, `DXVK_PASSTIME=1`, `DXVK_PASSSEQ=1`): every render pass of a frame with
its size, load/store ops and draws, its GPU time from Vulkan timestamps, and the order of passes with the D3D9 calls
around them. `DXVK_FRAMELOG=<frames>` logs each `Clear` and `StretchRect` rectangle and each viewport.

**Who in the game calls D3D9** (`USF_DXB_CALLER=1`): the bridge logs the guest return address and the code addresses
further up the guest stack for each new `SetViewport` / `StretchRect` caller. This is what led into the game's code in
step 4.

**CPU: `simpleperf`** (the app is `profileable`), recorded during play with call graphs, grouped by thread and by
function including callees. Kernel symbols are hidden, but the user-space frames show who enters the kernel.

**Input replay**: `USF_REC` / `USF_REPLAY` record and play back input. On the device the helpers press the touch
buttons with a 0.25 s hold (a plain `input tap` is too short for the game to see) and hold the stick with
`input motionevent`.

---

## 3. Step 1: centroid interpolation (DXVK)

**Cause.** For every `ps_2_0` (any pixel shader below 3.0), DXVK's D3D9 shader compiler decorates the colour inputs as
`Centroid` (`src/dxso/dxso_compiler.cpp`, the input declaration). Centroid only matters with MSAA, and the game has
none. But on Qualcomm's driver a centroid input makes **every pixel 2.7 times slower**, MSAA or not. All of Rayman's
sprites are `ps_2_0` (texture × vertex colour).

**Proof**, vkbench, 14 blended 1280x720 layers, same shader except the decoration:

| Fragment shader | ms per frame |
|---|---|
| colour input `Centroid` (what DXVK emits) | **9.41** |
| colour input without centroid | **3.52** |
| no colour input | 3.33 |

**Change.** `tools/android/dxvk_mobile_nocentroid.patch`: the decoration is applied only when `DXVK_CENTROID=1`.
The picture is identical, because there is no MSAA to resolve.

**Result.** Menu at 1280x720: **24.2 → 35.5 fps** (+47%). The RB went from 299 M to 438 M pixels/s: the same pixels,
drawn faster.

---

## 4. Step 2: FSR's upscaling pass (DXVK present)

**Cause.** The launcher's quality modes render below the screen's resolution, and DXVK enlarged the picture to the
screen in the present with **FSR 1.0 EASU**. EASU makes 12 texture reads and much arithmetic for **each of the
screen's 2.4 M pixels**. On the Adreno 618 it was about **85% of all the GPU's ALU work**, more than the game itself.

| Menu, 1280x720 | fps | ALU cycles per frame |
|---|---|---|
| FSR on (the old default) | 36.0 | 17.7 M |
| **FSR off (bilinear)** | **48.9** | **2.7 M** |
| FSR off, touch controls off | 50.0 | 2.6 M |

At 540 lines: 47 fps with EASU, 60 without.

**Change.** FSR became a launcher option, **off by default**:

- `GameMenu.java`: an **FSR 1.0** check box on the Display Options row, next to Frameskip, saved as `fsr` in
  `port_settings.txt`;
- `dxvk_overlay.c`: `DXVK_FSR` set from `USF_FSR` or `fsr`, default 0.

Turning it off does **not** lower the game's resolution: only the filter used to enlarge it changes. With step 3 the
display hardware does the enlarging.

---

## 5. Step 3: the display enlarges, the controls get their own layer

**Cause.** Even with bilinear, the present was a full-screen pass at 2000x1200 per frame: DXVK blitted the 1200x720
picture to a 2000x1200 swap chain, and the touch controls were blended over it in that same pass. On a tiling GPU that
is a second full-screen pass every frame.

**Change, part 1: the swap chain at the game's size.** `DXVK_HWC_SIZE=<w>x<h>` (in
`tools/android/dxvk_mobile_adreno_perf.patch`, `wsi_presenter_sdl2.cpp`) calls
`ANativeWindow_setBuffersGeometry(window, w, h, 0)` on the window. The swap chain's buffers become 1200x720, and the
display hardware (HWC) scales them to the panel for free. The runtime (`dxvk_overlay.c`, `dxb_overlay_init`) sets it to
the game's resolution when all of these hold:

- FSR is off (with FSR the GPU must enlarge);
- the controls are on their own layer (otherwise they would be enlarged and blurred with the game);
- `USF_NO_HWC` is not set.

**Change, part 2: the touch controls on a separate Android surface.**

- `GameActivity.java` adds a transparent `SurfaceView` over the game (`setZOrderMediaOverlay(true)`, `TRANSLUCENT`,
  neither clickable nor focusable, so touches still reach SDL) and hands its surface to native code
  (`nativeTouchSurface`).
- `dxvk_overlay.c` runs a thread of its own that draws the controls with `touch_picture` into that surface
  (`ANativeWindow_lock` / `unlockAndPost`), **only when they change**. The controls stay at the screen's full
  resolution and sharp; the game's picture is enlarged under them by the compositor.
- When the layer is on, `dxb_overlay_draw` only wakes that thread. DXVK draws no overlay and the GPU does no work for
  the controls.

**Result.** Measured on the presenter alone: 49.7 → 56 fps. Menu at 720 lines, both parts together: **48.5 → 57.5
fps**. The log confirms it with `swap chain at 1200x720` and `touch layer on its own surface`.

---

## 6. Step 4: the game's own letterbox (UbiArt code)

**Cause.** The game kept its picture at **16:9 inside the 5:3 screen**. At 1200x720 that is a 1200x675 band with bars
of 22 lines above and below. The frame log showed what it cost on a tiling GPU:

- the scene's `Clear` covered only the 1200x675 band. A partial clear cannot be a render-pass `loadOp CLEAR`, so the
  pass had to **LOAD** the whole previous picture into tile memory: an extra pass of **~1.1 ms a frame** that drew
  nothing;
- the picture was then copied centred with **two `StretchRect`s**;
- the bars were cleared in yet another pass.

**Finding the code.** The obvious suspect was wrong. The function at `0x83EFF0` compares the rectangle's aspect with
two `double`s in `.rdata` (9/16 at `0xA2C230`, 16/9 at `0xA2C238`). Changing them did nothing; the viewport stayed
1200x675.

With `USF_DXB_CALLER=1`:

- `SetViewport` came from `0x4B946A`;
- the two `StretchRect`s from `0x4C5777` and `0x4C579D`;
- a second path calls the same `SetViewport` from `0x7D998A` (reached from `0x4C42F0`) with 1200x675.

Reading the x86 there showed the real rule: **integer code** that computes a height from a width as
`lea r, [x + x*8]` then `shr r, 4`, that is `x * 9 / 16`. A scan of the executable for that pair found it at
**95 places**, next to the back buffer's width (`[+0x10A0]`).

**Change.** In the lifter (`tools/lift/lift.py`): `FIT_SITES`, the 95 addresses of the `shr`, and `GAME_PATCHES`, one
rule for all of them. When the lifter emits one of those `shr r, 4` instructions it emits instead:

```c
r = usf_fill_screen ? (uint32_t)((uint64_t)(r / 9u) * usf_fit_h / usf_fit_w) : r >> 4;
```

`r` holds `x*9` there, so `r / 9` is `x` again. The height becomes `x * screen_h / screen_w`, the screen's own shape. The
flags are set as the `shr` would set them, so the code after it behaves the same. Nothing in the game's files changes.

`apply_aspect` in `d3d9_device.c` decides when the patch applies:

- `usf_fill_screen = 1` when the screen is **narrower than 16:9** (tablets: 5:3, 16:10, 4:3) and `USF_LETTERBOX` is
  unset; `usf_fit_w/h` are the screen's size;
- the aspect floor went from 16:9 to 4:3, so the camera pair (`GAME_ASPECT_PAIR`) follows the screen too. UbiArt fits
  its camera by height, so a 5:3 screen sees a little less at the sides and nothing is stretched;
- wider screens (phones over 16:9) are untouched by this patch and keep the existing Hor+ path.

**Result.** The viewport is 1200x720, the `Clear` is a real `loadOp CLEAR`, and the frame is **one scene pass**. The
empty 1.1 ms pass, the two copies and the bars pass are gone. Menu **57.5 → 60 fps**, and the picture fills the
screen, which was also wanted for its own sake.

---

## 7. Step 5: CPU in play, a framebuffer per render-target change (DXVK)

The menu was at 60, but the first level dropped to 20–28 fps in its heavier parts. The GPU timer showed the scene pass
at 7.6–11.4 ms and the present at 1.16 ms, which fits in a 16.6 ms frame. So the drops were on the CPU.

**Cause** (`simpleperf` during play): the game thread spent **16–20 ms per frame**, **30% of it in the kernel**.
`createFramebuffer` alone was **~13%** of the game's CPU time:

- DXVK 1.9 creates a new `VkFramebuffer` at every render-target change, and UbiArt changes render targets many times a
  frame (scene, refraction, resolve copies);
- Qualcomm's driver allocates GPU memory in the kernel for each framebuffer:
  `vkCreateFramebuffer → gsl_memory_alloc_pure → IOCTL_KGSL_SHAREDMEM_ALLOC`.

**Change.** A framebuffer cache in `DxvkDevice::createFramebuffer` (`dxvk_device.cpp`, in
`dxvk_mobile_adreno_perf.patch`):

- 32 entries, matched with DXVK's own `DxvkFramebuffer::hasTargets` (the same attachments and layouts mean the same
  framebuffer);
- the least recently used one is replaced when the cache is full;
- entries not asked for in the last 512 requests are dropped, so the render targets they hold are freed, for example
  after a level change;
- the cache is cleared when the device changes, and a framebuffer still in use stays alive through its command lists
  (reference counted);
- `DXVK_FB_CACHE=0` turns it off. Hits and misses are logged every 65,536 hits.

**Result**, the same kind of level section:

| | Before | After |
|---|---|---|
| Game thread CPU per frame | 16–20 ms | **10–12 ms** |
| Time in the kernel | 30% | **16%** |
| `createFramebuffer` | ~13% | below 2% |
| fps in the level's worst parts | 20–28 | **43–57** |

Honest caveat: between the two profiles the GPU driver also changed (the second run used the Turnip 27 driver from
`gpu_drivers/`; the first had fallen back to Qualcomm's), and it may not have been exactly the same stretch of the
level. Part of the gain may be Turnip's. Running the same stretch with `DXVK_FB_CACHE=0` on the same driver would
separate the two. In the menu, which is GPU-bound, the cache changes little (57.0 → 57.5).

---

## 8. Step 6: the stick that cost frames (touch layer)

With everything above, play still lost frames **while the on-screen stick was moved**.

**Cause.** The game reads touch events on its own thread: the HLE message pump in `hle_user32.c` calls `touch_event`
for every finger move, and `touch_event` takes the touch lock. The layer thread, meanwhile, redrew the controls on
**every frame while a finger held the stick**, and did it **holding that same lock**. Each redraw:

- cleared and rebuilt the whole 2000x1200 picture (9.6 MB);
- rasterised every button again, pixel by pixel, with bilinear filtering in floating point;
- copied the whole picture to the surface.

That is about 10 ms a frame. Every finger move made the game thread wait for it.

**Change** (`touch.c`, `touch_picture`):

1. **Copy, then draw without the lock.** The touch state is copied under the lock and drawn after it is released. The
   game thread never waits for drawing.
2. **Pre-scaled sprites.** Each drawing is scaled once to its on-screen size, with opacity and brightness applied
   (premultiplied), into a 16-entry cache. Drawing it is then an integer copy.
3. **Only what changed.** The frame's drawings are kept as a list and compared with the previous frame's. Only the
   union of the rectangles that changed (old and new place of the moved knob) is cleared and drawn again. A full
   redraw happens only when the list's length changes (a finger down or up), or with the menu, the layout editor or
   the fps text.

The surface also has to receive only that rectangle (`dxvk_overlay.c`, `layer_loop`). An Android window cycles
through several buffers, and each holds an older picture. Two attempts failed before the right one:

- **Passing the dirty rectangle to `ANativeWindow_lock`** (Android's own copy-back) left **ghost sticks** on screen:
  old pictures stayed in the other buffers.
- **Identifying buffers by their address** also failed, because the address changes from one lock to the next.
- **The working approach is the buffer age** (`NATIVE_WINDOW_BUFFER_AGE`, the same query EGL's buffer age uses,
  through `ANativeWindow_query`, looked up with `dlsym` because the NDK headers leave it out). The age says how many
  posts ago this buffer was shown. The thread keeps the last 8 posts' rectangles and copies the current change plus
  everything this buffer missed. If the age is unknown (0, a new buffer, or no query on the device), it copies
  everything.

`USF_LAYER_COPYALL=1` forces the full copy every time, and it is the reference when checking for ghosts.
`USF_LAYER_LOG=1` logs the first 200 posts (age and rectangles).

**Result.** The world map holds 59.6 fps with the stick in use, and playing the level by hand on the tablet holds 60 fps.

---

## 9. Smaller DXVK changes

All in `tools/android/dxvk_mobile_adreno_perf.patch`.

- **Fragment-only barriers** (`DXVK_FRAG_BARRIERS`, on by default from `dxvk_bridge.c`). The barriers after a render
  pass and those of D3D9 textures wait for the fragment stage only, instead of every stage from vertex input. On a
  tiling GPU a barrier that reaches the vertex stage holds the next pass's binning until the previous pass ends.
  - It is safe while no vertex shader samples an image: none of the game's 18 vertex shaders on the title screen does.
  - `D3D9DeviceEx::SetVertexShader` calls `dxvkVertexStageReadsImages()` for any shader that does, which turns the
    optimisation off for good and logs it.
  - `DXVK_FRAG_BARRIERS=0` brings the full barriers back.
- **No blending into formats that cannot blend** (`dxvkFormatBlends`, `dxvk_graphics.cpp`). A pipeline that blends
  into a format without `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT` is invalid, and some mobile drivers draw garbage
  or hang on it. Blending is turned off for such attachments. This is a correctness fix, not a speed one.
- **Pipeline state cache in the app's data folder** (`dxvk_bridge.c`). `DXVK_STATE_CACHE_PATH` points to the data dir.
  At the first start an APK asset `rayman.dxvk-cache` is copied there if the APK has one. **None ships yet**: one made
  by playing the final build would remove the hitches when each effect first appears.
- **Analysis-only switches**, all off by default:
  - `DXVK_FRAMELOG`, `DXVK_PASSTIME`, `DXVK_PASSSEQ` (described in section 2);
  - `DXVK_SKIP_STRETCH`, which skips `StretchRect`s to measure their cost.

  The pass timer only times passes from the thread that used it first. The swap chain's blitter on another thread had
  raced it and crashed (`passlog::timeEnd`).

---

## 10. What was ruled out

Each of these was measured and changed nothing worth keeping, so they need not be tried again on this GPU.

| Suspect | Test | Result |
|---|---|---|
| Depth buffer | `USF_NO_DEPTH` | 24.2 fps (no change) |
| Blending | `USF_NOBLEND` | 26.2 (+2 only) |
| MSAA | forced to 0 | 24.2 (no change) |
| FP16 blending | render target format checked | it is 8-bit RGBA |
| Memory bandwidth for colour | counters | colour stays in tile memory (GMEM) |
| CPU in the menu | per-frame CPU time | 8–9 ms, not the limit |
| The `.rdata` 16:9 doubles | changed at run time | the letterbox stays (the fit is integer code, step 4) |
| Rewriting the renderer natively | the existing `USF_DXVK=0` path: 10–15 fps; the fixes above took DXVK to 60 | not needed |

---

## 11. Every switch

Set in `<data>/env.txt` (one `KEY=VALUE` per line, read at start) or in the environment.

| Switch | Default | What it does |
|---|---|---|
| `fsr` (port_settings) / `USF_FSR` | 0 | FSR 1.0 EASU when enlarging (the launcher's check box) |
| `DXVK_CENTROID` | 0 | 1 = DXVK's centroid inputs back (slow on Adreno) |
| `DXVK_HWC_SIZE` | set by the runtime | the swap chain's buffer size; the display scales them |
| `USF_NO_HWC` | unset | 1 = swap chain at the screen's size (the old path) |
| `USF_LETTERBOX` | unset | 1 = keep the game's 16:9 band with bars |
| `USF_ASPECT` | auto | the camera's aspect: auto (4:3 to 32:9), 16:9, 21:9, 32:9, a number |
| `DXVK_FB_CACHE` | 1 | 0 = a new framebuffer per render-target change |
| `DXVK_FRAG_BARRIERS` | 1 | 0 = barriers on every shader stage |
| `USF_LAYER_COPYALL` | unset | 1 = the touch layer copies the whole picture each post |
| `USF_LAYER_LOG` | unset | 1 = log the touch layer's first 200 posts |
| `DXVK_PASSLOG` / `DXVK_PASSTIME` / `DXVK_PASSSEQ` | 0 | pass log, GPU time per pass, pass order |
| `DXVK_FRAMELOG` | 0 | log `Clear` / `StretchRect` / viewport for N frames |
| `DXVK_SKIP_STRETCH` | 0 | skip `StretchRect` (analysis only: the picture breaks) |
| `USF_DXB_CALLER` | unset | log who in the game calls `SetViewport` / `StretchRect` |
| `USF_GPU_COUNTERS` | unset | Adreno counters, logged |

---

## 12. Files

| File | What changed |
|---|---|
| `tools/android/dxvk_mobile_nocentroid.patch` | no centroid on ps < 3 inputs |
| `tools/android/dxvk_mobile_adreno_perf.patch` | framebuffer cache, `DXVK_HWC_SIZE`, fragment-only barriers, blend check, pass timer and logs |
| `tools/lift/lift.py` | `FIT_SITES` and `GAME_PATCHES`: the 95 letterbox sites |
| `runtime/src/d3d9/d3d9_device.c` | `apply_aspect`: 4:3 floor, `usf_fill_screen`, `usf_fit_w/h` |
| `runtime/src/game.h` | `GAME_FIT_INV` / `GAME_FIT_ASPECT` (kept in step; not the cause) |
| `runtime/src/d3d9/dxvk_overlay.c` | FSR default, `DXVK_HWC_SIZE`, the touch layer's thread and buffer-age copy |
| `runtime/src/touch.c` | state copied outside the lock, sprite cache, changed-rectangle redraw |
| `runtime/src/d3d9/dxvk_bridge.c` | `DXVK_FRAG_BARRIERS` on, state cache path and asset, `USF_DXB_CALLER` |
| `android/.../GameActivity.java` | the touch layer's `SurfaceView` |
| `android/.../GameMenu.java` | the FSR 1.0 check box, off by default |
| `tools/android/perf_ab.sh` | automated A/B on the device |
| `tools/android/vkbench/` | micro-benchmarks with KGSL counters |
| `tools/android/build_*.sh` | macOS hosts (toolchain path), the Opus script's broken first line, a download fallback |

The DXVK tree (`private/deps/dxvk-mobile`) is not in git; the patches apply in the order given in
[ANDROID.md](ANDROID.md).

---

## 13. Lessons for DXVK on mobile GPUs

These hold beyond this game, for any D3D9 game on DXVK on a tiling GPU:

1. **Check what DXVK puts in shaders, not only what the game asks for.** One decoration (`Centroid`) cost 2.7x per
   pixel on Qualcomm. A micro-benchmark that isolates one shader difference finds this in minutes.
2. **Count full-screen passes.** On a tiler, every extra pass over the screen costs:
   - an upscale in the present: let the display scale with `setBuffersGeometry`;
   - an overlay in the present: give it its own surface;
   - a partial clear: it becomes a LOAD.
3. **A partial `Clear` is expensive on a tiler**: it forces `loadOp LOAD` of the whole attachment. Look at the clear
   rectangles in the frame log.
4. **Expensive upscalers are not free on weak GPUs.** EASU was 85% of the ALU work. Make it an option and let the
   display scale by default.
5. **Profile the CPU in real play, not in menus.** The framebuffer churn only showed in a level, and only as kernel time
   under the Vulkan driver.
6. **Anything the game thread waits on counts.** A lock shared with a UI thread can cost more than the GPU.
7. **On Android, never trust a window buffer to hold the last picture.** Use the buffer age, or copy everything.

---

## 14. Open items

- **Confirm the touch layer's buffer-age path on more devices.** It is tested on the Galaxy Tab S6 Lite; if a device
  shows ghost sticks, `USF_LAYER_COPYALL=1` is the fallback and the bug is in the age bookkeeping.
- **The display scaling and the touch layer are tested on the S6 Lite and the Galaxy S23 only.** `USF_NO_HWC=1` brings
  the old path back if a device misbehaves.
- **Ship a pipeline state cache** (`assets/rayman.dxvk-cache`) made by playing the final build.
- **DXT on Qualcomm's driver**: the log shows BC1 missing and DXT textures decoded on the CPU when loading. Transcoding
  to ETC2 would cut load-time CPU and GPU bandwidth (Turnip has BC).
- **Separate the framebuffer cache's gain from Turnip's** with `DXVK_FB_CACHE=0` on the same driver and the same stretch
  (with input replay).
- **A cheaper FSR** if it is wanted on weak GPUs: EASU in half precision, or bilinear plus RCAS.
- The `.rdata` fit doubles (`GAME_FIT_*`) are still written; they are harmless and could go.
