<div align="center">

# Rayman Origins — PC Recomp

**The Windows (Steam/Ubisoft) version of Rayman Origins, statically recompiled to native code.**

Every function of `Rayman Origins.exe` (x86-32, UbiArt Framework) translated to C and compiled for x86-64 and ARM64,
with its own Windows runtime and a Direct3D 9 → Vulkan 1.1 renderer. Linux first, Android next. No emulation, no Wine,
no FEX, no box64.

</div>

---

> [!IMPORTANT]
> **No game files are included.** This repository has no executables, DLLs, game archives (`.ipk`, `.bik`), shaders,
> recompiled output or artwork of the game. Everything derived from the game is generated on your machine into
> `private/` (git-ignored). You need **your own copy** of Rayman Origins for PC; it is read in place and never modified.

## Why the PC version

The Xbox 360 port ([RaymanOriginsRecomp](https://github.com/BelmanteGu/RaymanOriginsRecomp)) already runs on Android.
This one starts from the **PC build** because that is the copy people can still buy and keep: one install folder
(`Rayman Origins.exe` + `GameData/`, 2.4 GB), no console dump needed.

## Status

| Phase | Goal | Status |
|---|---|---|
| 0 | Read the executable and the data: imports, protection, engine, shaders | ✅ [study](docs/studies/pe-analysis-1511.md) |
| 1 | Function discovery + lifter (from USFIV-PC) produce C that compiles | ⏳ |
| 2 | Runtime boots the game to its main loop (window, D3D9 device) | — |
| 3 | HLSL `.fx` → SM2/SM3 bytecode (D3DX effects) → SPIR-V; first frames | — |
| 4 | Audio (XAudio2), input, Bink movies (FFmpeg), a full level | — |
| 5 | Widescreen / ultrawide, then Android (arm64, touch) | — |

Plan: [docs/PLAN.md](docs/PLAN.md) · log: [docs/PROGRESS.md](docs/PROGRESS.md) · rules: [docs/RULES.md](docs/RULES.md)

## How it will work

```
Rayman Origins.exe (x86-32) ──► function discovery ──► lifter ──► C ──► clang ──► x86-64 / ARM64

runtime (C)
 ├─ kernel32, user32, MSVCR90/MSVCP90 (host libc), DirectInput, XInput, XAudio2, WinSock/Quazal (offline) …
 ├─ D3DX9: math, ID3DXEffect, HLSL compile (vkd3d-shader) ──► SM2/SM3 bytecode
 ├─ Direct3D 9 ──► Vulkan 1.1   (D3D bytecode ──► SPIR-V)
 └─ binkw32 ──► FFmpeg (Bink video/audio)
```

The pipeline and most of the runtime come from
[UltraStreetFighterIVPCRecomp](https://github.com/BelmanteGu/UltraStreetFighterIVPCRecomp) (same compiler family,
same D3D9 → Vulkan path). Knowledge of the UbiArt engine (renderer adapter, widescreen, frieze/water quirks, Android
app) comes from the Xbox 360 port.
