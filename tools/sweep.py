#!/usr/bin/env python3
"""No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is
indistinguishable from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

------------------------------------------------------------------ the traps

**Seven controls are PINNED by Type, and that is the design rather than a
defect.** A tube type is an override, not a write (see `source/Tubes.h`), so
while one is selected the operator's Transfer Gamma slider moves and the
picture does not. Every one of those seven therefore carries `Type=0` --
Custom -- in its context. Without it this file would report seven dead
controls, correctly, and bury any real failure among them.

**Burn is measured in HUNDREDS of fields.** Its whole point is that it is
slower than everything else in the plugin, so a sweep that renders thirty
fields cannot see it at any setting. The burn controls ask for ninety fields
and for `Burn Depth` to be somewhere it can act.

**Lag needs a beam that cannot keep up.** With the beam above the light the
target is emptied every field and there is no residue for Recovery or Field
Mode to do anything to -- which is correct behaviour and reads as a dead
control. Both are swept with the beam turned down.

**Never sweep the About block.** Those are buttons that open a web browser,
and sweeping them opens one tab per press.

    python3 tools/sweep.py [--binary build/pbtest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.
"""

import argparse
import concurrent.futures
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all.
#
# An entry beginning with '#' is a RENDER setting rather than a plugin
# parameter -- '#frames=90'. The prefix is not decoration: this plugin has a
# control genuinely called "Noise", and a bare 'Noise=' key would be ambiguous
# between the two the day somebody adds a source-noise context.
CONTEXT = {
    # Pinned by Type. Custom is the only position at which the slider is the
    # truth. See the docstring.
    "Transfer Gamma": ["Type=0"],
    "Dark Current": ["Type=0"],
    "Lag Amount": ["Type=0"],
    "Halo": ["Type=0"],

    # Burn is slow by construction, and invisible unless it is allowed to bias
    # anything. The card's static over-bright plate is what it acts on.
    "Burn Rate": ["Type=0", "Burn Depth=1", "#frames=90"],
    "Burn Depth": ["Type=0", "Burn Rate=1", "#frames=90"],
    # Recovery needs burn that has already risen AND somewhere it is now dark,
    # which is what the card's travelling disc and bar provide.
    "Burn Recovery": ["Type=0", "Burn Rate=1", "Burn Depth=1", "#frames=90"],

    # Leakage and the scan cadence only exist where the beam left something
    # behind. With a beam above the light there is no residue at all.
    "Recovery": ["Type=0", "Beam Current=0.45", "#frames=40"],
    "Field Mode": ["Type=0", "Beam Current=0.45", "#frames=40"],

    # The optics need something above the threshold to scatter.
    "Halation Radius": ["Halation=1"],
    "Bloom Threshold": ["Halation=1"],
}

# Positions to try, as a fraction of the parameter's declared range. Three
# rather than two: a control that is a no-op at both ends but not in the
# middle is rare, but it costs one render to stop worrying about it.
FRACTIONS = [0.0, 0.5, 1.0]

# Parameters with no scalar value worth sweeping.
SKIP = {
    "About": "a display-only text line",
    "User guide": "a button that opens a web browser",
    "Project page": "a button that opens a web browser",
    "Source on GitHub": "a button that opens a web browser",
    "Support the work": "a button that opens a web browser",
}


def parse_list(binary):
    """Every parameter as (name, type, default, min, max)."""
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError("could not list parameters: " + listing.stderr.strip())

    rows = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 5:
            continue
        # The name may contain spaces, so take the fixed columns off the end.
        low, high = float(parts[-2]), float(parts[-1])
        default, kind = float(parts[-3]), parts[-4]
        name = " ".join(parts[1:-4])
        rows.append((name, kind, default, low, high))
    return rows


def render(binary, out, settings, size, frames):
    command = [binary, "--out", str(out), "--size", size, "--frames", str(frames)]
    for setting in settings:
        command += ["--set", setting]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("render failed: " + result.stderr.strip())
    return hashlib.sha256(out.read_bytes()).hexdigest()


def sweep_one(binary, size, default_frames, row, index):
    name, kind, _default, low, high = row

    context, frames = [], default_frames
    for entry in CONTEXT.get(name, []):
        if entry.startswith("#frames="):
            frames = int(entry.split("=", 1)[1])
        elif entry.startswith("#"):
            raise RuntimeError(f"unknown render setting {entry!r} for {name}")
        else:
            context.append(entry)

    # An option parameter is set in its own units; a standard one is 0..1 and
    # its range says so anyway.
    values = [low + (high - low) * f for f in FRACTIONS]
    if kind in ("integer", "option"):
        values = sorted({round(v) for v in values})

    digests = set()
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / f"sweep{index}.png"
        for value in values:
            digests.add(render(binary, out, context + [f"{name}={value}"], size, frames))
    return name, len(digests) > 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/pbtest")
    parser.add_argument("--size", default="320x180")
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DPLUMBICON_BUILD_TOOLS=ON first")
        return 2

    rows = [r for r in parse_list(str(binary)) if r[0] not in SKIP and r[1] != "text"]
    if not rows:
        print("no parameters found")
        return 2

    dead, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = {
            pool.submit(sweep_one, str(binary), arguments.size, arguments.frames, row, i): row[0]
            for i, row in enumerate(rows)
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                name, alive = future.result()
            except RuntimeError as error:
                print(f"  {'ERR':4}  {name}: {error}")
                failed.append(name)
                continue
            print(f"  {'ok' if alive else 'DEAD':4}  {name}")
            if not alive:
                dead.append(name)

    print()
    if failed:
        print(f"{len(failed)} parameter(s) could not be rendered: {', '.join(failed)}")
        return 1
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(sorted(dead))}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {len(rows)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
