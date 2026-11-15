# Android (arm64)

The same lifted C as the PC build, compiled with the NDK for arm64, with the runtime, SDL3, shaderc (GLSL to SPIR-V on
the phone) and a small FFmpeg (Bink only) linked into `libmain.so`. The game files are the user's own PC copy, read in
place from the phone's storage. Status: **builds; not run on a phone yet** (no device was connected, 2026-11-15).

## Build

```sh
sh tools/lift/build_check.sh            # lifted objects for x86-64 and arm64 (private/obj/a64)
sh tools/android/build_ffmpeg.sh        # once: FFmpeg 7.1 for arm64 with the Bink demuxer and decoders only
sh tools/android/build_native.sh        # private/android/lib/arm64-v8a/libmain.so (+ libSDL3.so)
sh tools/android/build_apk.sh           # private/android/rayman-origins-pc.apk (no Gradle; a local debug key)
```

Needs NDK r28c, the Android SDK (build-tools 35, platform 35), JDK 17, an SDL3 3.4 Android build and source
(`SDL3_ANDROID`, `SDL3_SRC`), the NDK's shaderc built once (`private/android/shaderc`).

## On the phone

1. Install the APK.
2. Copy your Rayman Origins folder (`Rayman Origins.exe` and `GameData/`, 2.4 GB) to the phone, for example to
   `Games/RaymanOrigins`.
3. Open **Rayman Origins**, allow file access, **Select game folder** (or import a .zip of it), **Play**.

The launcher only finds the game. Everything else happens in the game, by touch:

- in play only the on-screen controls show (stick, jump, attack, back, run, pause);
- **pause** opens the game's own pause menu, with the port's settings under it: controls on/off, size, opacity,
  60 / 120 fps;
- the **resolution** is in the game's own Options; on the phone its list is the screen's shape at its full height and
  at 900, 720 and 540 lines (720 by default).

A physical controller works too (DirectInput pad through SDL).
