# The light copy of the game (2.4 GB → 560 MB)

The Steam copy of Rayman Origins takes 2.37 GB. Most of it is sound kept uncompressed. The light copy keeps
everything else byte for byte and stores the sounds as Opus. It takes **560 MB** (the archive 473 MB), and the game
cannot tell the two apart.

| Part | Steam copy | Light copy | How |
|---|---|---|---|
| Sounds (2,664 WAVE files in the archive) | 1,672 MB | 111 MB | Opus, 96 kb/s stereo / 64 kb/s mono |
| Other uncompressed files (scenes, scripts) | 115 MB | 51 MB | zlib, lossless |
| Textures, animations (already zlib) | 306 MB | 306 MB | as they are |
| Attract-mode movie `rolling_demo.bik` | 162 MB | — | left out: the game goes on without it |
| Intro, ending and sample movies, the program | 114 MB | 114 MB | as they are |

## On the phone (Android)

No step is needed: the launcher makes the light copy itself, once.

1. Install the APK (the GitHub release).
2. Copy your Rayman Origins folder (`Rayman Origins.exe` and `GameData/`) to the phone.
3. Open the app, then **Game files** → **Game folder**, and choose that folder.
4. The launcher shows **Making the light copy of the game (once)** with a percentage. It takes about a minute on a
   Galaxy S23 and needs 0.7 GB free.
5. The copy is written to the app's folder (`Android/data/io.github.belmantegu.raymanpc/files/game_lite`), and the
   game plays from it from then on.
6. The Steam folder you copied is left alone. You can delete it to get the 2.4 GB back.

### Keeping it, and installing it again

A .zip of the light copy goes back in as it is, with no new compression. It is useful after a reinstall or on
another phone.

1. Copy `game_lite` from the phone to the PC (or make it on the PC, below).
2. Zip it with a folder inside: `RaymanOrigins/Rayman Origins.exe`, `RaymanOrigins/GameData/...`. Use "store" (no
   compression), because the files are already compressed.
3. On the phone: **Game files** → **.zip file**, and choose the .zip. It replaces the light copy that was there.

A .zip of the whole Steam folder works too. It is extracted (2.4 GB), made light, and the extracted files are deleted
afterwards. It needs about 3 GB free while that runs.

## On the PC (Linux)

The PC build reads the light copy as well: put `bundle_PC.lite` where `bundle_PC.ipk` was (and without the `.ipk`).
To make it, and the .zip for the phone:

```sh
sh tools/lite/make_light.sh "$HOME/.local/share/Steam/steamapps/common/Rayman Origins"   # out: private/lite_out
```

It needs `libopus` (`pkg-config opus`) and zlib. The script:

1. builds `private/lite/lite_make` and `private/lite/lite_check`;
2. copies the program and the three movies to `private/lite_out/RaymanOrigins/`;
3. writes `GameData/bundle_PC.lite` (about 1 minute on 4 threads);
4. checks it against the original archive, and must print `wrong 0`;
5. writes `private/lite_out/RaymanOrigins-lite.zip`, ready for the phone's **.zip file** import.

## How it works

- **The format** (`runtime/src/lite.h`): the original archive's directory as it is, then the archive cut into
  regions (each file, and the gaps between files). Each region is stored one way: as it is, zlib, Opus, or nothing
  (zeros). A table maps each region to its offset in the original archive.
- **Making it** (`runtime/src/lite_make.c`, in the app as `libpack.so`):
  - Every 16-bit 48 kHz PCM WAVE becomes Opus: CELT only, 20 ms frames, VBR, complexity 10. Its header and trailing
    chunks are kept, and so is its exact number of samples, so loops stay exact.
  - Other uncompressed files are zlib level 9 when that saves a tenth.
  - The work runs on several threads, and the file is written as `.part` and then renamed.
- **Reading it** (`runtime/src/lite_read.c`): when `bundle_PC.ipk` is missing and `bundle_PC.lite` is next to it, the
  file layer answers with the original archive's bytes at the original offsets. That covers `CreateFile`,
  `ReadFile`, sizes, find-first/next and the C runtime's `_waccess_s` / `_stat`. Sounds are decoded progressively,
  as far as the game has read plus half a second, into a 96 MB cache.
- **The launcher** (`android/.../Packer.java`, `LauncherActivity.java`, `ZipImport.java`): it makes the copy at the
  first folder choice or the first Play, and imports a .zip.
- **Checked** (`tools/lite/lite_check.c`):
  - All 13,447 regions give back the original bytes, except the sounds' samples.
  - The sounds keep their headers and lengths and are aligned to the sample.
  - The copy made on the S23 checks exact too.
  - The game runs from it to a level, with its music and effects.

Not done: recompressing the textures more strongly (xz would save about 80 MB). The game wants them as zlib, so they
would have to be re-deflated at every load, and levels would load slower on a phone.
