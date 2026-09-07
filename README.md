# Rogue Squadron native macOS port

Experimental Apple Silicon port of the N64 version of **Star Wars: Rogue Squadron**,
using static recompilation and the original game assets supplied locally by the user.

The original MIPS game code is translated into C with N64Recomp, then compiled into
an ARM64 executable. The macOS host is primarily C++ with C audio components and
Python build/analysis tools. There is no runtime N64 CPU interpreter or JIT, and
no Wine or Rosetta dependency. Graphics and audio use emulation-derived HLE
components; this is not a claim of zero emulation or a full source decompilation.

**This repository contains source, configuration and patches only.** It includes
no ROM, extracted game assets, generated game-code translations, saves, screenshots,
runtime logs, or playable app binaries. Supply your own supported cartridge image.
The resulting local app contains game data and is not intended for redistribution.

## Status

The development build has demonstrated original menus, player flight, native
keyboard input, window resizing/fullscreen, audio and persistent saves. The SDL
controller adapter has been tested with virtual devices; physical-controller
coverage remains incomplete. A separate synthetic-completion fixture exercised
medals, account advancement, saving and reload into the next mission. This was
not a played victory and does not establish full campaign compatibility.

See [controls](docs/CONTROLS.md), [architecture](docs/ARCHITECTURE.md),
[validation scope](docs/VALIDATION.md), and [known limitations](docs/KNOWN_LIMITATIONS.md).

## Build locally

Prerequisites: an Apple Silicon Mac, Xcode Command Line Tools, Homebrew SDL2,
Python 3.11 or newer, and Git. Tooling versions are pinned in
`requirements-tools.txt`; dependency revisions are pinned in `config/source-pins.json`.

Place a supported, big-endian USA Revision 1 image at:

```
roms/rogue_squadron.us.rev1.z64
```

Expected size: 16 MiB. SHA-256:
`4813551d01d3a3474df3a51f84c31059cd2a9d1eeae7885dda51a88ee6b9f88d`.
This is a public cartridge identity, not a user-specific hash.

```sh
mkdir -p roms reports build
python3 tools/bootstrap.py
python3 tools/inspect_rom.py roms/rogue_squadron.us.rev1.z64 --report reports/rom.json
python3 tools/extract_assets.py roms/rogue_squadron.us.rev1.z64
.venv/bin/python tools/generate_candidates.py
python3 tools/build_game.py
python3 tools/package_dev_app.py
```

Open `build/Rogue Squadron Development.app`. It resolves runtime resources from
its bundle and stores saves in the current user's Application Support directory.
The app is signed locally, not notarized for public distribution. Diagnostic and
build outputs stay in ignored directories.

## Focused tests

After a successful build:

```sh
python3 tools/test_safety.py
python3 tools/build_probe.py
python3 tools/test_matrices.py
build/game/rogue_thread_probe
build/game/rogue_io_probe
build/game/rogue_rsp_probe
build/game/rogue_boot_probe
build/game/rogue_controller_probe
build/game/rogue_gamepad_probe
python3 tools/test_audio_devices.py
python3 tools/test_app_package.py --normal-launch --seconds 45
```

Save tests require an existing valid save created by a normal launch:

```sh
python3 tools/test_eeprom.py
python3 tools/test_app_package.py --normal-launch --seconds 45 --recover-save
```

They copy the save into disposable test directories and verify that the original
stays unchanged. Do not upload local test reports: they can contain paths, device
information and save data. `python3 tools/check_public_tree.py` checks the tracked
source tree before publication.

## Credits and licensing

This work depends on substantial existing community tooling and research:

- [N64Recomp](https://github.com/N64Recomp/N64Recomp): static binary-to-C recompilation.
- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime): native runtime services.
- [GLideN64](https://github.com/gonetz/GLideN64): graphics HLE and OpenGL rendering.
- [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle): MusyX audio HLE.
- [rogue_squadron64](https://github.com/Tmcg2/rogue_squadron64): game research and symbol/format references.
- [SDL2](https://github.com/libsdl-org/SDL): macOS window, input and audio integration.

Existing third-party licenses and credits are preserved in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `LICENSES/`. No new project-wide
license grant is added for the project-specific files in this publication.
Star Wars and Rogue Squadron names and game content belong to their respective owners;
no affiliation or endorsement is implied.
