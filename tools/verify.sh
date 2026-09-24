#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag -- and the fix
# for a bad tag is to re-point it, which strands the release.
#
# The two that have actually bitten this fleet:
#
#   * CFBundleExecutable carrying the PREVIOUS plugin's name, because the
#     plist template was copied from another repo. Nothing fails: the bundle
#     assembles, the binary is universal, nm finds plugMain. Then codesign
#     says "code object is not signed at all" and mentions nothing about a
#     plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was latched before it arrived. The build log calls that a success. Only
#     lipo knows.
#
# What each check answers that none of the others can:
#
#   shaders      does every shader compile, through a real GLSL compiler,
#                before a host has to find out -- including the FOUR the
#                plugin assembles at run time around the shared target model,
#                which no file on disk contains
#   demo         the browser demo's copies of those shaders, and of their
#                assembly, are still the plugin's, character for character
#   lag         a highlight switched off discharges as the exact recursion
#                predicts, at two rasters, and STOPS at the field the
#                capacity-to-beam ratio names
#   comet        the tail behind a highlight moving at v pixels a field is
#                v x (fields to discharge) pixels long, at two rasters
#   capacity     ten times and a hundred times past capacity come out
#                bitwise identical, and equal to the capacity
#   burn         the burn accumulator follows both of its poles over 700
#                fields, at two rasters
#   passthrough  a beam above capacity is the identity, to the tolerance the
#                GLSL spec's accuracy for pow allows and not to one ULP
#   sweep        no control is silently dead. A GLSL uniform whose name does
#                not match the C++ is ignored without a word, so this is the
#                only thing standing between a typo and a shipped slider that
#                does nothing.
#   bench        the render cost, for the record. Not pass/fail -- there is no
#                threshold worth asserting on somebody else's GPU -- but a
#                verify run leaves a timing, which is what turns "it feels
#                slower" into a comparison.
#   binary       universal, exports plugMain, the plist names a binary that is
#                really there with the right identifier, and the ad-hoc
#                codesign the release job runs succeeds
#   oxbow        instantiation and real frames through a real FFGL host, which
#                nothing else here reaches
#
# Every numeric check above runs at TWO rasters and fails if they disagree.
# That is not thoroughness for its own sake: four of six plugins in a previous
# round shipped checks calibrated to this Mac's GPU and failed on a GPU-less
# runner, and in all four cases the test was wrong rather than the plugin.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
UNIVERSAL="${UNIVERSAL:-build-universal}"
failures=()

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures+=("$1"); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [ "source/Shaders.cpp" ]

# Shaders the plugin assembles at RUN TIME, which therefore exist in no file.
# Mirrors TargetShaderSource() and the three beside it in Shaders.cpp -- and a
# name that has moved is a KeyError here, not a silent skip.
ASSEMBLED = {
	"TargetShader":    [ "kTargetPreamble",    "kTargetLibrarySource", "kTargetMain" ],
	"HeldShader":      [ "kHeldPreamble",      "kTargetLibrarySource", "kHeldMain" ],
	"BrightShader":    [ "kBrightPreamble",    "kTargetLibrarySource", "kBrightMain" ],
	"CompositeShader": [ "kCompositePreamble", "kTargetLibrarySource", "kCompositeMain" ],
}

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

# The complete shaders: a #version and a main of their own.
for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( named[ p ] for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	# Six: the vertex shader, the blur, and the four assembled ones. Fewer
	# means the extraction has lost track of where this repo keeps its GLSL,
	# and a check that silently looks at less than it thinks is worse than no
	# check at all.
	if [ "$n" -lt 6 ]; then
		printf '   only %d shaders were extracted, expected 6 -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	[ "$bad" -eq 0 ] && printf '   %d shaders, all compile\n' "$n"
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then pass "every shader compiles"; else fail "a shader does not compile"; fi

#---------------------------------------------------------------------------
# The browser demo's copy of the same GLSL.
#
# `demo/plugin.js` cannot include a C++ file, so it carries its own copy of
# every shader. This compares the two character for character -- reformatting
# counts, deliberately, because "it is only whitespace" is how a real change
# gets waved through. It says nothing about the demo's PORT of the CPU half;
# only a reader can check that.
#---------------------------------------------------------------------------
step "demo: the browser copy of the shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if python3 demo/tools/check_shaders.py >/tmp/plumbicon-demo-shaders.log 2>&1; then
		pass "$( tail -1 /tmp/plumbicon-demo-shaders.log )"
	else
		fail "the demo's shaders have drifted -- see /tmp/plumbicon-demo-shaders.log"
		tail -12 /tmp/plumbicon-demo-shaders.log
	fi
else
	printf '   skipped: no demo/\n'
fi

#---------------------------------------------------------------------------
# A FRESH universal build, and the build directory is deleted first.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list lives. A developer who configured once
# with -DCMAKE_OSX_ARCHITECTURES=arm64 for a fast iteration loop -- which is
# the documented way to work in CLAUDE.md -- leaves a tree where this script
# happily rebuilds, finds a single-architecture binary, and reports it as a
# defect in the source.
#---------------------------------------------------------------------------
step "build (fresh, universal)"
rm -rf "$UNIVERSAL"
if cmake -B "$UNIVERSAL" -DCMAKE_BUILD_TYPE=Release >/tmp/plumbicon-configure.log 2>&1 \
   && cmake --build "$UNIVERSAL" --parallel >/tmp/plumbicon-build.log 2>&1; then
	pass "configured and built universal"
else
	fail "build failed -- see /tmp/plumbicon-build.log"
	tail -25 /tmp/plumbicon-build.log
	printf '\n\033[31mstopping: nothing below can run\033[0m\n'
	exit 1
fi

TEST="$UNIVERSAL/pbtest"

step "the physics"
for check in lag comet capacity burn passthrough; do
	log="/tmp/plumbicon-$check.log"
	if "$TEST" "--$check" >"$log" 2>&1; then
		pass "pbtest --$check"
		# The numbers, not just the verdict: a verify run that only says "ok"
		# cannot be compared with the last one.
		sed -n '1,4p;$p' "$log" | sed 's/^/        /'
	else
		fail "pbtest --$check -- see $log"
		tail -14 "$log"
	fi
done

step "sweep: no control silently dead"
if python3 tools/sweep.py --binary "$TEST" >/tmp/plumbicon-sweep.log 2>&1; then
	pass "$(tail -1 /tmp/plumbicon-sweep.log)"
else
	fail "dead controls -- see /tmp/plumbicon-sweep.log"
	tail -5 /tmp/plumbicon-sweep.log
fi

step "bench: the render cost, for the record"
"$TEST" --bench --frames 60 2>&1 | sed -n '3,7p'

BUNDLE="$UNIVERSAL/Plumbicon.bundle"
BIN="$BUNDLE/Contents/MacOS/Plumbicon"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "binary"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o
	# pipefail`: grep exits at once, nm takes SIGPIPE, and the pipeline reports
	# failure. It is output-size dependent, so it fires on the bigger binary
	# first and looks intermittent. Capture and match with `case` -- not a
	# pipeline anywhere.
	symbols=$( nm -gU "$BIN" 2>/dev/null || true )
	case "$symbols" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle would load and contain no plugins" ;;
	esac

	archs=$( lipo -archs "$BIN" 2>/dev/null )
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present ($archs)" ;; *) fail "NOT universal (got: $archs)" ;; esac

	exe=$( /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign fails after the tag"
	fi

	ident=$( /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ "$ident" = "com.stoatworks.ffgl.plumbicon" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', not com.stoatworks.ffgl.plumbicon"
	fi

	step "codesign (the exact command the release job runs, on a copy)"
	tmp=$( mktemp -d )
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Plumbicon.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs"
	else
		fail "ad-hoc signing failed -- the failure that never mentions the plist"
	fi
	rm -rf "$tmp"

	step "oxbow: a real FFGL host loads it"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$( "$OXBOW" probe "$BUNDLE" 2>&1 )
		case "$probe" in
			*"SW Plumbicon"*) pass "name is SW Plumbicon" ;;
			*) fail "oxbow does not see the name: $( printf '%s' "$probe" | head -3 )" ;;
		esac
		case "$probe" in *"PB01"*) pass "id is PB01" ;; *) fail "id is not PB01" ;; esac
		case "$probe" in *"type:        effect"*) pass "type is effect" ;; *) fail "type is not effect" ;; esac

		# An effect needs an input, and oxbow's selftest feeds it one.
		self=$( "$OXBOW" selftest "$BUNDLE" 2>&1 )
		case "$self" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "instantiates and renders in a host" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if (( ${#failures[@]} == 0 )); then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi
printf '\033[31mFAILURES:\033[0m\n'
printf '  %s\n' "${failures[@]}"
exit 1
