#pragma once

/**
	The passes, as GLSL source.

	------------------------------------------------------------ the one shader

	`kTargetLibrarySource` is a **fragment, not a shader**: it has no
	`#version` and no `main`. It holds `targetField()`, which is the entire
	plugin -- one field of a camera tube's photoconductive target -- and the
	four passes that need it are each assembled around the same string at run
	time.

	That matters for the same reason it matters in tinsel: a check that
	compiled its own transcription of the model would agree with itself
	perfectly and prove nothing. `tools/pbtest` drives the real plugin class,
	so every number it prints came out of the text below.

	`tools/verify.sh` mirrors the assembly when it compiles the shaders
	through `glslc`, and a constant that has been renamed is a `KeyError`
	there rather than a silent skip.

	---------------------------------------------------------------- the frame

	1. **target**    picture size, RGBA32F, ping-ponged against itself.
	                 `rgb` is the charge the target holds AFTER this field's
	                 light and BEFORE the beam reads it; `a` is the burn.
	                 This is the only pass with any physics in it.
	2. **held**      picture size, RGBA32F, ping-ponged. ONLY in Two Fields
	                 mode: it carries the signal from the last field on which
	                 the beam actually visited each line. In Frame mode it
	                 does not exist and is not run.
	3. **bright**    quarter size. The part of the signal above Bloom
	                 Threshold, box-averaged over the 4x4 full-size texels
	                 each output texel covers -- a point sample would drop a
	                 one-pixel highlight three times out of four and the
	                 halation would flicker.
	4. **blur**      quarter size, four times: two axes narrow, two axes
	                 wide. The narrow one is the halation. The DIFFERENCE
	                 between them is the Image Orthicon's halo ring.
	5. **composite** output size, straight to the host.

	The signal itself is never a pass. It is `min( charge, beam )`, computed
	wherever it is wanted from the same library function, because it is a pure
	function of the state.
*/

#include <string>

namespace plumbicon
{

extern const char* const kVertexShader;
extern const char* const kBlurShader;

/// The four passes that need the target model, assembled around the shared
/// library. Held in a `std::string` because the assembly happens at run time;
/// keep the result alive until `Compile` returns.
std::string TargetShaderSource();
std::string HeldShaderSource();
std::string BrightShaderSource();
std::string CompositeShaderSource();

} // namespace plumbicon
