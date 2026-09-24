# Plumbicon user guide

Plumbicon is **a camera tube for [Resolume](https://resolume.com) Arena and Avenue**, as an FFGL
effect. It does not add smear and bloom to a clip. It puts a photoconductive target in front of
it — a layer that stores charge — and reads that target the way a tube camera did, with an
electron beam that discharges it and can only take so much charge per pass. Lag, comet tails,
blocked-up highlights and burn-in are all what that one store of charge does.

![A test card through the plugin: a bright disc dragging a comet tail, a blocked-up white plate with a burn shadow around it, and a blown highlight on the ramp](hero.png)

*The repo's test card through the plugin at its defaults, rendered by the offline harness rather
than captured from Resolume. The tail behind the disc, the blue trailing edge on the bar, the
white plate with no detail in it and the faint rectangle etched around it are one store of
charge, seen from four angles.*

> **Before you rely on this:** released at **v0.1.0**, and honestly early. The model is measured
> rather than asserted, by a harness that drives the real plugin class: a saturated highlight
> switched off reads full for four fields, 0.277 on the fifth against 0.2769813 predicted, then
> exactly zero; a 64 px highlight moving 4 px a field leaves a 16 px tail against 16 predicted;
> patches ten and a hundred times past capacity come out bit-for-bit identical; and the burn
> tracks its closed form to 2e-7 over 700 fields. All 19 controls measurably change the picture.
> It has **never been loaded into Resolume on macOS** — the one host it has run in is the fleet's
> own test host, `oxbow`, for 120 frames.
> On Windows, a build of v0.1.0 loads, registers and renders in Resolume Arena 7.27.1, with every control matching what the plugin declares — on software rendering, so that says nothing about a GPU. Two controls that only act on motion and over many fields, Recovery and Burn Rate, could not be shown moving there on a still picture.
> Try it on a spare layer before you put it in a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Every download carries one effect, **SW Plumbicon**. Drop it into Resolume's effects folder and
restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. The effect then appears in the effects
browser as **SW Plumbicon**.

The macOS download is a universal build (Apple silicon and Intel), as a `.dmg` or a `.zip`. It is
**Developer ID-signed and notarised**, so the bundle simply loads. The Windows download is an x64
installer or a `.zip`. It is not code-signed, so the installer trips SmartScreen once: **More
info** → **Run anyway**.

---

## This is the camera, not the screen

Nothing here models a display: no phosphor, no ghosting, no vertical hold, no dot crawl, no
shadow mask, no scanlines. All of that belongs to **SW Old Cathode**, the sibling plugin. Put
Plumbicon on the layer and Old Cathode after it and you have the whole chain — camera, then
monitor. If you want the picture to look like it is *on* an old television, that is the other
plugin. This one is what the picture looked like on its way *into* one.

---

## Start here

Put SW Plumbicon on a layer with something moving and bright in it, and leave every control
alone. The defaults are **Type = Plumbicon, slightly over-exposed**: the brightest part of the
picture blocks up to flat white with no detail in it, anything bright that moves drags a short
tail for a couple of fields, and there is a little halation round the highlights and a little
noise in the shadows.

Then, in this order:

1. **Type.** Try **Vidicon**: the same model with different constants, and it smears for about
   nine fields where a plumbicon smears for two. **Image Orthicon** adds the dark ring round
   every highlight.
2. **Sensitivity.** This is the exposure. Push it up to drive more of the picture past what the
   beam can take off in one pass, and the tails lengthen and more of the highlights block up.
3. **Beam Current.** Turn it down for more lag and earlier blocking; all the way up and the lag
   disappears.

**Be honest about the input.** A real comet tail comes from something a hundred times brighter
than peak white — a lamp in shot, a sun glint, a welding arc. A clip has nothing above white:
it was thrown away before the plugin ever saw it. So the only way to get a tail is to
over-expose the clip's own highlights, and that costs some headroom in the midtones. There is no
setting that gives a comet tail on a correctly exposed picture, because there is nothing in one
for the tail to come from. That is why the defaults are over-exposed.

Two numbers are worth carrying in your head, because between them they are the whole instrument:

- **Target Capacity ÷ Beam Current is the length of the tail**, in fields.
- **Beam Current ÷ Sensitivity is the light level at which the picture starts blocking up.**

They are independent, which is why both controls exist.

---

## Time is counted in fields

A field here is **one rendered frame**. Lag is quoted per field on every tube data sheet, so the
model counts fields and never reads a clock. The consequence is that the look depends on your
composition's frame rate: a 30 fps composition holds a tail for twice as long in seconds as a
60 fps one. A real camera does exactly that.

A host that renders the same composition frame twice — a preview and a record, say — advances
the target twice. Nothing in FFGL tells a plugin with memory that it happened.

---

## The Tube group

**Type** — Custom, Image Orthicon, Plumbicon, Saticon or Vidicon. The four tubes are the same
model with different constants:

| Type | What it is |
| --- | --- |
| **Plumbicon** | The default, and the tube the plugin is named for. Near-linear response, low dark current, a short tail, little burn. |
| **Saticon** | Between the other two, which is why it existed. |
| **Vidicon** | Cheap and sensitive, and famous for smearing everything and keeping a picture of whatever it was left pointing at. A long tail, heavy burn, and a transfer gamma near 0.65, so the picture looks lifted. |
| **Image Orthicon** | An ordinary tube with one signature: the black halo round every highlight. |
| **Custom** | No tube. The sliders below are the truth. |

**While a tube is selected, eight controls are inert**: Target Capacity, Transfer Gamma, Dark
Current, Lag Amount, Burn Rate, Burn Recovery, Burn Depth and Halo. Their sliders still move and
the picture does not change, because the tube is supplying those values. Set **Type = Custom** to
drive them yourself. The tube values are judged from what the literature agrees on — their
ordering and rough spacing — not taken off a data sheet.

**Sensitivity** — how much charge a unit of light puts on the target in one field. The
exposure, and always live, because it is how a camera is lined up on the day rather than what
the tube is made of. A quarter of the way along is exactly unity. The default, a little past
that, is what over-exposes the picture. Push it up for longer tails and more blocking; pull it
down towards a quarter for a picture that barely lags.

**Target Capacity** — the most charge the target can hold. Light past this is simply not stored,
which is why a highlight blocks up and stops carrying detail rather than getting brighter. It is
also, divided by Beam Current, **the length of the tail**, so this is the control for a long
smear. Pinned by Type.

**Beam Current** — how much charge the beam can take off in one field. Always live. The travel
is logarithmic, so most of it is spent in the lower region where lag lives. At the top the beam
can empty even a full target in one pass and there is no lag at all. Turning it down gives more
lag *and* blocks the highlights earlier — the picture is lined up so that the beam's maximum
reads as white, so a low beam clips the top and lifts everything under it, which is what an
over-driven tube looks like.

**Transfer Gamma** — the tube's response curve, 0.5 to 1.5, exactly 1.0 in the middle of the
slider. A plumbicon is near 1.0; a vidicon is about 0.65. **Below 1.0 the picture looks lifted
and milky**, and that is faithful: a real chain handed this signal to a display that
re-applied about 2.2. This plugin is the camera and does not correct for a display it does not
model — Old Cathode after it is where that happens. Pinned by Type.

**Dark Current** — charge that arrives whether or not there is any light: the target's own
leakage. It lifts the blacks, and because it is charge like any other, it gives the dark parts
of the picture a little lag of their own. The bottom of the slider is exactly none. Pinned by
Type.

---

## The Lag group

**Lag Amount** — multiplies the beam current down, from exactly ×1 at zero to ×1/9 at the top.
Beam Current is what the tube is; Lag Amount is how much of it you want today. The range is
deliberately modest because the beam is also the ceiling on the signal — for a *longer* tail,
reach for Target Capacity on a Custom tube. Pinned by Type.

**Recovery** — how much of the leftover charge leaks away on its own between scans, each field.
A real target is not a perfect capacitor and recovers from a smear a little faster than the beam
alone would manage. At zero the tail is exactly the beam's work; turning it up shortens and
softens the end of every tail. Always live.

**Field Mode** — **Frame** or **Two Fields**.

- **Frame** — the beam visits every line every field.
- **Two Fields** — the beam visits alternate lines on alternate fields, so each line is read half
  as often and **the tail doubles**. It also **twitters on motion, and only on motion**: a moving
  edge is caught at two positions on alternate lines, because those lines were last read a field
  apart, while a static area shows no line structure at all.

Two Fields models the scan cadence, not an interlaced signal: the output is still progressive.
It also **doubles the video memory** the effect uses (see Performance).

---

## The Burn group

A second, far slower store sits under the first. Where the target has been bright, it biases the
local sensitivity down, so that area reads darker afterwards. Leave the effect on a static logo
and the logo's shadow is still there after it has gone.

**Burn Rate** — how fast the target etches. At zero it never burns; at the top it takes a time
constant of about 200 fields — roughly three seconds at 60 fps. That is far faster than a real
tube, which etches over minutes to hours, and it is there so the effect can be shown in a take.
Pinned by Type.

**Burn Recovery** — how fast an etched area recovers once the light has gone. At the same slider
position it is two and a half times slower than the rate, because a tube etches faster than it
recovers. At zero a burn never clears. Pinned by Type.

**Burn Depth** — how much sensitivity a fully burnt area loses, from none to all of it. Pinned
by Type.

The burn is driven by the brightness of the light, not its colour, so a burnt-in red logo
leaves a neutral shadow rather than a cyan one. A three-tube camera would leave a cyan one; see
Known limits.

---

## The Optics group

Light scattered in the faceplate of the camera, before it reaches the target. (Old Cathode has a
halation too, and it is the other one — the display's glass, at the far end of the chain. Run
both and a bright caption gets both, which is what actually happened.)

**Halation** — how much scattered light is added back round the highlights. Up to 1.5.

**Halation Radius** — how far it spreads. It is measured in pixels of a quarter-size copy of the
picture, so the same setting looks tighter on a 4K composition than on a 720p one. It also sets
the size of the Halo ring.

**Bloom Threshold** — how bright a part of the picture has to be before it scatters. Lower it and
more of the picture glows. It feeds the Halo too.

**Halo** — the Image Orthicon's dark ring round every highlight. **This is the one thing in the
plugin that is drawn rather than derived.** A real orthicon's halo is secondary electrons
knocked off the target landing back round a highlight — a different mechanism, which nothing in
a charge store can produce — so it is made here as a ring subtracted from the picture. Pinned by
Type: zero on every tube except Image Orthicon.

With Halation and Halo both at zero the optics are skipped altogether, which saves five passes.

![The same card as an Image Orthicon: a heavy comet tail with a dark ring around every highlight](orthicon.png)

*Type = Image Orthicon. The dark ring round the disc, the plate and the bar is the Halo.*

---

## The Output group

**Monochrome** — fades the picture to its luma, from full colour to black and white. The charge
store works per channel, which is why a smear can change colour — a bluish highlight lags in blue
and not in red. Monochrome takes that away, along with the colour.

**Noise** — beam and preamp noise. It is added at the same level everywhere, which is why it
shows in the shadows: there is less picture under it there. It changes every field.

**Mix** — the processed picture against the untouched clip. Zero is the clip as it arrived. The
target keeps charging and burning underneath whatever Mix says, so bringing Mix back up shows
what the tube has been storing all along, burn included. Alpha always passes straight through
from the clip.

There is no gain control. The plugin does contain a gain — it is how the picture is lined up so
the beam's maximum reads as white — but that is the camera's line-up, not a knob.

---

## How it works

Once a field, for every pixel and every colour channel:

1. **What the beam left behind last field is still there.** It took what it could — up to Beam
   Current — and the rest stays on the target. Recovery lets a little of it leak away.
2. **Light adds charge**, through the Transfer Gamma, scaled by Sensitivity and reduced wherever
   the target has burnt, plus the Dark Current.
3. **The target saturates.** Charge past Target Capacity is not stored.
4. **The beam reads it**, taking at most Beam Current. What it took is the picture.
5. **The burn store moves** a small step towards the brightness of the light — faster going up
   than coming down.

Residue read out again is **lag**; residue left along a moving highlight's path is **a comet
tail**, and nothing draws it; the clamp in step 3 is **a blocked highlight**. The tail is linear
rather than exponential — full for a few fields, one partial field, then exactly nothing.

The halation and the halo are added afterwards from a quarter-size copy of the picture, then
Monochrome, Noise and Mix.

---

## Performance

Measured by the offline harness on an M4 Max, at the default controls:

| | ms/frame | Target memory |
| --- | --- | --- |
| 1280×720 | 0.60 – 0.64 | 28 MB |
| 1920×1080 | 1.20 – 1.30 | 63 MB |
| 3840×2160 | 4.50 – 4.63 | 253 MB |

**The memory is the headline, not the time.** The target is two picture-sized 32-bit float
buffers — the burn store moves in steps too small for anything less — and **Two Fields doubles
it**. Stack this on four 4K layers and you will notice. Nothing was timed inside Resolume, and
nothing was timed on Windows.

---

## If it looks wrong

**Nothing seems to happen.** Check Mix. Then check the clip has something bright in it: on a
dim, correctly exposed picture there is nothing for the target to lag on. Raise Sensitivity.

**A slider does nothing.** A tube Type is selected, and that slider is one of the eight it pins.
Set Type to Custom.

**The picture looks grey and lifted.** Transfer Gamma is below 1.0 — on Vidicon or Saticon, it
is. That is the camera's own signal; put Old Cathode after it for a display's response.

**Dark patches linger where something bright was.** That is burn. Lower Burn Depth or raise
Burn Recovery on a Custom tube, or change the Type.

**The effect does nothing at all.** A shader that will not compile looks exactly like that, and
the real message is in the log:

```
macOS    ~/Library/Logs/plumbicon/plumbicon.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\plumbicon\logs\plumbicon.YYYY-MM-DD.log
```

It records the GL vendor and version at load, and which shader failed if one did.

---

## Known limits

- **Never loaded into Resolume on macOS**, and nothing has driven the controls in a host. How
  the six groups read in the inspector is untested.
- **The tube constants are judged, not measured.** Nothing came off a data sheet.
- **Burn is far faster than a real tube's**, so it can be seen in a take, and it is a single
  brightness-driven store rather than one per colour.
- **The halation is two Gaussians**, chosen as a look, not a measured scattering profile. The
  Halo is drawn, not modelled.
- **Field Mode is the scan cadence, not interlace.** The output is progressive.
- **No presets** beyond Type, and no OpenFX version.
- **There is a browser demo** at [plumbicon-demo.stoatworks-labs.com](https://plumbicon-demo.stoatworks-labs.com).
  It is a port to a web page, not the plugin: the shaders run in WebGL2 and any CPU
  half is rewritten in JavaScript. The page lists what it does not reproduce.

---

## About

The last group, **About**, carries the plugin's name, version, licence and maker, and buttons
that open this user guide, the project page, the source on GitHub and the support page in
your browser.

## Reporting something

[github.com/stoatworks-labs/plumbicon/issues](https://github.com/stoatworks-labs/plumbicon/issues).
A screenshot, the Type and the composition's resolution and frame rate is usually enough.
