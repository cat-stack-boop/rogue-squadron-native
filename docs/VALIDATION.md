# Validation scope

The development process exercised original menus, pilot/settings persistence,
mission entry, flight and pause in Ambush at Mos Eisley and Rendezvous on Barkhesh.
It also checked window resizing, Retina output, desktop fullscreen and normal quit.

Focused native probes cover floating-point behavior, scheduling, input, audio
buffer timing/recovery, and host save integrity. Save tests include a short write,
an interrupted temporary-file write, and original save-codec round trips checked
against independent checksums. Physical controller and audio-device unplug/reconnect
coverage remains incomplete.

Campaign result validation used a **synthetic** completion event and score in a
separate test build. The original game awarded a medal, advanced the account and
wrote both save records. The normal build then restored the result and loaded the
newly unlocked mission. This verifies that boundary; it does not establish a
legitimate played victory, objective-completion coverage, or full campaign compatibility.

For the optional fixture, build normally, package with
`python3 tools/package_dev_app.py --result-fixture`, then run
`python3 tools/run_result_fixture.py` and enter the first mission through the menus.
The launcher uses a marked, isolated first-mission save and a bounded runtime.
The fixture app is never the player deliverable.

Raw recordings, saves, machine inventory, process identifiers, paths and historical
logs are intentionally absent from the public repository. Generate fresh evidence
locally as needed. Keep it in the ignored `reports/` and `runtime/` directories.
