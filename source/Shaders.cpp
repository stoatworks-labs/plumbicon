#include "Shaders.h"

namespace plumbicon
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The host's MaxUV is applied
	//where the host's texture is read and nowhere else; every buffer in this
	//plugin is one we allocated, where the picture really does fill the
	//texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// THE MODEL. Shared verbatim by the target, held, bright and composite
// passes; no #version and no main, so it is assembled into each of them.
//---------------------------------------------------------------------------
static const char* const kTargetLibrarySource = R"(
//Rec.709 luma. Used for the burn drive and for Monochrome, and for nothing
//else -- the charge model is per channel throughout, because a three-tube
//camera has three targets and they do not share a residue.
const vec3 kLuma = vec3( 0.2126, 0.7152, 0.0722 );

//Does the beam visit this line on this field?
//
//`parity` is -1 in Frame mode, which is every line, and 0 or 1 in Two Fields
//mode. INTEGER arithmetic on the line index, never a float `mod`: GLSL
//defines mod as x - y * floor( x / y ), and where the division rounds a hair
//below an integer the result comes back as the divisor rather than as zero.
//That cost tinsel sixty wrong lamps out of half a million and it does not
//reproduce on the CPU.
bool lineIsRead( int parity, float fragY )
{
	if( parity < 0 )
		return true;

	//gl_FragCoord.y is at a pixel centre, n + 0.5, so the truncation is exact.
	return ( int( fragY ) & 1 ) == parity;
}

float beamOnLine( float beam, int parity, float fragY )
{
	return lineIsRead( parity, fragY ) ? beam : 0.0;
}

//=========================================================================
// ONE FIELD OF A CAMERA TUBE'S TARGET.
//
// This function is the whole plugin. Light charges a photoconductive layer;
// the scanning beam reads it by DISCHARGING it; the beam can only remove so
// much charge per pass; whatever it could not remove is still sitting there
// on the next field.
//
// Nothing below draws a comet tail, blocks a highlight, etches the target or
// smears a moving object. Every one of those is a consequence of the four
// lines under "the beam" and the one clamp under "photoconversion":
//
//   lag        -- the beam takes `beam` and leaves the rest, so a highlight
//                 that charged harder than that is read out again next field
//   comet tail -- the same thing at a point the highlight has since left
//   blocked    -- `min( ..., capacity )`: past that, more light is not more
//   highlights    charge, so the detail in a highlight is simply not stored
//   burn-in    -- a second, far slower accumulator biasing the sensitivity
//
// If you find yourself adding a term to make one of those appear, stop: the
// mechanism has been lost somewhere above.
//
// `prevCharge` is what the target held at the END of the previous field,
// before that field's beam pass. `prevBeam` is the beam current that pass
// actually applied to THIS line, which is zero in Two Fields mode on the
// lines it did not visit.
//=========================================================================
struct Target
{
	vec3 charge;///< held after this field's light, before this field's beam
	float burn; ///< the slow accumulator, 0..1
};

Target targetField( vec3 prevCharge, float prevBurn, float prevBeam, vec3 light,
                    float photoGain, float gamma, float dark, float capacity,
                    float leak, float burnRise, float burnFall, float burnDepth )
{
	Target t;

	//--- the previous field's beam pass ---------------------------------
	//It took what it could reach and left the rest. THE RESIDUE IS THE LAG.
	vec3 residue = prevCharge - min( prevCharge, vec3( prevBeam ) );

	//--- the target's own leakage between scans -------------------------
	//A real target is not a perfect capacitor. Exactly zero at the control's
	//null, which is what makes the lag recursion the pure one.
	residue -= residue * leak;

	//--- photoconversion ------------------------------------------------
	vec3 lit   = pow( max( light, vec3( 0.0 ) ), vec3( gamma ) );
	float sens = photoGain * ( 1.0 - burnDepth * prevBurn );
	vec3 photo = sens * lit + vec3( dark );

	//The target cannot hold more than it can hold. This ONE CLAMP is why a
	//highlight blocks up: past capacity, more light is not more charge, so
	//the detail inside a highlight is never stored and cannot be read back.
	t.charge = min( residue + photo, vec3( capacity ) );

	//--- burn -----------------------------------------------------------
	//Asymmetric on purpose: a tube etches faster than it recovers, which is
	//the entire reason burn-in is something anybody has ever had to live
	//with. Minutes, not frames -- see Controls.h for the poles.
	float drive = dot( lit, kLuma );
	float pole  = drive > prevBurn ? burnRise : burnFall;
	t.burn      = prevBurn + ( drive - prevBurn ) * pole;

	return t;
}

//What the beam takes off the target: the signal. A pure function of the
//state, which is why it is never a pass of its own.
vec3 signalOf( vec3 charge, float beam )
{
	return min( charge, vec3( beam ) );
}

//The signal at a point, through the video gain.
//
//In Frame mode the beam visits every line every field, so the signal is just
//`min( charge, beam )` and `heldTexture` is bound to the state texture and
//never used. In Two Fields mode the beam visits a line every OTHER field, and
//what the picture shows between visits is the signal the beam took last time
//it was there -- which is why that mode, and only that mode, pays for a
//second pair of buffers.
//
//`gain` is the rest of the chain, lined up so that peak signal is peak white.
//It is not a control and not a fudge: without it Beam Current does two jobs at
//once, deciding how much lag there is AND how bright the picture is, and
//turning the lag up hands back a dim grey picture instead of a smeared one. A
//camera is set up by putting the beam where it just handles peak white and
//then setting the amplifier so that reads as white. See Controls.h.
//
//The held buffer stores the signal UNGAINED, so that moving Sensitivity does
//not reach back and re-level the field the beam read two fields ago.
vec3 signalAt( sampler2D stateTexture, sampler2D heldTexture, vec2 at,
               float beam, int useHeld, float gain )
{
	vec3 raw = useHeld != 0 ? texture( heldTexture, at ).rgb
	                        : signalOf( texture( stateTexture, at ).rgb, beam );
	return raw * gain;
}

//PCG output mix. Integer, never fract( sin( x ) ): a trigonometric hash
//depends on the driver's sin, so two GPUs disagree about which pixels got
//which noise and no CPU mirror can agree with either.
uint hashInt( uint seed )
{
	uint state = seed * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hash01( uint seed )
{
	return float( hashInt( seed ) ) * 2.3283064365386963e-10;
}
)";

//---------------------------------------------------------------------------
// Pass 1: target.
//---------------------------------------------------------------------------
static const char* const kTargetPreamble = R"(#version 410 core

uniform sampler2D InputTexture;///< the host's picture
uniform sampler2D StateTexture;///< our own previous state, rgb charge, a burn
uniform vec2 MaxUV;            ///< the part of the host texture that is picture
uniform vec2 HalfTexel;        ///< half an input texel, in picture space

uniform float PhotoGain;
uniform float Gamma;
uniform float DarkCurrent;
uniform float Capacity;
uniform float Beam;      ///< beam current x lag scale, charge per field
uniform float Leak;
uniform float BurnRise;
uniform float BurnFall;
uniform float BurnDepth;

/// Parity of the field BEFORE this one -- the beam pass this shader is
/// undoing. Not this field's parity: getting that wrong puts the discharge on
/// the wrong lines and makes Two Fields look like a broken comb filter.
uniform int PrevParity;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kTargetMain = R"(
void main()
{
	vec4 state = texture( StateTexture, uv );

	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//part of its weight from the texture's undrawn padding, and this plugin
	//integrates -- a dark fringe would charge the edge of the target less on
	//every field and etch itself in.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	vec3 light   = texture( InputTexture, picture * MaxUV ).rgb;

	Target t = targetField( state.rgb, state.a,
	                        beamOnLine( Beam, PrevParity, gl_FragCoord.y ),
	                        light,
	                        PhotoGain, Gamma, DarkCurrent, Capacity,
	                        Leak, BurnRise, BurnFall, BurnDepth );

	fragColor = vec4( t.charge, t.burn );
}
)";

//---------------------------------------------------------------------------
// Pass 2: held. Two Fields mode only.
//---------------------------------------------------------------------------
static const char* const kHeldPreamble = R"(#version 410 core

uniform sampler2D StateTexture;///< the state this field just wrote
uniform sampler2D HeldTexture; ///< what the picture has been showing
uniform float Beam;
uniform int FieldParity;       ///< THIS field's parity

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kHeldMain = R"(
void main()
{
	vec3 charge = texture( StateTexture, uv ).rgb;
	vec3 previous = texture( HeldTexture, uv ).rgb;

	//A line the beam did not visit keeps showing what it gave up last time.
	//Showing `min( charge, beam )` there instead is the obvious thing and it
	//is wrong: the charge on an unvisited line has had two fields of light
	//rather than one, so every other line would come out twice as bright and
	//the whole picture would be a 2:1 line pair. That is not an artefact
	//falling out of the model, it is the model being read at a moment it was
	//not read at.
	fragColor = vec4( lineIsRead( FieldParity, gl_FragCoord.y )
	                      ? signalOf( charge, Beam )
	                      : previous,
	                  1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: bright.
//---------------------------------------------------------------------------
static const char* const kBrightPreamble = R"(#version 410 core

uniform sampler2D StateTexture;
uniform sampler2D HeldTexture;
uniform float Beam;
uniform float Gain;
uniform int UseHeld;
uniform float Threshold;
uniform vec2 SourceTexel;///< one FULL-SIZE texel, in uv

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kBrightMain = R"(
void main()
{
	//The 4x4 full-size texels this quarter-size texel covers, box averaged.
	//
	//A single fetch is cheaper and wrong: a one-pixel specular would be
	//sampled one time in sixteen, so the halation around it would appear and
	//vanish as the highlight moved, at no particular rate. The state buffer
	//is NEAREST-filtered on purpose -- it is data, not a picture -- so the
	//box has to be written out rather than left to the hardware.
	vec3 total = vec3( 0.0 );
	for( int y = 0; y < 4; ++y )
	{
		for( int x = 0; x < 4; ++x )
		{
			vec2 at = uv + ( vec2( float( x ), float( y ) ) - 1.5 ) * SourceTexel;
			total += signalAt( StateTexture, HeldTexture, at, Beam, UseHeld, Gain );
		}
	}

	//Only what is above the threshold scatters. Subtracting the threshold
	//rather than masking on it is what stops the halation having a hard edge
	//of its own at the point where the picture crosses it.
	fragColor = vec4( max( total * ( 1.0 / 16.0 ) - vec3( Threshold ), vec3( 0.0 ) ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 4: blur. No model in it, so it is not assembled.
//---------------------------------------------------------------------------
const char* const kBlurShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform vec2 Direction;///< one axis' step, in uv

in vec2 uv;
out vec4 fragColor;

void main()
{
	//A five-tap Gaussian on a GL_LINEAR buffer: each off-centre tap sits
	//between two texels and averages them, so five fetches cover nine texels.
	vec4 total = texture( SourceTexture, uv ) * 0.2270270270;
	total += texture( SourceTexture, uv + Direction * 1.3846153846 ) * 0.3162162162;
	total += texture( SourceTexture, uv - Direction * 1.3846153846 ) * 0.3162162162;
	total += texture( SourceTexture, uv + Direction * 3.2307692308 ) * 0.0702702703;
	total += texture( SourceTexture, uv - Direction * 3.2307692308 ) * 0.0702702703;
	fragColor = total;
}
)";

//---------------------------------------------------------------------------
// Pass 5: composite.
//---------------------------------------------------------------------------
static const char* const kCompositePreamble = R"(#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D StateTexture;
uniform sampler2D HeldTexture;
uniform sampler2D NarrowTexture;///< the halation
uniform sampler2D WideTexture;  ///< the same, wider; the difference is the halo

uniform vec2 MaxUV;
uniform vec2 HalfTexel;
uniform float Beam;
uniform float Gain;
uniform int UseHeld;

uniform float Halation;
uniform float Halo;
uniform float Monochrome;
uniform float NoiseAmount;
uniform float MixAmount;
uniform uint FieldIndex;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kCompositeMain = R"(
void main()
{
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	vec4 source  = texture( InputTexture, picture * MaxUV );

	vec3 signal = signalAt( StateTexture, HeldTexture, uv, Beam, UseHeld, Gain );

	//Halation: light that got past the target, scattered in the faceplate and
	//came back. Added, because that is what scattered light does.
	vec3 narrow = texture( NarrowTexture, uv ).rgb;
	vec3 wide   = texture( WideTexture, uv ).rgb;
	signal += Halation * narrow;

	//THE HALO IS A BOLTED-ON TERM AND THE ONLY ONE IN THE PLUGIN.
	//
	//An Image Orthicon's black ring is not the charge store doing anything.
	//It is redistribution of secondary electrons knocked off the target and
	//landing back around a highlight, so the ring is a DEFICIT of signal
	//around a bright area. Nothing in targetField() can produce it, because
	//nothing in targetField() lets one point on the target affect another.
	//
	//This is a difference of two Gaussians -- positive in an annulus around a
	//highlight, negative in the middle where the narrow blur dominates -- used
	//subtractively. It is an ARRANGEMENT of the artefact, not a derivation of
	//it, and AGENTS.md says so at length. `Type` pins it to zero on the three
	//tubes that do not do it.
	signal -= Halo * max( wide - narrow, vec3( 0.0 ) );

	//A signal cannot be negative; the halo is the only thing that could make
	//it so.
	signal = max( signal, vec3( 0.0 ) );

	signal = mix( signal, vec3( dot( signal, kLuma ) ), Monochrome );

	//Beam and preamp noise. Additive and level-independent, which is why it is
	//the dark parts of a tube picture that look noisy -- the noise is the same
	//everywhere and there is less picture under it.
	uint seed = uint( gl_FragCoord.x ) * 1973u
	          + uint( gl_FragCoord.y ) * 9277u
	          + FieldIndex * 26699u;
	signal += ( hash01( seed ) - 0.5 ) * NoiseAmount;

	fragColor = vec4( mix( source.rgb, signal, MixAmount ), source.a );
}
)";

//---------------------------------------------------------------------------
std::string TargetShaderSource()
{
	return std::string( kTargetPreamble ) + kTargetLibrarySource + kTargetMain;
}

std::string HeldShaderSource()
{
	return std::string( kHeldPreamble ) + kTargetLibrarySource + kHeldMain;
}

std::string BrightShaderSource()
{
	return std::string( kBrightPreamble ) + kTargetLibrarySource + kBrightMain;
}

std::string CompositeShaderSource()
{
	return std::string( kCompositePreamble ) + kTargetLibrarySource + kCompositeMain;
}

} // namespace plumbicon
