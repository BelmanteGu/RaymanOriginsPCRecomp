# Android (arm64)

The same lifted C as the PC build, compiled with the NDK for arm64, with the runtime, SDL3 and a small FFmpeg (Bink
only) linked into `libmain.so`. Direct3D 9 goes to `libdxvk_d3d9.so`: dxvk-native 1.9.2b with mobile-GPU patches, its
window system moved to SDL3 (Vulkan 1.1; `private/deps/dxvk-mobile`, from the Sonic & All-Stars Racing Transformed
port), loaded next to `libmain.so` by the same bridge as on the PC. The game files are the user's own PC copy, read in
place from the phone's storage. How it reached 60 fps at 720 lines on an Adreno 618, step by step:
[ANDROID-PERFORMANCE.md](ANDROID-PERFORMANCE.md). Status: **runs on the Galaxy S23** (2026-11-19, played by the user).

## Build

```sh
sh tools/lift/build_check.sh            # lifted objects for x86-64 and arm64 (private/obj/a64)
sh tools/android/build_ffmpeg.sh        # once: FFmpeg 7.1 for arm64 with the Bink demuxer and decoders only
sh tools/android/build_dxvk_mobile.sh   # once: private/android/lib/arm64-v8a/libdxvk_d3d9.so (DXVK, Vulkan 1.1)
sh tools/android/build_native.sh        # private/android/lib/arm64-v8a/libmain.so (+ libSDL3.so)
sh tools/android/build_apk.sh           # private/android/rayman-origins-pc.apk (no Gradle; a local debug key)
```

Needs NDK r28c, the Android SDK (build-tools 35, platform 35), JDK 17, an SDL3 3.4 Android build and source
(`SDL3_ANDROID`, `SDL3_SRC`), the NDK's shaderc built once (`private/android/shaderc`).

### DXVK on another PC

`private/` is not in git: copy `private/deps/dxvk-mobile` (and `private/deps/libadrenotools`) from a PC that has them,
or start again from dxvk-native `native-1.9.2b` with the mobile patches of the Sonic ASRT port and apply this port's
patches in this order (`patch -p1` inside `private/deps/dxvk-mobile`):

1. `tools/android/dxvk_mobile_gipa.patch` (the chosen driver's vkGetInstanceProcAddr)
2. `tools/android/dxvk_mobile_passlog.patch` (render pass log, analysis)
3. `tools/android/dxvk_mobile_lean.patch`, `tools/android/dxvk_mobile_lean_ff.patch` (lean shaders)
4. `tools/android/dxvk_mobile_window.patch` (a new surface after leaving and coming back)
5. `tools/android/dxvk_mobile_prerotate.patch` (pre-rotation)
6. `tools/android/dxvk_mobile_overlay_fsr.patch` (touch layer at the screen's resolution, FSR 1.0 EASU)
7. `tools/android/dxvk_mobile_nocentroid.patch` (no centroid on ps < 3 inputs: 2.7x slower per pixel on the Adreno)
8. `tools/android/dxvk_mobile_adreno_perf.patch` (framebuffer cache, the window's buffers at the game's size, fragment-only
   barriers, no blending where a format cannot blend)

The new shaders' headers are made with `glslangValidator -V --vn <name> -o generated/<name>.h <file>` (names in the
patches' headers), then `sh tools/android/build_dxvk_mobile.sh`.

## On the phone

1. Install the APK (the [GitHub release](https://github.com/BelmanteGu/RaymanOriginsPCRecomp/releases)).
2. Copy your Rayman Origins folder (`Rayman Origins.exe` and `GameData/`, 2.4 GB) to the phone, for example to
   `Games/RaymanOrigins`, or a .zip of it.
3. Open **Rayman Origins**, allow file access, **Game files** → **Game folder** (or **.zip file**).
4. The first time, the launcher makes the light copy of the game (0.6 GB, about a minute on the S23; see
   [LIGHT_COPY.md](LIGHT_COPY.md)), then **Play**. A .zip of the light copy is taken as it is.

The launcher only finds the game. Everything else happens in the game, by touch:

- in play only the on-screen controls show (stick, jump, attack, back, run, pause);
- **pause** opens the game's own pause menu; the **gear** opens the port's menu over the game (the launcher's
  Options, the button-layout editor, back to the launcher);
- the **resolution** is in the game's own Options; on the phone its list is the screen's shape at its full height and
  at 900, 720 and 540 lines (720 by default).

**GPU driver** (Adreno only): Game files → GPU driver lists the system's driver (Turnip off), the drivers already
installed, Turnip builds to download from GitHub (Turnip 26.3 R6 recommended: on an Adreno 610 only Mesa 26.3 or
newer works) and "Import driver (.zip)" for a pack of one's own (meta.json + the .so, the Winlator / Yuzu format).
The choice applies at the next start; a driver that never draws sends the game back to the system's at the start
after, and the launcher says so.

A physical controller works too (any SDL gamepad, read by the game as an XInput pad).
