#pragma once

#include "PassBuffer.h"
#include "StoatworksAboutParams.h"
#include "Tubes.h"

#include <FFGLSDK.h>

#include <string>

/**
	Plumbicon -- a camera tube, as an FFGL effect.

	**What it is.** A photoconductive target that stores charge. Light charges
	it; the scanning electron beam reads it by discharging it; the beam can
	only remove so much charge per pass. That is the whole plugin, and it is
	one function -- `targetField()` in `Shaders.cpp`.

	**What falls out of it**, rather than having been drawn:

	- **lag**, because a highlight that charged harder than the beam can
	  discharge is still there to be read out on the following fields;
	- **a comet tail**, because that same residue is at every point the
	  highlight has passed over, and a moving highlight has passed over a
	  line of them;
	- **blocked highlights**, because the target saturates and past that more
	  light is not more charge, so there is no detail in there to read;
	- **burn-in**, from a second accumulator with a time constant in minutes
	  biasing the local sensitivity down.

	There is no tail term, no smear term and no ghost term anywhere in this
	repo. There is **one** bolted-on artefact -- the Image Orthicon's black
	halo, which is a different mechanism entirely -- and it is flagged
	wherever it appears.

	**What this is NOT.** It is not a screen. Nothing here models a display:
	no phosphor, no ghosting, no vertical hold, no dot crawl, no shadow mask,
	no scanline. That is `old-cathode`, which is a sibling and not a
	competitor: put this before it and you have the whole chain, camera then
	monitor.

	**Time is measured in fields**, one per `ProcessOpenGL` call, and the
	charge model never reads a clock. See `Controls.h` for why, and for the
	two costs.
*/
class Plumbicon : public CFFGLPlugin
{
public:
	Plumbicon();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and `CFFGLPlugin`'s
	/// `SetTextParameter` is a stub that returns exactly that failure. Omit
	/// this and no host can create the plugin while every offline check here
	/// still passes.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Accepted, and deliberately inert. The charge model advances one field
	/// per `ProcessOpenGL` call and never reads a clock -- see `Controls.h`.
	/// It is taken and logged so that a host which drives it is not surprised
	/// by a refusal, and so the diagnostics say what the host's clock looked
	/// like if that ever matters.
	FFResult SetTime( double time ) override;

	/// The effective value of a parameter, with the Type override applied.
	/// Public so `pbtest` can print what the plugin will actually run with
	/// rather than what the slider says -- those are different numbers
	/// whenever Type is not Custom, and a harness that printed the slider
	/// would quietly be testing something else.
	float Effective( unsigned int index ) const;

	/// The ParamID each `tubes::Param` drives, in table order. Handed out
	/// rather than copied, so a second list cannot go out of step.
	static const unsigned int* TubeParamIDsForTest( int& count );

	/// The order the host shows them in: what the tube is, then how it lags,
	/// then how it burns, then the glass in front of it, then the output.
	///
	/// `SetParamGroup` collapses RUNS of consecutive same-group ids, so this
	/// order is load-bearing: insert a parameter mid-enum and a group
	/// silently splits in two AND every saved composition renumbers. Append
	/// only.
	enum ParamID : FFUInt32
	{
		//Tube
		PT_TYPE,
		PT_SENSITIVITY,
		PT_CAPACITY,
		PT_BEAM,
		PT_GAMMA,
		PT_DARK,

		//Lag
		PT_LAG,
		PT_RECOVERY,
		PT_FIELD_MODE,

		//Burn
		PT_BURN_RATE,
		PT_BURN_RECOVERY,
		PT_BURN_DEPTH,

		//Optics
		PT_HALATION,
		PT_HALATION_RADIUS,
		PT_BLOOM_THRESHOLD,
		PT_HALO,

		//Output
		PT_MONOCHROME,
		PT_NOISE,
		PT_MIX,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// What Field Mode stores. Ordinal, not alphabetical: this is a
	/// progression rather than a list to look a name up in.
	enum FieldMode
	{
		kFieldFrame     = 0,///< the beam visits every line every field
		kFieldTwoFields = 1,///< alternate lines, so a line is read every other field
		kFieldCount
	};

private:
	/// Bring the state buffers to this size, clearing them if the size moved.
	///
	/// `PassBuffer::Ensure()` returns a bool and reuses a buffer that already
	/// matches, so it cannot tell us whether it REALLOCATED. The width and
	/// height are tracked here for that reason: a resize has to empty the
	/// accumulators, and a target still holding the last composition's charge
	/// is a picture of somebody else's clip fading out over the first second
	/// of this one.
	bool EnsureBuffers( int pictureWidth, int pictureHeight, bool wantHeld );

	ffglex::FFGLShader targetShader;
	ffglex::FFGLShader heldShader;
	ffglex::FFGLShader brightShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	//---------------------------------------------------------------------
	// The target.
	//
	// RGBA32F and not 16F, and that is not caution. The burn accumulator
	// moves by as little as 1e-5 of its own range per field, which is an
	// order of magnitude below a half-float's epsilon at 1.0 -- in 16F the
	// burn would simply never start. The charge needs the range for a
	// different reason: the lag recursion subtracts the same beam current
	// from the same charge dozens of times, and half-float rounding on each
	// step turns an exact closed form into an approximate one.
	//
	// The cost is memory, and it is the honest headline number for this
	// plugin: two picture-sized RGBA32F buffers, which is 66 MB at 1080p and
	// 265 MB at 4K. Two Fields mode adds a third and a fourth.
	//---------------------------------------------------------------------
	plumbicon::PassBuffer state[ 2 ];///< rgb charge (post-light, pre-beam), a burn
	plumbicon::PassBuffer held[ 2 ]; ///< Two Fields only: the signal between visits
	plumbicon::PassBuffer bloom[ 2 ];///< quarter size, ping-ponged by the blur
	plumbicon::PassBuffer narrow;    ///< the halation
	plumbicon::PassBuffer wide;      ///< the same, wider; the difference is the halo

	int current     = 0;///< which of state[]/held[] holds the previous field
	int stateWidth  = 0;
	int stateHeight = 0;
	bool heldReady  = false;

	/// Fields since the buffers were last cleared. Drives the scan parity and
	/// the noise, and nothing else. It is not a clock and does not pretend to
	/// be one.
	unsigned int fieldIndex = 0;

	double hostTime = -1.0;
	int clockFrames = 0;

	float params[ PT_COUNT ] = {};

	/// The ParamID each tube type pins, in `tubes::Param` order.
	static constexpr unsigned int kTubeParamIDs[ plumbicon::tubes::kParamCount ] = {
		PT_GAMMA, PT_DARK, PT_CAPACITY, PT_LAG,
		PT_BURN_RATE, PT_BURN_RECOVERY, PT_BURN_DEPTH, PT_HALO
	};

	/// `GetTextParameter` hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
