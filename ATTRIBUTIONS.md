# Attributions

Plumbicon is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper over the SDK's FFGLFBO — reallocating only when the size changes, freeing the colour texture the SDK's Release() leaks, and owning its own filtering — along with the FFGL trap list that came with it.

### Offline harness, control sweep and verify script — Stoatworks galvo

<https://github.com/stoatworks-labs/galvo>  
Licence: MIT  
Copyright: Stoatworks Labs

The shape of pbtest (the real plugin class driven in a headless GL context), tools/sweep.py (no control is silently dead) and tools/verify.sh (the release job's checks run before the tag), and the rule that a tolerance is derived rather than fitted.

### Persistent-buffer plumbing and the Diag logger — Stoatworks afterglow

<https://github.com/stoatworks-labs/afterglow>  
Licence: MIT  
Copyright: Stoatworks Labs

The plumbing for state carried across frames in picture-sized buffers, and the log-only diagnostics for a plugin living inside somebody else's process.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Photoconductive camera tubes

The target as a charge store — light charges a photoconductive layer, the beam reads it by discharging it, and can only remove so much per pass — is the standard account in broadcast engineering texts, and so are its consequences: lag, comet tails, blocked highlights and burn-in. Implemented as one function from that description; no simulation, code or data was taken.

### Plumbicon, Saticon, Vidicon and Image Orthicon tubes

The tube types follow the ordering the literature agrees on — a plumbicon's near-unity transfer gamma against a vidicon's ~0.65, and their relative lag and burn — expressed in the plugin's own control range. Nothing came off a data sheet. The Image Orthicon's halo is attributed in the same texts to redistribution of secondary electrons; the plugin does not model that, and draws the ring instead.

## Standards and published specifications

What the implementation is measured against.

- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The integer output permutation used for the noise hash, implemented from the paper. Chosen over fract(sin(x)) because a trigonometric hash depends on the driver's sin.
- **ITU-R BT.709** — The luma coefficients that drive the burn accumulator and Monochrome.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
