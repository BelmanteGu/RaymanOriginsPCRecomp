# How this repository is kept

The same rules as [UltraStreetFighterIVPCRecomp](https://github.com/BelmanteGu/UltraStreetFighterIVPCRecomp).

1. **No game content in git.** No executables, DLLs, game archives (`.ipk`, `.bik`), shaders (`.fx`), textures,
   recompiled output or captures of game data. All of it lives in `private/`, which is gitignored, plus the patterns in
   `.gitignore`. The game itself is read in place from its install folder. No keys, no signing material.
2. **Private repository.** It stays private. Before it could ever change, every file is reviewed for game content.
3. **Files of at most 700 lines** (code, scripts and documents). A file that grows past it is split by topic, not
   trimmed. `tools/check_repo.py` enforces it.
4. **Everything is documented.** Each file starts with a comment that says what it is for and what it does not do.
   A question that takes real work gets a study in `docs/studies/` named `<topic>-<ddmm>.md`, with the numbers, the
   command that produced them and the conclusion. `docs/PROGRESS.md` gets a dated entry for each step.
5. **Screenshots.** Every step is documented with an image in `docs/media/` (JPEG) referenced from the study or the
   progress entry. A step that only has text output gets a terminal picture made from the tool's real output with
   `tools/term_shot.py`.
6. **Say only what was tested.** A claim is marked verified (with where: machine, date) or not verified. A failed
   attempt is written down too, with what it ruled out.
7. **Reference first.** The PC build running under Proton is the oracle; the Xbox 360 port
   ([RaymanOriginsRecomp](https://github.com/BelmanteGu/RaymanOriginsRecomp)) is a second reference for the same
   engine. A behaviour is compared with them before it is called right or wrong.
8. **No circumvention.** The runtime answers the calls of a game the user owns. A copy-protection wrapper is not
   removed or bypassed: if a build of the game carries one, the work stops and is reported.
9. **Commits** are small, one per step, say what changed and why in English, and are never pushed with game content
   in them (`python3 tools/check_repo.py` and `git diff --cached --name-only` are checked first).
