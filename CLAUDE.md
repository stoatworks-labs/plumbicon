# plumbicon

A camera tube as an FFGL **effect** (`PB01`) for Resolume Arena/Avenue: a
photoconductive target that stores charge, and an electron beam that reads it
by discharging it. Lag, comet tails, blocked highlights and burn-in all fall
out of that one store. C++17 + GLSL 4.10, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`. MIT.

**This is the camera, not the screen.** Nothing here models a display. Phosphor
persistence, ghosting, vertical hold, dot crawl and the shadow mask are
`old-cathode`'s, which is a sibling rather than a competitor — put this first
and that after it and you have the whole chain.

Read `AGENTS.md` before changing `targetField()`, the tube table or a tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/pbtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Type=2" --set "Beam Current=0.5"`
- List parameters, with type and range: `./build/pbtest --list`
- The test card alone: `./build/pbtest --card /tmp/card.png`
- Film a clip through it: `... | ./build/pbtest --pipe --size 1920x1080 [--script cues.txt] | ...`
  — raw RGBA frames on stdin, raw RGBA frames on stdout, the fleet's format, so
  one filming script drives any of the plugins. A `--script` line is
  `frame  Parameter Name  value`. **A reel has to be filmed from its first
  frame**: the target carries charge across fields, so there is no seeking.

Every parameter is 0..1 except `Type` and `Field Mode`, which are options and
are set by value (`Type=2` is Vidicon, not the second row of the dropdown).

## Verify
- Everything: `tools/verify.sh` (fresh universal build + all five checks + oxbow, ~15 s)
- A highlight switched off: `./build/pbtest --lag`
- The tail behind a moving one: `./build/pbtest --comet`
- The target saturates: `./build/pbtest --capacity`
- Both poles of the burn: `./build/pbtest --burn`
- A beam above capacity is the identity: `./build/pbtest --passthrough`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/pbtest --bench`

**Every one of those needs a GPU**, because the model lives in GLSL rather than
in C++. `--list` does not, and is answered before a context is created, which
is what lets CI run it on a runner with no GL at all. See AGENTS.md.

## Notes
- **Nothing is drawn.** Lag, comet tails, blocked highlights and burn-in are
  all consequences of `targetField()` in `Shaders.cpp`. If you are tempted to
  add a term to make one of them appear, the mechanism has been lost somewhere
  above and that is the bug. The **one** exception is the Image Orthicon's
  black halo, which is a different physical mechanism and is flagged as a
  bolted-on term everywhere it appears.
- **Time is in FIELDS, one per `ProcessOpenGL` call.** There is no clock in the
  charge model. `SetTime` is accepted and inert. Lag is a field figure in every
  tube data sheet, and counting fields is what makes every check exact.
- **`Capacity / Beam Current` is the length of the tail**, in fields.
  **`Beam Current / Sensitivity`** is the light level at which the target
  starts blocking up. They are independent and that is why both controls exist.
- **The beam current is also the CEILING on the signal**, so `VideoGain` in
  `Controls.cpp` lines the rest of the chain up with it. Without that, turning
  the lag up hands back a dim grey picture instead of a smeared one.
- **A tube type is an OVERRIDE, not a write.** `Effective()` is the one place
  that reads the table. Eight controls are inert unless `Type` is `Custom`, and
  `tools/sweep.py` gives every one of them a `Type=0` context.
- **A sub-unity Transfer Gamma lifts the picture**, because a real chain hands
  that signal to a display that re-applies about 2.2. This plugin is the
  camera; the correction is the monitor's job.
- `sample`, `half`, `layout`, `filter`, `input`, `output`, `common`, `active`,
  `patch`, `flat` are GLSL reserved words. A shader error surfaces only at
  runtime, in the diagnostics log, as "the effect does nothing" — and four of
  the six shaders here are assembled at run time, so the line number is in a
  file that does not exist.
- **Integer arithmetic for the line parity and the noise hash.** A float `mod`
  on a line index can return the divisor instead of zero, and
  `fract( sin( x ) )` depends on the driver's `sin`.
- **`ScopedFBOBinding` restores the framebuffer and NOT the viewport**; every
  `ffglex::Scoped*` binding clears to 0 rather than restoring. Allocate every
  buffer before anything binds a texture.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it, which is why every numeric control here is 0..1 and the conversions
  live in `Controls.cpp`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `plumbicon_core` is an **OBJECT** library, not STATIC — the plugin registers
  itself from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PB01`. Display name `SW Plumbicon` (16 characters is the limit).

## Memory
Two picture-sized **RGBA32F** buffers: 28 MB at 720p, 63 MB at 1080p, 253 MB at
4K. `Field Mode = Two Fields` doubles it. 32F is not caution — the burn
accumulator moves by 1e-5 of its range per field, an order of magnitude below a
half-float's epsilon at 1.0, so in 16F it would never start.

## Not done yet
- **Never loaded into Resolume**, on any platform. Everything here was compiled,
  rendered and measured offline against the real plugin class in a headless GL
  context, plus one load through `oxbow`.
- No OpenFX port, no browser demo, no user guide, no video, no Windows build.
- `ATTRIBUTIONS.md` and `source/StoatworksAbout.h` are provisional hand copies —
  the project is not registered in `sync-about.py` or `sync-attributions.py`
  yet, and `guide` is deliberately empty because no user guide exists.
- No factory presets beyond `Type`, which is an override rather than the
  fleet's copy-based preset mechanism.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It exists for the one failure that actually happens: a shader that
will not compile, which otherwise looks like "the effect does nothing" with no
message anywhere.

    ~/Library/Logs/plumbicon/plumbicon.YYYY-MM-DD.log       macOS
    %LOCALAPPDATA%\plumbicon\plumbicon.YYYY-MM-DD.log       Windows
