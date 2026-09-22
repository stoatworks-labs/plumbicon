# Attributions

Plumbicon is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Plumbicon is not in
> that script's lists at all yet, so this copy is hand-written in the shape the
> script produces. v0.1.0 ships that way; registering the project and re-running
> the sync is the fix — and note that the script's `--only` flag truncates the
> file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at `external/ffgl/deps/glew-2.1.0`. Not
fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at
OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>
Licence: PNG Reference Library License (libpng)
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly
— listed because it is present in the checkout.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked from the system, by the offline harness only, to deflate the PNGs it
writes. Nothing in the shipped plugin uses it.

## Methods, not code

Nothing below was copied. Each is a published idea implemented here from a
description of what it does.

- **The photoconductive target as a charge store** — the standard account of
  how a vidicon-family camera tube works, found in any broadcast engineering
  text: light generates carriers in a photoconductive layer, the layer holds
  the resulting charge pattern between scans, and the electron beam restores
  the scanned side to cathode potential, the restoring current being the
  signal. The consequences — lag, comet tails, blocked highlights and burn —
  are the same texts' account of why those happen. The model here is one
  function, written from that description; no simulation, code or data was
  taken from anywhere.
- **The Image Orthicon halo** is attributed in those same texts to
  redistribution of secondary electrons around a highlight. This plugin does
  **not** model that; it draws a ring, and says so everywhere it appears. See
  AGENTS.md.
- **The tube-type figures** — a plumbicon's near-unity transfer gamma against a
  vidicon's ~0.65, and the ordering of their lag and burn — are the ordering
  the literature agrees on, expressed in this plugin's own control range.
  Nothing here came off a data sheet, and `source/Tubes.h` says so.
- **PCG** (Melissa O'Neill, 2014) — the integer output mix used for the noise
  hash, implemented from the published permutation. Chosen over
  `fract( sin( x ) )` because a trigonometric hash depends on the driver's
  `sin` and two GPUs then disagree about the picture.

The `PassBuffer` wrapper, the `Diag` logger, the offline-harness shape, the
sweep and the verify script all come from this fleet's **tinsel**, **galvo**
and **afterglow**, and are the same code lineage rather than third-party
components.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong,
or you would rather not be listed — open an issue and it will be fixed.
