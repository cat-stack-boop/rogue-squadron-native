# Architecture

```
User-supplied N64 ROM
  -> reviewed function/overlay metadata
  -> N64Recomp-generated C (local build output)
  -> Clang ARM64 executable
  -> native macOS host services
```

The generated game functions retain the original N64 memory/register model. A
lookup table selects precompiled functions for the currently loaded overlay.
Unknown function or hardware mappings stop with a diagnostic address; there is
no interpreter fallback or JIT.

`src/headless_runtime.cpp` binds the original OS calls to native thread, event,
cartridge, controller and EEPROM services. `src/native_video.cpp` hosts the
GLideN64 graphics-only interface through SDL/OpenGL. `src/native_audio.cpp` supplies
sample-clock DMA completions and output recovery; `native_rsp.c` connects the
MusyX mixer. The CPU and audio paths do not load an emulator CPU core or RSP
instruction interpreter.

The frontend supports keyboard input and standard SDL game controllers, with
focus/disconnect release, a radial dead zone and retained short button taps.
Window size in points and drawable size in pixels are communicated separately
between the SDL and render threads.

EEPROM is a 512-byte host file. Writes flush a temporary file, rename it atomically,
and flush the directory. A validated backup can restore a missing or malformed
primary. Complete images still use the original game's checksums and redundant
records. The original serializer can ignore low-level write errors, so a failed
host commit stops explicitly instead of returning a false success.

The separate `rogue_result_fixture` executable can request an original success
transition and supply synthetic result statistics for validation. Its hook is
excluded from the normal player executable and it refuses ordinary save paths.
It is an engineering fixture, not an automated pilot or a player release.
