# plumbicon — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that puts a camera
tube in front of a clip. C++17 + GLSL 4.10, CMake, universal macOS `.bundle`
and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/plumbicon`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching `targetField()`, the tube table, or any
tolerance in the harness.

---

## The one idea

**A camera tube's target is a photoconductive layer that stores charge.** Light
charges it; the scanning electron beam reads it by *discharging* it; and the
beam can only remove so much charge per pass.

That is the whole plugin. It is one function — `targetField()` in
`Shaders.cpp`, about fifteen lines — and everything the tube is famous for is
what is left over rather than something that was arranged:

- **Lag.** A highlight that charged the target harder than the beam can
  discharge is still sitting there on the next field, so it is read out again.
  `charge - min( charge, beam )` is the entire mechanism and there is no lag
  term anywhere else in the repo.
- **A comet tail.** That residue is at *every point the highlight has passed
  over*, and a moving highlight has passed over a line of them. Nothing draws a
  tail. `pbtest --comet` measures one and gets the length the discharge
  recursion predicts, to the pixel.
- **Blocked highlights.** `min( ..., capacity )`: past capacity, more light is
  not more charge, so the detail inside a highlight is never stored and cannot
  be read back out. A tenfold and a hundredfold over-exposure come out
  **bitwise identical**.
- **Burn-in.** A second accumulator with a time constant in minutes, biasing
  the local sensitivity down where the target has been bright, recovering on a
  slower pole than it rose on — because a tube etches faster than it recovers.

Two smaller things fall out too, and neither was aimed at:

- **A smear changes colour.** The store is per channel, so a bluish highlight
  lags in blue and not in red. The moving bar in the test card is
  `(0.90, 0.95, 1.00)` and leaves a blue trailing edge that nothing in the code
  asks for.
- **Two Fields doubles the tail.** The beam visits a line every other field, so
  a line is discharged half as often and holds its residue twice as long. That
  is one uniform, not a mode.
- **And Two Fields twitters on motion, and only on motion.** A moving edge is
  caught at two different positions on alternate lines, because the two sets of
  lines were last read one field apart; a static area has no line structure at
  all. Nothing in the code has an opinion about lines except the parity test,
  and what comes out is interlace twitter on exactly the parts of the picture
  that move. It is visible in a render of the test card at
  `Field Mode = Two Fields` and it was not designed.

### What this is NOT

**It is not a screen.** Nothing here models a display: no phosphor, no
ghosting, no vertical hold, no dot crawl, no shadow mask, no scanline, no
tracking error. Every one of those belongs to **old-cathode**, which is a
sibling in this fleet and not a competitor. Put this plugin first and
old-cathode after it and you have the whole chain, camera then monitor. Read
old-cathode before changing anything here, and then stay out of its territory:
the moment this plugin grows a phosphor it has stopped being a camera.

**One word appears in both plugins and it is not a duplicate.** old-cathode has
halation too -- the beam blooming in the glass of a *display* when it is driven
hard. This plugin's halation is light scattering in the *faceplate of a camera*
before it ever reaches the target. Same word, opposite ends of the chain, and
both are real; run the two together and a bright caption gets both, which is
what actually happened. The same goes for noise: old-cathode's is on the
composite, after the encoder, and arrives as coloured speckle through the
decoder. This plugin's is beam and preamp noise at the camera, additive and
level-independent, which is why it shows up in the shadows.

A consequence worth knowing, because it looks like a defect: **a Transfer Gamma
below 1.0 lifts the picture.** A vidicon's transfer characteristic really is
about 0.65, and a real chain hands that signal to a display which re-applies
something near 2.2. This plugin is the camera. It does not correct for a
display it does not model, and the Vidicon type therefore looks milky on its
own — which is both faithful and, on purpose, old-cathode's problem rather than
ours.

### The one bolted-on term, declared

**The Image Orthicon's black halo does not fall out of the charge store, and
it could not.** It is redistribution of secondary electrons: electrons knocked
off the target by a highlight land back around it, so the ring is a *deficit*
of signal in an annulus around a bright area. Nothing in `targetField()` lets
one point on the target affect another, so no amount of care with the store
will produce it.

So it is drawn. `Halo` subtracts a difference of two Gaussians — positive in an
annulus around a highlight, negative in the middle — from the signal. That is
an **arrangement** of the artefact, not a derivation of it. It is flagged in
the shader, in `Controls.h`, in `Tubes.h` and here, and `Type` pins it to zero
on the three tubes that do not do it.

The spec asked for it to be flagged, and this is that flag. If a future version
wants it honestly, the mechanism to model is charge transport *across* the
target, which is a different plugin.

---

## The traps

Ordered by how much time they will cost you.

**The beam current is also the CEILING on the signal, and forgetting that makes
lag into a brightness control.** `min( charge, beam )` means that turning the
beam down to get a longer tail also clips peak white to whatever the beam is —
so the first version of the tube table produced an Image Orthicon whose
highlights were mid grey and whose whole picture was dim. That is not what a
camera does: a camera is lined up by putting the beam where it just handles
peak white and then setting the video amplifier so that reads as white.
`VideoGain()` in `Controls.cpp` is that second half. With it, turning the beam
down clips the highlights and *lifts* everything under them, which is what an
over-driven tube looks like.

Two things follow. **The length of the tail is `capacity / beam`**, not the lag
control, which is why `Target Capacity` is pinned by `Type` and why the Lag
Amount range is 9:1 rather than something more dramatic. And **the gain is
exactly 1.0 at the pass-through settings**, which is what lets that check still
claim the identity — `x * 1.0` is exact, and anything else is not.

**An 8-bit clip has no headroom, and a comet tail needs some.** A real comet
tail comes from an object a hundred times brighter than peak white: a lamp, a
sun glint, a welding arc. A clip has no such object — everything above white
was thrown away before the plugin ever saw it. So the only way to get a tail is
to over-expose the clip's own highlights past the point where the beam can keep
up, and that necessarily costs headroom in the midtones. There is no setting
that gives a comet tail on a correctly exposed 8-bit picture, because there is
nothing in one for the tail to come from. The plugin's description says so and
the README says so; do not try to tune around it.

**A held signal costs a second pair of buffers, and Two Fields needs one.** The
obvious implementation of Two Fields is to discharge alternate lines and show
`min( charge, beam )` on every line. It is wrong, and it is wrong in a way that
looks like a comb filter: an unvisited line has had *two* fields of light
rather than one, so every other line comes out twice as bright and the whole
picture is a 2:1 line pair. The picture has to show what the beam actually took
last time it was there, which means storing it — a second RGBA32F ping-pong
pair, allocated only in that mode. That is why `held[]` exists and why Frame
mode costs nothing for it.

**`PassBuffer::Ensure()` cannot tell you whether it reallocated.** It returns a
bool meaning "you have a buffer", and reuses one that already matches. (The
spec for this build said it returns `this`; in the C++ it is a bool — the
`ensure()`-returns-`this` shape is the fleet's *browser demo* kit, not
`PassBuffer`.) Either way the width and height have to be tracked separately,
because a resize must **clear** the target: a charge store carried across a
resize is not a frame of noise, it is the last composition fading out over the
first second of this one.

**`ScopedFBOBinding` does not restore the viewport.** It restores the
framebuffer binding and only that (SDK `b1afaf9`,
`FFGLScopedFBOBinding.cpp`). So every pass's `ResizeViewPort()` leaks into the
pass after it, and the composite — which draws to the host's framebuffer and so
has no buffer of its own to size itself from — inherits whatever the last pass
left. Here that would be the quarter-size bloom buffer, and the effect would
render into the bottom-left quarter of the frame. `ProcessOpenGL` captures the
host viewport up front and restores it before the composite.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its new colour texture under one of
those, so *allocating a buffer silently unbinds your input texture from the
active unit*. The symptom is the dangerous part: correct on every frame except
the one that allocates. Every `Ensure()` happens before anything binds a
texture.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second
time where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes
it first. On this plugin that is not pedantry: the state buffers are RGBA32F at
picture size, so one leaked texture is 133 MB at 4K.

**The state buffers are NEAREST, deliberately.** They are data, not a picture.
Every pass reads them texel for texel; a `GL_LINEAR` fetch at a texel centre is
*meant* to return that texel exactly, and on a buffer that feeds back into
itself once per field, "meant to" is not a thing to rest a charge store on.
The consequence is that the bright pass has to write its 4x4 box average out
by hand rather than letting the hardware do it.

**A float `mod` is not safe for a line index.** GLSL defines `mod` as
`x - y * floor( x / y )`, and where the division rounds a hair below an integer
the result comes back as the divisor rather than as zero. That cost tinsel
sixty wrong lamps out of half a million and does not reproduce on the CPU. The
field parity is `( int( gl_FragCoord.y ) & 1 ) == parity`, integer both sides.

**The parity that undoes a beam pass is the PREVIOUS field's.** The state
buffer holds the charge after this field's light and *before* this field's
beam, so the pass that computes it has to undo the read that happened at the
end of the field before. Using this field's parity there puts the discharge on
the wrong lines and Two Fields becomes a comb filter for no visible reason.
`PrevParity` and `FieldParity` are both uniforms for that reason.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
can only be called afterwards. Every numeric control here is 0..1 and the
conversions live in `Controls.cpp`.

**`SetParamGroup` collapses RUNS of consecutive same-group ids.** The enum
order in `Plumbicon.h` is therefore load-bearing: insert a parameter mid-enum
and a group silently splits in two, *and* every saved composition renumbers.
Append only.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. The core is an **OBJECT** library for that
reason. `tools/verify.sh` checks both `nm -gU` and an actual host load through
`oxbow`.

**`set -o pipefail` plus `grep -q` is a race, and the big binary loses.**
`nm -gU "$bin" | grep -q _plugMain` reports failure *because* the symbol was
found: `grep -q` exits at the first match, `nm` takes SIGPIPE, and `pipefail`
propagates it. `verify.sh` captures into a variable and matches with `case`.

**`pow( x, 1.0 )` is not the identity, and `log2( 1.0 )` need not be zero.**
GLSL 4.10 §8.2 gives `log2` an absolute error under 2^-21 inside [0.5, 2.0], so
`pow( 1.0, 1.0 )` is allowed to come back as 1 ± 7e-7. Any check that feeds the
model a white field and predicts exactly one is wrong by that much before its
own arithmetic starts. This is not hypothetical — it is what the tolerance
audit below found in `--burn`, where the original tolerance at n = 1 was
4.8e-7, tighter than the specification allows.

---

## Shape of the code

    source/Shaders.cpp      ALL the physics. kTargetLibrarySource is a
                            fragment, not a shader -- no #version, no main --
                            and the four passes that need targetField() are
                            each assembled around the same string at run time.
    source/Controls.*       0..1 host parameters to the target's own units,
                            plus VideoGain.
    source/Tubes.h          the four tube types, as an OVERRIDE table.
    source/PassBuffer.*     FFGLFBO with the leak fixed, three sampling modes.
    source/Plumbicon.*      the plugin: parameters, buffers, the frame.
    source/Diag.*           a log file, for the shader that will not compile.
    tools/pbtest/           the offline harness.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it, plus what the release job would check.

The frame, in order:

1. **target** — picture size, RGBA32F, ping-ponged against itself. `rgb` is the
   charge the target holds after this field's light and before the beam reads
   it; `a` is the burn. The only pass with any physics in it.
2. **held** — picture size, RGBA32F, ping-ponged. **Two Fields only.** The
   signal from the last field on which the beam actually visited each line.
3. **bright** — quarter size. The signal above Bloom Threshold, box-averaged
   over the 4x4 full-size texels each output texel covers.
4. **blur** — quarter size, four times: two axes narrow, two axes wide. The
   narrow one is the halation; the *difference* between them is the halo ring.
5. **composite** — output size, to the host.

The signal itself is never a pass. It is `min( charge, beam ) * gain`, a pure
function of the state, computed wherever it is wanted from the same library
function.

### Why the model is GLSL and not C++

Every other plugin in this fleet with a physics claim has a CPU half a test can
call with no context at all. This one does not, and that is a real cost: it
means **every numeric check here needs a GL 4.1 context**. A hosted runner has no
GPU, but the harness falls back to Apple's software renderer, which does give
one — so CI runs the physics there too, on a rasteriser that is not the
development GPU (see the CI bullet under "What is not done").

The reason is that the model is genuinely per pixel and per field. A charge
store is a picture-sized state that has to be updated once per frame; the only
sensible place for it is a texture, and the only sensible place for the update
is a fragment shader. Mirroring it in C++ would mean a second implementation of
the one thing this plugin is, and the fleet's experience of mirrored models
(tinsel's `--effects`) is that they are worth it when the CPU side is *used* by
something — an OpenFX build, a demo — and a liability when it exists only to be
compared against.

What is done instead: the harness drives the **real plugin class** through the
real shader text, and predicts what should come out from the **same physical
constants the plugin uploads**, read out of `Controls.h` rather than typed in
again. `tools/verify.sh` is the gate on a machine with a GPU; CI is the same
five checks on the runner's software renderer.

---

## Every numeric check, and where its tolerance comes from

This section exists because of a specific failure. In a previous round, four of
six plugins shipped checks whose tolerances had been calibrated to the number
this Mac's GPU printed first; all four failed on a GPU-less CI runner, and in
all four cases **the test was wrong, not the plugin**. So every numeric
assertion in `tools/pbtest` was walked through once, deliberately, asking two
questions of each: *would this still hold on a different rasteriser, and at a
different raster?*

Two rules came out of it and they are followed everywhere below.

**Prefer a tolerance derived from the physics or the specification** — one ULP
of the recursion, one source field, one lattice cell, the GLSL spec's stated
accuracy for a function — over one fitted to an observation.

**Every check that could depend on the raster runs at two rasters and fails if
they disagree.** Cheap, and the only way to find out.

### The checks

| # | Check | Assertion | Tolerance | Where it comes from |
|---|---|---|---|---|
| 1 | `--lag` | the four probes across the frame agree | `8 × f × ½ ULP(capacity) × gain` | on a flat field every pixel runs identical arithmetic on identical inputs, so this is **bitwise** in principle; the lag tolerance is reused because it can only be more permissive |
| 2 | `--lag` | signal at dark field *f* equals `min( max( 0, K − fB ), B ) × gain` | same, 1.38e-5 | `min` is exact; each subtraction is correctly rounded, so over *f* fields the error is at most `f × ½ ULP(K)`. The ×8 is margin. Measured worst: **3e-7** |
| 3 | `--lag` | the two rasters agree | same | per-pixel arithmetic on a flat field; could be zero |
| 4 | `--lag` | field `ceil(K/B)` reads **exactly** zero and the field before it does not | exact, no tolerance | once `charge ≤ beam`, `charge − min( charge, beam )` is `x − x`, which is exactly zero in IEEE-754 on any conforming implementation. The non-zero side has five orders of magnitude of margin |
| 5 | `--lag` | Two Fields follows the alternating recursion | `8 × 2f × ½ ULP(K) × gain` | same argument over twice as many fields. The check does **not** say which row has which parity — that is a fact about the raster's origin, not about the physics — and accepts either assignment |
| 6 | `--lag` | the Two Fields tail is at least `2 × fields − 1` | integer | the −1 is the parity phase: whether a line's first dark read lands on the field after the last lit one depends on where the switch-off fell |
| 7 | `--comet` | tail length = `v × floor( K/B − 0.5 )` pixels | **±1 pixel** | the spec's figure. Everything in the prediction is integer: `v` is integer pixels a field, positions are integer, and `ceil( d/v ) ≤ F ⟺ d ≤ vF` |
| 8 | `--comet` | the last lit pixel and the first dark one are each ≥ 0.15 of a full signal clear of the threshold | precondition on the constants, not on the GPU | a tail measured at a level the signal crosses near a pixel boundary is a coin flip dressed as a measurement. If this fails the right fix is to re-choose the constants, never to widen the tolerance. Measured: +0.500 and +0.223 |
| 9 | `--comet` | the raster is big enough to hold the patch and its tail | stated and checked | the only way the raster enters this check at all |
| 10 | `--capacity` | the 10× and 100× patches are equal | `2 × FLT_EPSILON × max( source, signal )`, 2.4e-7 | `min( x, K )` returns `K` unchanged whenever `x > K`, so the charge and the signal are bitwise identical for both patches. The OUTPUT is not: the last line is `mix( source, signal, Mix )` and the source is the light itself, 0.1 in one patch and 1.0 in the other. GLSL defines `mix` as `x(1−a) + ya`, exact at a = 1, but an implementation may evaluate `x + (y−x)a`, which rounds the subtraction at the scale of 1.0. GitHub's software renderer does, and the 100× patch came out 7.5e-9 (one ULP of the output) off — see below. Two half-ULPs of the larger operand bound it |
| 11 | `--capacity` | both equal `capacity × gain` | same | the harness performs the *same single-precision multiply* the shader does, and then the same `mix` argument applies |
| 12 | `--capacity` | a patch at a quarter of capacity comes out different | `< half` of the saturated value | without it, a plugin that emitted a constant would pass this check with full marks |
| 13 | `--burn` | burn at field *n* follows `D + (b₀ − D)(1 − pole)ⁿ` | `4 × n × FLT_EPSILON + 2 × kPowBound` | *n* fields of subtract–multiply–add, at most ~2 ULP each, times a safety factor of 2 — **plus** the pow term, which the audit added. Measured worst: **2e-7** against 2.1e-4 |
| 14 | `--burn` | the video gain and the sensitivity are exactly 1 | exact | the prediction is `output = 1 − burn`, which is only true at unity gain. Asserted so that a moved range in `Controls.cpp` cannot turn this into a measurement of the gain |
| 15 | `--passthrough` | output equals input | **8e-6 absolute**, `kPowBound` | derived from GLSL 4.10 §8.2, NOT from an observation: `pow` is inherited from `exp2`, `log2` and a multiply, 3 ULP each; for light as low as 1/256 `log2` returns −8 where 3 ULP is 2.9e-6 absolute, which through `exp2` is ~2.0e-6 relative. Bound ≈ 2.4e-6; the tolerance is a little over three times it. Measured worst: **5.96e-8** |
| 16 | `--passthrough` | alpha is unchanged | **zero, bitwise** | alpha is a straight copy of a float32 texel through a float32 render target. No arithmetic touches it |
| 17 | `--passthrough` | the null settings are exactly their null values | exact | `sensitivity == 1.0f`, `gamma == 1.0f`, `dark == 0.0f`, `leak == 0.0f`, `gain == 1.0f`. Four ranges in `Controls.cpp` are chosen so a slider position lands on these **exactly in binary** — that is why the gamma range is 0.5..1.5 rather than the more natural 0.45..1.4 |
| 18 | `sweep.py` | every control changes the picture somewhere | digest inequality | not a numeric tolerance. A driver difference cannot make a control dead |
| 19 | `--bench` | — | none | not pass/fail. There is no threshold worth asserting on somebody else's GPU |

### What the audit actually found

**One real defect, in `--burn`.** Its tolerance was `4 × n × FLT_EPSILON`, which
at n = 1 is 4.8e-7. The check feeds the model a **white** field and predicts an
output of exactly 1, and `pow( 1.0, 1.0 )` is allowed by the specification to
return 1 ± 7e-7. So the check was tighter than the spec at its first sample —
it passed here only because this GPU happens to return exactly 1.0 — and would
have failed on a conforming driver that does not. `kPowBound` is now a named
constant used by both `--burn` and `--passthrough`, with the derivation in one
place.

**One unit inconsistency, caught by the check itself.** When `VideoGain` was
added, `--comet`'s threshold moved into output units while the margins beside
it stayed in charge units, and the margin came out at **−0.443** — a failure,
correctly, of exactly the "is this measurement on a knife edge?" guard that
exists for the purpose.

**Three tolerances that could legitimately be zero** and are not: rows 1, 3 and
16 above. They are left permissive because tightening them buys nothing and a
bitwise claim is a promise to every future driver.

**Two raster assumptions made explicit rather than removed.** `--capacity`
needs a raster at least twelve pixels wide for its three patches; `--lag` needs
four pixels each way for four distinct probes. Both are now checked and both
fail loudly rather than quietly measuring something else.

**Nothing in this repo sums over pixels**, which is the usual way a check ends
up depending on the raster without saying so.

**And one the audit missed, found by the first CI run (2026-09-23).** Row 10
claimed the 10× and 100× patches bitwise equal, and argued every downstream
operation exact "with or without FMA contraction". The argument treated
`mix( src, sig, 1 )` as `src*0 + sig*1`. GitHub's macOS runner has no GPU, the
harness falls back to Apple's software renderer there, and it evaluates `mix` as
`src + (sig − src)*1` — so the 100× patch, whose source is 1.0, read
0.115470052 against the capacity's 0.115470044. The step was
`continue-on-error`, so the run was green with `--capacity` failed and the two
checks after it never run. The model was right; the check assumed an
implementation of `mix` the specification does not promise. Tolerance now
derived from that; CI now runs all five checks as a gate.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4, `Apple M4 Max`
renderer, GL `4.1 Metal - 90.5`):**

- **The discharge is the recursion, and it is LINEAR.** With capacity 1.828 and
  beam 0.346 — 5.277 fields to discharge — a saturated highlight switched off
  reads, in output units:

  | field | predicted | 64×36 | 320×180 |
  | --- | --- | --- | --- |
  | 1–4 | 1.0000000 | 1.0000000 | 1.0000000 |
  | 5 | 0.2769813 | 0.2769816 | 0.2769816 |
  | 6 and after | 0.0000000 | 0.0000000 | 0.0000000 |

  Four fields at full amplitude, one partial field, then **exactly** zero. An
  exponential does neither: it has no flat part and it never reaches zero. That
  table is the difference between modelling the store and fitting a decay.
- **Two Fields doubles it**, to 12 fields against 6, and adjacent rows follow
  the alternating recursion to 2.6e-7 against a 2.8e-5 tolerance, at both
  rasters.
- **The comet tail is the discharge, not a smear.** A 64-pixel highlight moving
  at 4 px/field leaves a tail of **16 pixels** against 16 predicted, at
  320×64 and at 1280×128, with the pixel beyond it at 0.277 against a 0.5
  threshold.
- **The target saturates.** Patches at ten times and a hundred times capacity
  both read **0.115470044** — the same bits — at both rasters, and that is
  `capacity × gain` exactly. A patch at a quarter of capacity reads
  0.0288675167, so the check is not passing on a constant.
- **Burn follows both poles.** Over 400 fields of rise (τ = 200 fields) the
  sensitivity deficit tracks the closed form to **2e-7**, at six sample points;
  after 300 dark fields of recovery (τ = 500) it is at 0.5253755 against
  0.5253753 predicted. Both rasters.
- **A beam above capacity is the identity.** Worst error **5.96e-8** over
  96×64 and 480×270 of a gradient, against an 8e-6 tolerance derived from the
  GLSL specification. Alpha passes through bitwise.
- **No dead controls.** All **19** swept parameters measurably change the
  picture, each with the context that makes it mean anything
  (`tools/sweep.py`). Eight of them need `Type=0`, because a tube type pins
  them — that is the design, and the sweep is where it would otherwise look
  like eight defects.
- **Every shader compiles**, all six, through `glslc` — including the four the
  plugin assembles at run time and which exist in no file on disk.
- **It loads in a real FFGL host.** `oxbow selftest` instantiates it and renders
  **120 frames, gl error 0x0, PASS**, reporting `SW Plumbicon` / `PB01` /
  `effect`, 23 parameters in six groups, with the About line populated.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`, the plist names a binary that is
  really there with the right identifier, and the ad-hoc codesign the release
  job runs succeeds.
- **The render cost**, by `pbtest --bench` (60 frames each, 20-frame warm-up,
  `glFinish` both sides, default controls):

  | | ms/frame | % of a 60 fps frame | state memory |
  | --- | --- | --- | --- |
  | 1280×720 | 0.60 – 0.64 | 3.8% | 28 MB |
  | 1920×1080 | 1.20 – 1.30 | 7.8% | 63 MB |
  | 3840×2160 | 4.50 – 4.63 | 27.6% | 253 MB |

  Close to linear in pixels, which is what one picture-sized pass plus five
  small ones should be. **The memory is the headline, not the time.** A quarter
  of a gigabyte of RGBA32F at 4K is a real cost and Two Fields doubles it; an
  operator stacking this on four layers at 4K will notice. 32F is not caution —
  see `Plumbicon.h` for why 16F cannot hold the burn accumulator.

**Assumed, or not yet done:**

- **Never loaded into Resolume, on any platform.** Everything here was
  compiled, rendered and measured offline against the real plugin class in a
  headless CGL context, plus one load through `oxbow`. How the parameters
  *present* — whether six groups and nineteen controls read sensibly in the
  inspector, whether an operator finds `Type` before they find `Sensitivity` —
  is untested. Nothing has driven the host.
- **Never built for Windows.** The CMake has the path and `vcpkg.json` has
  GLEW, and neither has been exercised.
- **The tube-type values are judged, not measured.** Nothing in `Tubes.h` came
  off a data sheet. The ordering and rough spacing are what the literature
  agrees on — a plumbicon lags and burns far less than a vidicon, a saticon
  sits between them, a vidicon's transfer gamma is near 0.65 against a
  plumbicon's near 1.0 — expressed in this plugin's own control range. The
  numbers are arguable and `Tubes.h` says so at the table.
- **The burn time constants are plausible rather than sourced.** The rise pole
  tops out at 5e-3 per field, a time constant of 200 fields or about three
  seconds at 60 — which is fast for burn and is there so the effect can be
  *shown* in a take rather than because a tube does that. A real tube etches
  over minutes to hours. The control's range is the compromise, and it is the
  first thing to argue with.
- **The halation is a look, not a scattering model.** Two Gaussians at a ratio
  of 2.2, chosen so the difference makes a usable ring. Real faceplate
  scattering is a point-spread function with a long tail, not a Gaussian, and
  nothing here was fitted to one.
- **Field Mode models the scan cadence and not an interlaced signal.** The
  output is progressive. Line repeat, twitter, dot crawl and everything else
  about interlace *on a display* is old-cathode's, deliberately.
- **The plugin advances one field per `ProcessOpenGL` call.** A host that
  renders the same composition frame twice — two outputs, a preview plus a
  record — advances the target twice, and nothing in FFGL lets a plugin with
  memory know that happened. afterglow has the same property and says so too.
  The look also depends on the composition's frame rate, because a field is a
  rendered frame. A real camera does exactly that; it is still worth knowing.
- **`SetTime` is accepted and inert**, and the harness drives it anyway,
  because a host does and the plugin should be exercised the way a host does.
- **No OpenFX port and no browser demo**, neither required for 0.1.0. A demo
  would be a good fit here — the whole model is one shader — but the state
  buffers are RGBA32F and WebGL2 needs `EXT_color_buffer_float` for that, which
  is worth checking before promising it.
- **No factory presets** beyond `Type`. The fleet's copy-based preset mechanism
  carries a host-echo trap (reported against vertigo as its issue #2) that
  deserves its own pass rather than being copied in at the end of a build; the
  override pattern here has none of that surface, at the cost of eight controls
  being inert while a type is selected.
- **`ATTRIBUTIONS.md` and `source/StoatworksAbout.h` are provisional hand
  copies**, written in the shape the fleet's sync scripts generate. plumbicon
  is not in `sync-about.py`'s TARGETS, in `attributions/names.json` or in the
  website's `projects.json`; registering it and re-running both syncs is the
  fix. `guide` is deliberately `""` because no user guide exists, and the About
  block leaves out a button whose link would 404.
- **CI runs the physics on a software renderer, not a GPU.** The workflow
  builds, lists the parameters, compiles every shader through glslc, and runs
  all five numeric checks on the runner's Apple software renderer as a gate.
  That proves the tolerances hold on a second rasteriser; it proves nothing
  about speed, and `tools/verify.sh` on a machine with a GPU is still the full
  gate (sweep, bench, universal build, oxbow).

---

## Decisions taken without asking

The brief said to decide and move on, so:

**Time is counted in fields, not seconds.** Lag is a field figure in every tube
data sheet, a 50 Hz and a 60 Hz tube with the same third-field lag behave
identically per field, and counting fields is what makes every check in the
harness exact arithmetic with no clock to calibrate. The cost — the look
depends on the project frame rate — is real and is what a camera does.

**Burn is a scalar in the state buffer's alpha, not a per-channel accumulator.**
A three-tube camera burns per channel, so a burnt-in red logo should leave a
cyan shadow; here it leaves a neutral one. The alternative is a second
picture-sized RGBA32F pair, which at 4K is another 265 MB for an artefact that
takes minutes to appear. The scalar is driven by the luma of the incident
light, which is the right *shape* of the mechanism at the wrong *rank*.

**A tube type is an override, not a copy-based preset.** graticule's pattern
rather than afterglow's, because the host owns parameter state and restates it
whenever it likes, and getting that wrong is how a preset dropdown snaps back
to Custom. The visible cost is eight inert controls, which the README states
and the sweep encodes.

**`Target Capacity` is pinned by `Type` and `Sensitivity` and `Beam Current`
are not.** Capacity is what the tube is made of and it is what sets the tail
length; the other two are how the camera is lined up on the day, and a type
that pinned them could not be metered.

**The signal is never a pass of its own.** `min( charge, beam ) * gain` is a
pure function of the state, so it is computed wherever it is wanted from one
shared library function rather than written into a buffer. That is what keeps
Frame mode down to two picture-sized buffers.

**The bright pass box-averages 16 taps by hand.** A single fetch is cheaper and
wrong: a one-pixel specular would be sampled one time in sixteen, so the
halation around it would appear and vanish as the highlight moved. The state
buffer is NEAREST because it is data, so the hardware cannot do the box.

**There is no Gain control in the Output group**, even though the plugin
contains a gain. The spec's control list is Monochrome, Noise, Mix, and the
gain is not a knob — it is the line-up, and exposing it would let an operator
put the chain somewhere no camera is ever set.

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The `PassBuffer`
and the trap list come from **tinsel**; the harness, sweep and verify shape
from **galvo**; the persistent-buffer plumbing and the clock discussion from
**afterglow**; the override-not-a-write preset pattern from **graticule**. The
display half of the chain is **old-cathode**, and it is deliberately not here.
