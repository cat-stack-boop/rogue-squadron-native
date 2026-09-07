# Known limitations

- This is a playable development port. Full campaign compatibility is unverified.
  A synthetic completion tested the result/save boundary; no played victory is claimed.
- Graphics have not been exhaustively compared against an N64 reference. Untested
  scenes may expose rendering or runtime defects.
- Physical USB/Bluetooth controller compatibility and physical audio-device
  unplug/reconnect need broader validation. Controller remapping UI and rumble are absent.
- The graphics backend is GLideN64 through OpenGL 4.1, not a newly written Metal renderer.
  Desktop fullscreen is used instead of a separate macOS Space.
- Only Apple Silicon and the supported USA Revision 1 cartridge are targeted.
  Other hardware/software combinations and cartridge revisions are unverified.
- A failed save commit stops with an error in the runtime log. An interactive
  Retry/Quit dialog is not implemented. Host backup recovery handles missing or
  wrong-size files; complete 512-byte images use the original game's own handling.
- Local builds contain user-supplied game data. No redistributable player binary
  is provided here, and the local app is not notarized for public distribution.
