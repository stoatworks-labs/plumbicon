/**
 * Plumbicon — browser demo.
 *
 * The shaders below are copied unedited from `source/Shaders.cpp`, and the four
 * passes that carry the target model are assembled here from the same pieces,
 * in the same order, as `TargetShaderSource()` and its siblings assemble them —
 * so `targetField()`, which is the whole plugin, is the plugin's own text in
 * every pass that uses it. `demo/tools/check_shaders.py` proves the pieces and
 * the assembly character for character, and `tools/verify.sh` runs it.
 *
 * What is a hand port, and is checked by nothing but a reader: `Controls.cpp`
 * (every 0..1 conversion and `VideoGain`), the `Tubes.h` table, `Effective()`,
 * and the frame sequence of `Plumbicon::ProcessOpenGL` — the ping-pong of the
 * target, the held buffers of Two Fields mode, the field parities, the bloom
 * sizes and the four blur stages with the 2.2 wide ratio. The parameter
 * declarations — names, groups, order, element lists and defaults — come from
 * the constructor in `Plumbicon.cpp`.
 *
 * **The float targets.** The plugin keeps its charge store in RGBA32F and its
 * bloom in RGBA16F. WebGL2 can only render into either through
 * EXT_color_buffer_float, so the page asks for it (`needFloat`) and refuses to
 * start without it rather than dropping to eight bits: an 8-bit target would
 * not "look a bit worse", it would stop the burn accumulator ever moving
 * (Controls.h: it moves by 1e-5 of its range per field) and would turn the lag
 * recursion's exact closed form into an approximate one. Nothing else about the
 * floats differs. The state is sampled NEAREST, as in the plugin, so no float
 * filtering extension is involved; the 16F bloom is filtered LINEAR, which
 * WebGL2 allows for half floats in core; and there is no blending into a float
 * target anywhere in the chain. The 32-bit charge is the same IEEE single here
 * as there.
 *
 * **A field is a frame of THIS page.** The plugin advances the target one
 * field per `ProcessOpenGL` call and never reads a clock; here that is one
 * field per frame the page's clock advances, which is your display's refresh
 * rate rather than a composition's frame rate. Two consequences, both on the
 * page: a tail that is N fields long is N/120 s on a 120 Hz display and N/60 s
 * on a 60 Hz one, exactly as a 60 fps composition holds a smear half as long as
 * a 30 fps one; and a redraw that is NOT a new field — moving a slider while
 * paused — re-reads the target without charging it again, where a host calling
 * the plugin twice would advance it twice.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp. Do not edit here.
//===========================================================================

const VERTEX = `#version 410 core

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
`;

const TARGET_LIBRARY = `
//Rec.709 luma. Used for the burn drive and for Monochrome, and for nothing
//else -- the charge model is per channel throughout, because a three-tube
//camera has three targets and they do not share a residue.
const vec3 kLuma = vec3( 0.2126, 0.7152, 0.0722 );

//Does the beam visit this line on this field?
//
//\`parity\` is -1 in Frame mode, which is every line, and 0 or 1 in Two Fields
//mode. INTEGER arithmetic on the line index, never a float \`mod\`: GLSL
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
//   lag        -- the beam takes \`beam\` and leaves the rest, so a highlight
//                 that charged harder than that is read out again next field
//   comet tail -- the same thing at a point the highlight has since left
//   blocked    -- \`min( ..., capacity )\`: past that, more light is not more
//   highlights    charge, so the detail in a highlight is simply not stored
//   burn-in    -- a second, far slower accumulator biasing the sensitivity
//
// If you find yourself adding a term to make one of those appear, stop: the
// mechanism has been lost somewhere above.
//
// \`prevCharge\` is what the target held at the END of the previous field,
// before that field's beam pass. \`prevBeam\` is the beam current that pass
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
//\`min( charge, beam )\` and \`heldTexture\` is bound to the state texture and
//never used. In Two Fields mode the beam visits a line every OTHER field, and
//what the picture shows between visits is the signal the beam took last time
//it was there -- which is why that mode, and only that mode, pays for a
//second pair of buffers.
//
//\`gain\` is the rest of the chain, lined up so that peak signal is peak white.
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
`;

const TARGET_PREAMBLE = `#version 410 core

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
`;

const TARGET_MAIN = `
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
`;

const HELD_PREAMBLE = `#version 410 core

uniform sampler2D StateTexture;///< the state this field just wrote
uniform sampler2D HeldTexture; ///< what the picture has been showing
uniform float Beam;
uniform int FieldParity;       ///< THIS field's parity

in vec2 uv;
out vec4 fragColor;
`;

const HELD_MAIN = `
void main()
{
	vec3 charge = texture( StateTexture, uv ).rgb;
	vec3 previous = texture( HeldTexture, uv ).rgb;

	//A line the beam did not visit keeps showing what it gave up last time.
	//Showing \`min( charge, beam )\` there instead is the obvious thing and it
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
`;

const BRIGHT_PREAMBLE = `#version 410 core

uniform sampler2D StateTexture;
uniform sampler2D HeldTexture;
uniform float Beam;
uniform float Gain;
uniform int UseHeld;
uniform float Threshold;
uniform vec2 SourceTexel;///< one FULL-SIZE texel, in uv

in vec2 uv;
out vec4 fragColor;
`;

const BRIGHT_MAIN = `
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
`;

const BLUR = `#version 410 core

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
`;

const COMPOSITE_PREAMBLE = `#version 410 core

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
`;

const COMPOSITE_MAIN = `
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
	//it, and AGENTS.md says so at length. \`Type\` pins it to zero on the three
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
`;

/// The four model passes, assembled around the library exactly as
/// `TargetShaderSource()`, `HeldShaderSource()`, `BrightShaderSource()` and
/// `CompositeShaderSource()` do it.
const TARGET_SHADER = TARGET_PREAMBLE + TARGET_LIBRARY + TARGET_MAIN;
const HELD_SHADER = HELD_PREAMBLE + TARGET_LIBRARY + HELD_MAIN;
const BRIGHT_SHADER = BRIGHT_PREAMBLE + TARGET_LIBRARY + BRIGHT_MAIN;
const COMPOSITE_SHADER = COMPOSITE_PREAMBLE + TARGET_LIBRARY + COMPOSITE_MAIN;

//===========================================================================
// Controls.cpp, ported. Every range and every "exactly 1.0 at" is the
// plugin's; see Controls.h for why each one is the shape it is.
//===========================================================================

const clamp01 = (p) => Math.min(1, Math.max(0, p));
const f32 = Math.fround;

const sensitivityFromParam = (p) => f32(clamp01(p) * 4.0);
const capacityFromParam = (p) => f32(0.04 + clamp01(p) * 5.96);
const beamCurrentFromParam = (p) => f32(0.02 * Math.pow(300.0, clamp01(p)));
const transferGammaFromParam = (p) => f32(0.5 + clamp01(p));
const darkCurrentFromParam = (p) => { const q = clamp01(p); return f32(0.08 * q * q); };
const lagScaleFromParam = (p) => { const q = clamp01(p); return f32(1.0 / (1.0 + 8.0 * q * q)); };
const leakFromParam = (p) => { const q = clamp01(p); return f32(0.5 * q * q); };
const burnRiseFromParam = (p) => { const q = clamp01(p); return f32(5.0e-3 * q * q); };
const burnFallFromParam = (p) => { const q = clamp01(p); return f32(2.0e-3 * q * q); };
const burnDepthFromParam = (p) => f32(clamp01(p));
const halationFromParam = (p) => f32(clamp01(p) * 1.5);
const halationRadiusFromParam = (p) => f32(0.5 + clamp01(p) * 11.5);
const bloomThresholdFromParam = (p) => f32(clamp01(p) * 2.0);
const haloFromParam = (p) => f32(clamp01(p) * 2.0);
const noiseFromParam = (p) => f32(clamp01(p) * 0.15);

/// The rest of the video chain, lined up so peak signal is peak white. Not a
/// control. Exactly 1.0 at the pass-through settings.
function videoGain(photoGain, dark, beam) {
  const white = photoGain + dark;
  const peak = Math.min(beam, white);
  return peak > 0 ? f32(1.0 / peak) : 1.0;
}

//===========================================================================
// Tubes.h, ported: a type is an OVERRIDE read through Effective(), never a
// write into the sliders.
//===========================================================================

/// `tubes::Param` order, bound to this page's parameter ids the way
/// `kTubeParamIDs` binds them to the plugin's ParamIDs.
const TUBE_PARAM_IDS = ['gamma', 'dark', 'capacity', 'lag', 'burnRate', 'burnRecovery', 'burnDepth', 'halo'];

/// `tubes::kTubes`. A Type VALUE of n names TUBES[n - 1]; 0 is Custom.
const TUBES = [
  //                gamma  dark  cap   lag   rise  fall  depth halo
  ['Plumbicon',      [0.50, 0.05, 0.45, 0.22, 0.12, 0.55, 0.18, 0.00]],
  ['Vidicon',        [0.15, 0.55, 0.95, 0.46, 0.62, 0.22, 0.75, 0.00]],
  ['Saticon',        [0.30, 0.25, 0.70, 0.34, 0.35, 0.38, 0.40, 0.00]],
  ['Image Orthicon', [0.50, 0.30, 0.90, 0.38, 0.30, 0.45, 0.30, 0.75]],
];

/// The Type dropdown, as the constructor declares it. Custom is pinned to the
/// top and the tubes follow in case-insensitive name order — but each element
/// keeps the VALUE of its row in the table, because `SetParamElementInfo`
/// declares a display slot and a stored value separately. The kit's option
/// parameter stores the slot, so the slot is mapped to the value here, exactly
/// where the plugin would have read it.
const TYPE_ORDER = [0, ...TUBES.map((_, i) => i + 1)
  .sort((a, b) => TUBES[a - 1][0].toLowerCase().localeCompare(TUBES[b - 1][0].toLowerCase()))];
const TYPE_ELEMENTS = TYPE_ORDER.map((value) => (value === 0 ? 'Custom' : TUBES[value - 1][0]));
const TYPE_DEFAULT_SLOT = TYPE_ORDER.indexOf(1);// params[ PT_TYPE ] = 1.0f, Plumbicon

const typeValue = (params) => TYPE_ORDER[params.option('type')] ?? 0;

/// `Plumbicon::Effective()`: the slider, unless the tube type pins it.
function effective(params, id) {
  const type = typeValue(params);
  if (type >= 1 && type <= TUBES.length) {
    const j = TUBE_PARAM_IDS.indexOf(id);
    if (j >= 0) return TUBES[type - 1][1][j];
  }
  return params.get(id);
}

/// Field Mode's elements. Ordinal, not alphabetical.
const FIELD_FRAME = 0;
const FIELD_TWO_FIELDS = 1;

/// How much wider the second pair of blur passes is than the first. The halo
/// ring is the difference of the two.
const WIDE_RATIO = 2.2;

//===========================================================================
// The frame, in the order ProcessOpenGL runs it.
//===========================================================================

class PlumbiconRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    this.target = new Program(gl, VERTEX, TARGET_SHADER, 'target');
    this.heldPass = new Program(gl, VERTEX, HELD_SHADER, 'held');
    this.bright = new Program(gl, VERTEX, BRIGHT_SHADER, 'bright');
    this.blur = new Program(gl, VERTEX, BLUR, 'blur');
    this.composite = new Program(gl, VERTEX, COMPOSITE_SHADER, 'composite');

    // NEAREST on the state and held buffers: they are data, not a picture.
    this.state = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.held = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.bloom = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
    this.narrow = new PassBuffer(gl, { filter: 'linear' });
    this.wide = new PassBuffer(gl, { filter: 'linear' });

    this.current = 0;
    this.stateWidth = 0;
    this.stateHeight = 0;
    this.heldReady = false;
    this.fieldIndex = 0;

    this.lastFrameIndex = -1;
    this.lastTime = -1;
    this.readout = null;
  }

  clearAll(wantHeld) {
    this.state[0].clearTo(0, 0, 0, 0);
    this.state[1].clearTo(0, 0, 0, 0);
    if (wantHeld) {
      this.held[0].clearTo(0, 0, 0, 0);
      this.held[1].clearTo(0, 0, 0, 0);
    }
    this.bloom[0].clearTo(0, 0, 0, 0);
    this.bloom[1].clearTo(0, 0, 0, 0);
    this.narrow.clearTo(0, 0, 0, 0);
    this.wide.clearTo(0, 0, 0, 0);
    this.current = 0;
    this.fieldIndex = 0;
  }

  /// `EnsureBuffers()`. PassBuffer.ensure() cannot say whether it reallocated,
  /// so the size is tracked here, as it is in the plugin: a resize has to empty
  /// the target, or the last clip's charge fades out over this one.
  ensureBuffers(width, height, wantHeld) {
    const gl = this.gl;
    this.state[0].ensure(width, height, gl.RGBA32F);
    this.state[1].ensure(width, height, gl.RGBA32F);

    if (wantHeld) {
      this.held[0].ensure(width, height, gl.RGBA32F);
      this.held[1].ensure(width, height, gl.RGBA32F);
    } else if (this.heldReady) {
      // Leaving Two Fields gives the memory back.
      this.held[0].dispose();
      this.held[1].dispose();
    }

    const bloomWidth = Math.max(8, Math.trunc(width / 4));
    const bloomHeight = Math.max(8, Math.trunc(height / 4));
    this.bloom[0].ensure(bloomWidth, bloomHeight, gl.RGBA16F);
    this.bloom[1].ensure(bloomWidth, bloomHeight, gl.RGBA16F);
    this.narrow.ensure(bloomWidth, bloomHeight, gl.RGBA16F);
    this.wide.ensure(bloomWidth, bloomHeight, gl.RGBA16F);

    const resized = width !== this.stateWidth || height !== this.stateHeight;
    if (resized || (wantHeld && !this.heldReady)) {
      this.clearAll(wantHeld);
      this.stateWidth = width;
      this.stateHeight = height;
    }
    this.heldReady = wantHeld;
  }

  render({ input, params, width, height, time, frameIndex }) {
    const gl = this.gl;
    const quad = this.quad;
    const pictureWidth = input.width;
    const pictureHeight = input.height;

    //-----------------------------------------------------------------------
    // What the controls say, with the tube type applied.
    //-----------------------------------------------------------------------
    const fieldMode = params.option('fieldMode');
    const twoFields = fieldMode === FIELD_TWO_FIELDS;

    const photoGain = sensitivityFromParam(effective(params, 'sensitivity'));
    const gamma = transferGammaFromParam(effective(params, 'gamma'));
    const dark = darkCurrentFromParam(effective(params, 'dark'));
    const capacity = capacityFromParam(effective(params, 'capacity'));
    const beam = f32(beamCurrentFromParam(effective(params, 'beam')) * lagScaleFromParam(effective(params, 'lag')));
    const leak = leakFromParam(effective(params, 'recovery'));
    const burnRise = burnRiseFromParam(effective(params, 'burnRate'));
    const burnFall = burnFallFromParam(effective(params, 'burnRecovery'));
    const burnDepth = burnDepthFromParam(effective(params, 'burnDepth'));
    const halation = halationFromParam(effective(params, 'halation'));
    const radius = halationRadiusFromParam(effective(params, 'halationRadius'));
    const threshold = bloomThresholdFromParam(effective(params, 'bloomThreshold'));
    const halo = haloFromParam(effective(params, 'halo'));
    const noise = noiseFromParam(effective(params, 'noise'));
    const gain = videoGain(photoGain, dark, beam);

    //-----------------------------------------------------------------------
    // Buffers, all before anything binds a texture.
    //-----------------------------------------------------------------------
    this.ensureBuffers(pictureWidth, pictureHeight, twoFields);

    // Restart sends this page's clock back to zero. The plugin has no such
    // event; here it means the visitor asked for the top, so the target —
    // burn included — is emptied with it. Said on the page.
    if (this.lastTime >= 0 && time < this.lastTime) this.clearAll(twoFields);
    this.lastTime = time;

    // A new field only when the page's clock moved on. A redraw with the clock
    // standing still (a slider moved while paused) re-reads the target it
    // already has.
    const newField = frameIndex !== this.lastFrameIndex;
    this.lastFrameIndex = frameIndex;

    gl.disable(gl.BLEND);

    if (newField) {
      const next = 1 - this.current;
      const fieldParity = twoFields ? (this.fieldIndex & 1) : -1;
      const prevParity = twoFields ? ((this.fieldIndex + 1) & 1) : -1;

      //---------------------------------------------------------------------
      // 1. The target. One field: light in, charge stored, beam to come.
      //---------------------------------------------------------------------
      this.state[next].bind();
      this.target.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, this.state[this.current].texture);
      this.target.setSampler('InputTexture', 0);
      this.target.setSampler('StateTexture', 1);
      this.target.set('MaxUV', 1, 1);
      this.target.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
      this.target.set('PhotoGain', photoGain);
      this.target.set('Gamma', gamma);
      this.target.set('DarkCurrent', dark);
      this.target.set('Capacity', capacity);
      this.target.set('Beam', beam);
      this.target.set('Leak', leak);
      this.target.set('BurnRise', burnRise);
      this.target.set('BurnFall', burnFall);
      this.target.set('BurnDepth', burnDepth);
      this.target.setInt('PrevParity', prevParity);
      quad.draw();

      //---------------------------------------------------------------------
      // 2. The held signal. Two Fields only.
      //---------------------------------------------------------------------
      if (twoFields) {
        this.held[next].bind();
        this.heldPass.use();
        bindTexture(gl, 0, this.state[next].texture);
        bindTexture(gl, 1, this.held[this.current].texture);
        this.heldPass.setSampler('StateTexture', 0);
        this.heldPass.setSampler('HeldTexture', 1);
        this.heldPass.set('Beam', beam);
        this.heldPass.setInt('FieldParity', fieldParity);
        quad.draw();
      }

      this.current = next;
      this.fieldIndex = (this.fieldIndex + 1) >>> 0;
    }

    // In Frame mode the held sampler points at the state buffer and is never
    // read — UseHeld is zero — exactly as in the plugin.
    const signalState = this.state[this.current].texture;
    const signalHeld = twoFields ? this.held[this.current].texture : signalState;
    const useHeld = twoFields ? 1 : 0;

    //-----------------------------------------------------------------------
    // 3-7. The optics, skipped when neither control asks for them.
    //-----------------------------------------------------------------------
    if (halation > 0 || halo > 0) {
      const bloomWidth = this.bloom[0].width;
      const bloomHeight = this.bloom[0].height;

      this.bloom[0].bind();
      this.bright.use();
      bindTexture(gl, 0, signalState);
      bindTexture(gl, 1, signalHeld);
      this.bright.setSampler('StateTexture', 0);
      this.bright.setSampler('HeldTexture', 1);
      this.bright.set('Beam', beam);
      this.bright.set('Gain', gain);
      this.bright.set('Threshold', threshold);
      this.bright.set('SourceTexel', 1 / pictureWidth, 1 / pictureHeight);
      this.bright.setInt('UseHeld', useHeld);
      quad.draw();

      const stepX = radius / bloomWidth;
      const stepY = radius / bloomHeight;
      const stages = [
        [this.bloom[0], this.bloom[1], stepX, 0],
        [this.bloom[1], this.narrow, 0, stepY],
        [this.narrow, this.bloom[1], stepX * WIDE_RATIO, 0],
        [this.bloom[1], this.wide, 0, stepY * WIDE_RATIO],
      ];
      for (const [from, to, x, y] of stages) {
        to.bind();
        this.blur.use();
        bindTexture(gl, 0, from.texture);
        this.blur.setSampler('SourceTexture', 0);
        this.blur.set('Direction', x, y);
        quad.draw();
      }
    }

    //-----------------------------------------------------------------------
    // 8. Composite, straight to the canvas.
    //-----------------------------------------------------------------------
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    this.composite.use();
    bindTexture(gl, 0, input.texture);
    bindTexture(gl, 1, signalState);
    bindTexture(gl, 2, signalHeld);
    bindTexture(gl, 3, this.narrow.texture);
    bindTexture(gl, 4, this.wide.texture);
    this.composite.setSampler('InputTexture', 0);
    this.composite.setSampler('StateTexture', 1);
    this.composite.setSampler('HeldTexture', 2);
    this.composite.setSampler('NarrowTexture', 3);
    this.composite.setSampler('WideTexture', 4);
    this.composite.set('MaxUV', 1, 1);
    this.composite.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
    this.composite.set('Beam', beam);
    this.composite.set('Gain', gain);
    this.composite.set('Halation', halation);
    this.composite.set('Halo', halo);
    this.composite.set('Monochrome', effective(params, 'monochrome'));
    this.composite.set('NoiseAmount', noise);
    this.composite.set('MixAmount', effective(params, 'mix'));
    this.composite.setInt('UseHeld', useHeld);
    this.composite.setUint('FieldIndex', (this.fieldIndex - 1) >>> 0);
    quad.draw();

    // Leave nothing bound on the higher units for the kit's next pass.
    for (let unit = 4; unit >= 0; unit -= 1) bindTexture(gl, unit, null);

    this.report(capacity, beam, photoGain, dark);
  }

  /// One line under the canvas: the two numbers Controls.h says decide
  /// everything, so a visitor can see why a setting smears or blocks.
  report(capacity, beam, photoGain, dark) {
    // Not in embed mode: there the page is a video source and has no reader.
    if (document.body.dataset.embed !== undefined) return;
    if (!this.readout) {
      const stage = document.querySelector('.stage__status');
      if (!stage) return;
      this.readout = document.createElement('p');
      this.readout.className = 'stage__status';
      this.readout.setAttribute('aria-live', 'off');
      stage.after(this.readout);
    }
    const tail = capacity / beam;
    const block = beam / Math.max(photoGain, 1e-6);
    this.readout.textContent =
      `Field ${this.fieldIndex}. Tail ≈ capacity / beam = ${tail.toFixed(1)} fields. `
      + `The target blocks up above ${block.toFixed(2)} × peak white`
      + `${dark > 0 ? `, plus ${dark.toFixed(4)} of dark current a field` : ''}.`;
  }
}

//===========================================================================
// The parameters, from the constructor in Plumbicon.cpp.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });

/// The readout for a control a tube type pins: its own number, and "pinned"
/// when the type is overriding it — the slider moves and the picture does not.
const PINNED = ' (pinned by Type)';

const PARAMS = [
  { id: 'type', name: 'Type', type: 'option', default: TYPE_DEFAULT_SLOT, group: 'Tube', elements: TYPE_ELEMENTS,
    hint: 'What the tube is made of. A type is an OVERRIDE, not a write: while one is selected, Transfer Gamma, Dark Current, Target Capacity, Lag Amount, the three Burn controls and Halo are read from the type’s row and their sliders are inert. Custom means the sliders are the truth.' },
  std('sensitivity', 'Sensitivity', 0.34, 'Tube', {
    display: (v) => `${sensitivityFromParam(v).toFixed(2)} charge/field`,
    hint: 'Charge deposited per field per unit of light. 1.0 is unity. A real comet tail comes from something far brighter than peak white, and a clip has nothing above white in it — so this is what pushes the clip’s own highlights past what the beam can read in one pass.' }),
  std('capacity', 'Target Capacity', 0.45, 'Tube', {
    display: (v) => `${capacityFromParam(v).toFixed(2)}`,
    hint: 'The most charge the target can hold. Light past this is not stored, which is why highlights block up. Capacity over beam current is the length of the tail in fields. Pinned by Type.' }),
  std('beam', 'Beam Current', 0.77, 'Tube', {
    display: (v) => `${beamCurrentFromParam(v).toFixed(2)}`,
    hint: 'Charge the beam can remove in one field, before Lag Amount scales it. Logarithmic over 300:1; at the top the beam empties a full target in one pass and there is no lag at all.' }),
  std('gamma', 'Transfer Gamma', 0.50, 'Tube', {
    display: (v) => `γ ${transferGammaFromParam(v).toFixed(2)}`,
    hint: 'The transfer characteristic’s exponent: 1.0 is a plumbicon, 0.65 a vidicon. Pinned by Type.' }),
  std('dark', 'Dark Current', 0.05, 'Tube', {
    display: (v) => `${darkCurrentFromParam(v).toFixed(4)}/field`,
    hint: 'Charge that arrives whether or not there is light. It lifts the blacks and gives the dark parts of the picture lag of their own. Pinned by Type.' }),

  std('lag', 'Lag Amount', 0.22, 'Lag', {
    display: (v) => `beam × ${lagScaleFromParam(v).toFixed(3)}`,
    hint: 'A multiplier on the beam current, exactly 1 at zero and 1/9 at the top. The same physical quantity as Beam Current, reached from the other side. Pinned by Type.' }),
  std('recovery', 'Recovery', 0.15, 'Lag', {
    display: (v) => `${(leakFromParam(v) * 100).toFixed(2)}% leak/field`,
    hint: 'How much residual charge leaks away on its own between scans. Exactly zero at the bottom, which is where the pure lag recursion lives.' }),
  { id: 'fieldMode', name: 'Field Mode', type: 'option', default: FIELD_FRAME, group: 'Lag', elements: ['Frame', 'Two Fields'],
    hint: 'Frame: the beam reads every line every field. Two Fields: alternate lines on alternate fields, so a line holds what it gave up two fields ago — the interlaced camera, and the reason that mode alone keeps two extra buffers.' },

  std('burnRate', 'Burn Rate', 0.12, 'Burn', {
    display: (v) => { const r = burnRiseFromParam(v); return r > 0 ? `τ ${Math.round(1 / r)} fields` : 'never'; },
    hint: 'How fast a bright, still area etches the target: the rising pole of a second, far slower accumulator. Pinned by Type.' }),
  std('burnRecovery', 'Burn Recovery', 0.55, 'Burn', {
    display: (v) => { const r = burnFallFromParam(v); return r > 0 ? `τ ${Math.round(1 / r)} fields` : 'never'; },
    hint: 'How fast the etch recovers, deliberately 2.5 times slower than it rises at the same position. A tube etches faster than it recovers. Pinned by Type.' }),
  std('burnDepth', 'Burn Depth', 0.18, 'Burn', {
    display: (v) => `${(burnDepthFromParam(v) * 100).toFixed(0)}% of sensitivity`,
    hint: 'How much sensitivity a fully burnt area loses. Pinned by Type.' }),

  std('halation', 'Halation', 0.35, 'Optics', {
    display: (v) => `${halationFromParam(v).toFixed(2)}`,
    hint: 'Light that got past the target, scattered in the faceplate and came back, added around what is above Bloom Threshold.' }),
  std('halationRadius', 'Halation Radius', 0.40, 'Optics', {
    display: (v) => `${halationRadiusFromParam(v).toFixed(1)} texels`,
    hint: 'The halation’s blur, in texels of the quarter-size bloom buffer.' }),
  std('bloomThreshold', 'Bloom Threshold', 0.35, 'Optics', {
    display: (v) => `${bloomThresholdFromParam(v).toFixed(2)}`,
    hint: 'The signal level above which light scatters. Subtracted rather than masked, so the halation has no edge of its own.' }),
  std('halo', 'Halo', 0.0, 'Optics', {
    display: (v) => `${haloFromParam(v).toFixed(2)}`,
    hint: 'The Image Orthicon’s black ring: the ONE term in the plugin that does not fall out of the charge store. A difference of two blurs used subtractively — an arrangement of the artefact, not a derivation of it, and the plugin says so. Pinned by Type, to zero on the three tubes that do not do it.' }),

  std('monochrome', 'Monochrome', 0.0, 'Output', {
    display: (v) => `${(v * 100).toFixed(0)}%`,
    hint: 'Rec.709 luma of the signal, mixed in.' }),
  std('noise', 'Noise', 0.18, 'Output', {
    display: (v) => `${noiseFromParam(v).toFixed(3)} p-p`,
    hint: 'Beam and preamp noise: additive and level-independent, which is why the dark parts of a tube picture look noisy.' }),
  std('mix', 'Mix', 1.0, 'Output', {
    display: (v) => `${(v * 100).toFixed(0)}%`,
    hint: 'The camera against the clip. Zero is the null.' }),
];

// The readout of a pinned slider says so while a type is overriding it.
let currentParams = null;
for (const p of PARAMS) {
  if (!TUBE_PARAM_IDS.includes(p.id)) continue;
  const show = p.display;
  p.display = (v) => {
    const pinned = currentParams && typeValue(currentParams) !== 0;
    return pinned ? `${show(effective(currentParams, p.id))}${PINNED}` : show(v);
  };
}

const demo = mountDemo({
  name: 'Plumbicon',
  pluginId: 'PB01',
  tagline: 'A camera tube: a target that stores charge, and a beam that reads it by discharging it. Lag, comet tails, blocked highlights and burn-in are that one store of charge, seen from four sides.',
  repo: 'https://github.com/stoatworks-labs/plumbicon',
  page: 'https://stoatworks-labs.com/software/plumbicon/',
  video: 'https://www.youtube.com/watch?v=HQ-j39oUCgE',

  // RGBA32F state, RGBA16F bloom. See the note at the top.
  needFloat: true,

  // Lights on black first: a comet tail is a highlight that moved, and on a
  // full-contrast photographic frame everything smears at once — which is the
  // plugin working and the worst possible first look at it.
  sources: ['spot', 'scene', 'grid', 'bars', 'ramp', 'detail'],

  params: PARAMS,

  differences: [
    'The shaders — the target model in all four passes that use it, the blur and the composite — are the plugin’s own text, assembled in the plugin’s order, and demo/tools/check_shaders.py proves it. The control conversions (Controls.cpp), the tube table (Tubes.h), Effective() and the frame sequence of ProcessOpenGL are a hand port to JavaScript. Nothing checks that port but a reader.',
    'Float render targets. The plugin keeps the charge in RGBA32F and the bloom in RGBA16F; WebGL2 can render into neither without EXT_color_buffer_float, so this page refuses to start without it rather than falling back to 8 bits, where the burn could never move and the lag would stop being exact. That is the only float difference: the state is sampled NEAREST as in the plugin, the 16-bit bloom is filtered LINEAR (core in WebGL2), and nothing blends into a float target. The 32-bit charge is the same IEEE single here as there.',
    'A field is one frame of this page, which is your display’s refresh rate — in Resolume it is one frame of the composition. A tail N fields long is therefore N/120 s on a 120 Hz display and N/60 s on a 60 Hz one. A redraw that is not a new field (a slider moved while paused) re-reads the target without charging it again; a host that called the plugin twice would advance it twice.',
    'Restart empties the target, burn and all. The plugin has no such control — it clears only when the picture changes size — but here Restart means “from the top”, and a burn built over a minute would otherwise outlast it.',
    'The Type dropdown lists the tubes in the plugin’s display order and maps each entry to the value the plugin stores for it (Custom 0, Plumbicon 1, Vidicon 2, Saticon 3, Image Orthicon 4), as its SetParamElementInfo calls do. A link from “Copy link” records the list position, not that value.',
    'The About block — the version line and the link buttons the plugin draws in Resolume’s inspector — is not here; the links are in the header. The plugin has no audio path, so nothing is missing on that side.',
  ],

  createRenderer: (gl, quad) => new PlumbiconRenderer(gl, quad),
});

// A pinned slider's readout depends on Type, so a change of Type re-reads
// every readout. The kit's panel re-syncs all its rows on `reset`.
currentParams = demo?.params ?? null;
if (currentParams) {
  currentParams.addEventListener('change', (event) => {
    if (event.detail?.id === 'type') currentParams.dispatchEvent(new CustomEvent('reset'));
  });
  currentParams.dispatchEvent(new CustomEvent('reset'));
}
