"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` carries its own copy of the GLSL in `source/Shaders.cpp`,
because a browser cannot include a C++ file. Two copies drift -- quietly,
because a demo that renders a *plausible* picture looks exactly like a demo
that renders the right one. The page's whole claim is that it runs the
plugin's own shaders, so something has to enforce it. `pbtest` drives the real
plugin class and has no idea this page exists, and verify.sh's glslc step
compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation,
no comment stripping. A comment updated on one side and not the other is drift
worth catching: the comments in this repo carry the reasoning.

The one transformation is a decode: a backtick cannot appear raw inside a
JavaScript template literal, so `plugin.js` escapes it as \\`. This unescapes
that and rejects any other backslash on the JS side; there is none in the C++,
so a second escape could only be somebody hiding a difference.

Four of the passes are assembled at run time -- preamble + the target library +
main -- by `TargetShaderSource()` and its siblings. The pieces are checked one
by one, and the ASSEMBLY is checked too, on both sides: pieces that are all
identical and put together in a different order would otherwise pass.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: `Controls.cpp`, the `Tubes.h` table,
`Effective()` and the frame sequence of `ProcessOpenGL` are hand translations in
plugin.js, and only a reader can tell whether they still agree.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ symbol (all in source/Shaders.cpp).
SHADERS = [
    ("VERTEX", "kVertexShader"),
    ("TARGET_LIBRARY", "kTargetLibrarySource"),
    ("TARGET_PREAMBLE", "kTargetPreamble"),
    ("TARGET_MAIN", "kTargetMain"),
    ("HELD_PREAMBLE", "kHeldPreamble"),
    ("HELD_MAIN", "kHeldMain"),
    ("BRIGHT_PREAMBLE", "kBrightPreamble"),
    ("BRIGHT_MAIN", "kBrightMain"),
    ("BLUR", "kBlurShader"),
    ("COMPOSITE_PREAMBLE", "kCompositePreamble"),
    ("COMPOSITE_MAIN", "kCompositeMain"),
]

# C++ assembly function, JS assembled constant, and the pieces in order.
ASSEMBLY = [
    ("TargetShaderSource", "TARGET_SHADER", ["kTargetPreamble", "kTargetLibrarySource", "kTargetMain"]),
    ("HeldShaderSource", "HELD_SHADER", ["kHeldPreamble", "kTargetLibrarySource", "kHeldMain"]),
    ("BrightShaderSource", "BRIGHT_SHADER", ["kBrightPreamble", "kTargetLibrarySource", "kBrightMain"]),
    ("CompositeShaderSource", "COMPOSITE_SHADER", ["kCompositePreamble", "kTargetLibrarySource", "kCompositeMain"]),
]

JS_NAME = {symbol: name for name, symbol in SHADERS}


def from_cpp(source, symbol):
    match = re.search(r'(?:static )?const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol}")
        a_lines, b_lines = cpp_text.splitlines(), js_text.splitlines()
        for i in range(max(len(a_lines), len(b_lines))):
            a = a_lines[i] if i < len(a_lines) else "<missing>"
            b = b_lines[i] if i < len(b_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    for function, constant, pieces in ASSEMBLY:
        cpp_pattern = (r'std::string ' + function + r'\(\)\s*\{\s*return std::string\( ' + pieces[0] + r' \)'
                       + "".join(r'\s*\+\s*' + p for p in pieces[1:]) + r';\s*\}')
        js_line = f"const {constant} = " + " + ".join(JS_NAME[p] for p in pieces) + ";"
        if re.search(cpp_pattern, cpp) is None:
            print(f"FAIL  {function}() no longer assembles {' + '.join(pieces)}")
            problems += 1
        elif js_line not in js:
            print(f"FAIL  demo/plugin.js does not assemble {constant} as {function}() does")
            problems += 1
        else:
            print(f"ok    {constant:<20} assembled as {function}()")

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1
    print(f"all {len(SHADERS)} shader pieces and {len(ASSEMBLY)} assemblies are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
