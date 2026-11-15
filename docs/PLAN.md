# Plan

**Goal:** Rayman Origins, the PC (Windows) build that people can still buy, running natively: the game's x86 code
recompiled ahead of time to C, built for Linux x86-64 first and Android arm64 next. No CPU emulation and no runtime
translation (no FEX, box64, Wine or Proton in the result). Vulkan 1.1, every phone GPU (Adreno, Mali, Xclipse,
PowerVR).

## Architecture

```
 Rayman Origins.exe (x86-32, read in place)   our code
 ──────────────────────────────────────────   ─────────────────────────────────────────────────
 .text  ──(lifter, ahead of time)──▶  C per function ──▶ clang ──▶ x86-64 (PC) and arm64 (Android)
 .rdata/.data ──(mapped at runtime)──▶  guest memory at 0x00400000.. (32-bit pointers stay valid)
 imports (455 functions, 23 DLLs) ───▶  runtime: our KERNEL32 / USER32 / MSVCR90 / ... answers
 d3d9 ───────────────────────────────▶  D3D9 layer on Vulkan 1.1 (D3D bytecode -> SPIR-V)
 d3dx9 (effects, HLSL compile) ──────▶  vkd3d-shader + our ID3DXEffect
 XAudio2 / DINPUT8 / XInput ─────────▶  SDL3 audio and input (Android: on-screen controls + gamepads)
 binkw32 ────────────────────────────▶  FFmpeg
 WinSock / Quazal / WinINet ─────────▶  offline stubs
```

## Reused, and from where

| From | What |
|---|---|
| UltraStreetFighterIVPCRecomp | `tools/funcs` (discovery), `tools/lift` (x86 → C), runtime (loader, heap, HLE tables, kernel32/user32/dinput/xinput/xaudio2/net), D3D9 → Vulkan (`sm3.c`, BC decode), FFmpeg movies, Android launcher and no-Gradle APK build, Hor+ ultrawide |
| RaymanOriginsRecomp (Xbox 360) | Knowledge only: `GFXAdapter_Directx9` vtable roles (docs/D3D_MAP.md), true-widescreen camera change, frieze and water render-to-texture quirks, Android home screen, touch layout, phone performance notes |

The Xbox 360 code (PowerPC, ReXGlue, Xenos) is not reused.

## Milestones

1. **M1 — Discovery and lift.** Bring the pipeline over, use the relocation table for function pointers and jump
   tables, every function lifted to C that compiles.
2. **M2 — Boot.** Loader + runtime with the MSVCR90/MSVCP90 shim; the game opens `bundle_PC.ipk` and reaches its main
   loop with a window and a D3D9 device.
3. **M3 — Shaders.** HLSL `.fx` → bytecode (vkd3d-shader), `ID3DXEffect`, then SPIR-V; menus drawn.
4. **M4 — Playable.** XAudio2 sound, input, Bink movies, a full level, saves in our own folder.
5. **M5 — Widescreen**, then **Android** (arm64 build, launcher, touch, shader cache at import).
