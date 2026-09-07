# Controls

These controls use the original game's **Luke** preset. Other presets selected
in the game's settings can change the actions.

## Keyboard

| Key | Action |
| --- | --- |
| Arrow keys | Move the control stick |
| Shift + arrow keys | Fine steering at one-quarter stick deflection |
| X | Fire blasters |
| Z | Thrust |
| Space | Brake / airspeeder left brake |
| J | Fire secondary weapon |
| A | Switch camera view |
| S | Roll while steering / airspeeder right brake |
| K | Change blaster linking |
| L | Craft special function; opens/closes the X-wing's S-foils |
| I + arrows | Look around |
| Return | Start / Pause |

In menus, Z is the N64 A button, X is B, and Return is Start. Select Level uses
Start to commit the mission. Other original control presets can change actions.

## Gamepad

Connect a controller recognized by SDL before launching or while the app is
running. The first recognized controller controls player one. Disconnecting it
releases its inputs and selects another connected controller, if available.

Button positions below stay consistent across controller brands. Xbox labels
are examples; use the physical position when your controller's labels differ.

| Gamepad control | Action with Luke preset |
| --- | --- |
| Left stick | Steer / navigate menus |
| South face button (Xbox A / PlayStation cross) | Thrust / confirm menu choice |
| East face button (Xbox B / PlayStation circle) | Fire blasters / menu back |
| Right trigger | Fire blasters (also sends the menu Back button) |
| Left trigger | Brake / airspeeder left brake |
| West face button (Xbox X / PlayStation square) | Secondary weapon |
| North face button (Xbox Y / PlayStation triangle) | Craft special, including X-wing S-foils |
| Left shoulder | Switch camera |
| Right shoulder | Roll while steering / airspeeder right brake |
| Back / View / Share | Change blaster linking |
| Hold right stick click + move left stick | Look around |
| Start / Menu / Options | Start / Pause; commit the mission on Select Level |
| D-pad | Original directional-pad camera shortcuts |

The left stick has a 15% radial dead zone to suppress center drift. Keyboard
steering takes priority while an arrow key is pressed; otherwise the gamepad
supplies analog steering. Keyboard and gamepad action buttons can be combined.

Inputs are released when the game window loses focus. Pause before switching
apps if you want the mission to stop. Short button taps are retained briefly so
the original game's input polling can see them.

Gamepad mappings, analog steering, focus release, hotplug, unplug and reconnect
passed tests using SDL virtual controllers. Physical USB/Bluetooth controller
compatibility and rumble have not been verified; rumble is not implemented.

## Gameplay notes

Resize the window by dragging an edge, or use the green window button to zoom.
F11 toggles desktop fullscreen and Escape leaves fullscreen. The renderer keeps the original
4:3 picture with bars when needed and uses Retina drawable resolution. Command-Q
quits the app. Fullscreen uses the current desktop rather than a separate macOS
Space; this avoids the failed Spaces transition observed on the target Mac.

With X-wing S-foils closed, speed increases; open them for weapon use.

## Audio output

If the Mac's audio output fails, the window title shows **audio reconnecting**.
The game keeps running and retries the output automatically. Sound resumes after
a short buffer fills; missed sound is discarded so it does not play seconds late.
Pause the game if you want to wait for the output device to return.

## Saves

Saves live in `~/Library/Application Support/Rogue Squadron Native/saves/`.
The main file is `rogue-squadron.eep`; `rogue-squadron.previous.eep` holds a
checksum-valid copy from before the session's first write, when available.

If the main file is missing or has the wrong size, the app restores a valid
backup automatically. A malformed main file is kept as `rogue-squadron.damaged.*`.
Complete 512-byte files are passed to the original game's own checksum and
redundant-record handling. Diagnostic tests use separate saves.

A disk write failure stops the app and records the error in its runtime log,
because the original game can otherwise treat a failed EEPROM write as success.
The previous committed file remains intact when a temporary-file write fails.
