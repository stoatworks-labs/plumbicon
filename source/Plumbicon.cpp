#include "Plumbicon.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace plumbicon;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Plumbicon >,// Create method
	"PB01",                    // Plugin unique ID of maximum length 4.
	"SW Plumbicon",            // Plugin name
	2,                         // API major version number
	1,                         // API minor version number
	0,                         // Plugin major version number
	1,                         // Plugin minor version number
	FF_EFFECT,                 // Plugin type
	"A camera tube: a target that stores charge, and a beam that reads it by discharging it.\n\nThe beam can only take so much charge per pass, so a highlight that charged harder than that is read out again on the following fields. That is lag, and a moving highlight therefore drags a comet tail behind it. The target saturates, so highlights block up. Leave it pointing at something bright for long enough and the target etches - burn-in, recovering over minutes.\n\nNone of those is drawn. All four are the same store of charge, seen from different angles.\n\nThis is the CAMERA, not the screen. Nothing here models a display - no phosphor, no ghosting, no vertical hold, no dot crawl. Put SW Old Cathode after it for that.\n\nStart with Type, then Sensitivity: a real comet tail comes from something far brighter than peak white, and a clip has nothing above white in it.",// Plugin description
	"Plumbicon FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Ordinal, not alphabetical: Frame -> Two Fields is a progression, and
/// sorted it would read "Frame, Two Fields" by luck rather than by intent.
const char* const kFieldModeNames[] = { "Frame", "Two Fields" };

/// How much wider the second pair of blur passes is than the first. The halo
/// ring is the difference of the two, so this number is the ring's radius
/// relative to the halation's -- not a free parameter to tidy.
constexpr float kWideRatio = 2.2f;

/// Case-insensitive name order, so a list sorts the way a reader expects it
/// to rather than the way a byte comparison does.
bool nameLess( const char* a, const char* b )
{
	for( ; *a && *b; ++a, ++b )
	{
		const int ca = std::tolower( static_cast< unsigned char >( *a ) );
		const int cb = std::tolower( static_cast< unsigned char >( *b ) );
		if( ca != cb )
			return ca < cb;
	}
	return *b != '\0';
}
} // namespace

constexpr unsigned int Plumbicon::kTubeParamIDs[ tubes::kParamCount ];

//---------------------------------------------------------------------------
Plumbicon::Plumbicon()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Accepted and inert -- see the declaration. Saying so is cheaper than a
	//host discovering the refusal.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults.
	//
	// They add up to a slightly over-exposed plumbicon: the top third of the
	// picture blocks up and smears for about five fields, with a little
	// halation and a little noise. The null is Mix at zero.
	//
	// The exposure is deliberate and it is the one thing about this plugin
	// that needs explaining. A real comet tail comes from an object a hundred
	// times brighter than peak white -- a lamp, a sun glint, a welding arc.
	// A clip has no such object: everything above white was thrown away
	// before the plugin ever saw it. So Sensitivity has to push the clip's
	// own highlights past the point where the beam can keep up, and that
	// costs some headroom in the midtones. There is no setting that gives a
	// comet tail on a correctly exposed 8-bit picture, because there is
	// nothing in one for the tail to come from.
	//---------------------------------------------------------------------
	params[ PT_TYPE ]        = 1.0f;  //Plumbicon
	params[ PT_SENSITIVITY ] = 0.34f; //1.36 charge per field at peak white
	params[ PT_CAPACITY ]    = 0.865f;//5.19 -- about six fields of tail
	params[ PT_BEAM ]        = 0.77f; //1.61 before the lag scale, 0.91 after

	//Pinned by Type. The values here are what a Custom tube starts from, and
	//they are the Plumbicon row so that switching to Custom changes nothing.
	params[ PT_GAMMA ]          = 0.50f;//exactly 1.0
	params[ PT_DARK ]           = 0.05f;
	params[ PT_LAG ]            = 0.18f;
	params[ PT_BURN_RATE ]      = 0.12f;
	params[ PT_BURN_RECOVERY ]  = 0.55f;
	params[ PT_BURN_DEPTH ]     = 0.18f;
	params[ PT_HALO ]           = 0.0f;

	params[ PT_RECOVERY ]   = 0.15f;
	params[ PT_FIELD_MODE ] = static_cast< float >( kFieldFrame );

	params[ PT_HALATION ]         = 0.35f;
	params[ PT_HALATION_RADIUS ]  = 0.40f;
	params[ PT_BLOOM_THRESHOLD ]  = 0.35f;

	params[ PT_MONOCHROME ] = 0.0f;
	params[ PT_NOISE ]      = 0.18f;
	params[ PT_MIX ]        = 1.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for
	// a charge or a number of fields. SetParamInfo clamps an FF_TYPE_STANDARD
	// default into 0..1 *before* a range can be attached (SDK b1afaf9), so a
	// parameter declared in charge units cannot declare a default in them.
	// The conversions live in Controls.cpp.
	//
	// Option lists declare an element's display SLOT and its stored VALUE as
	// different arguments, and the spec is explicit that picking an option
	// gives the parameter "a value equal to that of the option's value". So a
	// list can be sorted for whoever has to read it without a saved
	// composition or the harness changing meaning.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_TYPE, "Type", 1 + tubes::kCount, params[ PT_TYPE ] );
	{
		//Custom is pinned to the top. It is not a tube -- it is the statement
		//that no tube is pinning anything -- so a list that filed it between
		//Image Orthicon and Plumbicon would be lying about what it is.
		std::vector< int > order( static_cast< size_t >( tubes::kCount ) );
		for( int i = 0; i < tubes::kCount; ++i )
			order[ i ] = i + 1;
		std::stable_sort( order.begin(), order.end(), []( int a, int b ) {
			return nameLess( tubes::kTubes[ a - 1 ].name, tubes::kTubes[ b - 1 ].name );
		} );

		SetParamElementInfo( PT_TYPE, 0, "Custom", 0.0f );
		for( int slot = 0; slot < tubes::kCount; ++slot )
			SetParamElementInfo( PT_TYPE, static_cast< unsigned int >( slot + 1 ),
			                     tubes::kTubes[ order[ slot ] - 1 ].name,
			                     static_cast< float >( order[ slot ] ) );
	}

	SetParamInfof( PT_SENSITIVITY, "Sensitivity", FF_TYPE_STANDARD );
	SetParamInfof( PT_CAPACITY, "Target Capacity", FF_TYPE_STANDARD );
	SetParamInfof( PT_BEAM, "Beam Current", FF_TYPE_STANDARD );
	SetParamInfof( PT_GAMMA, "Transfer Gamma", FF_TYPE_STANDARD );
	SetParamInfof( PT_DARK, "Dark Current", FF_TYPE_STANDARD );

	SetParamInfof( PT_LAG, "Lag Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_RECOVERY, "Recovery", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_FIELD_MODE, "Field Mode", kFieldCount, params[ PT_FIELD_MODE ] );
	for( int i = 0; i < kFieldCount; ++i )
		SetParamElementInfo( PT_FIELD_MODE, static_cast< unsigned int >( i ),
		                     kFieldModeNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_BURN_RATE, "Burn Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_BURN_RECOVERY, "Burn Recovery", FF_TYPE_STANDARD );
	SetParamInfof( PT_BURN_DEPTH, "Burn Depth", FF_TYPE_STANDARD );

	SetParamInfof( PT_HALATION, "Halation", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALATION_RADIUS, "Halation Radius", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLOOM_THRESHOLD, "Bloom Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALO, "Halo", FF_TYPE_STANDARD );

	SetParamInfof( PT_MONOCHROME, "Monochrome", FF_TYPE_STANDARD );
	SetParamInfof( PT_NOISE, "Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//Nineteen parameters is past the point where an ungrouped list in
	//somebody else's inspector stops being readable. SetParamGroup collapses
	//RUNS of consecutive same-group ids, which is why the enum order in the
	//header is load-bearing.
	for( FFUInt32 i = PT_TYPE; i <= PT_DARK; ++i )
		SetParamGroup( i, "Tube" );
	for( FFUInt32 i = PT_LAG; i <= PT_FIELD_MODE; ++i )
		SetParamGroup( i, "Lag" );
	for( FFUInt32 i = PT_BURN_RATE; i <= PT_BURN_DEPTH; ++i )
		SetParamGroup( i, "Burn" );
	for( FFUInt32 i = PT_HALATION; i <= PT_HALO; ++i )
		SetParamGroup( i, "Optics" );
	for( FFUInt32 i = PT_MONOCHROME; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Plumbicon effect" );

	diag::init();
}

//---------------------------------------------------------------------------
float Plumbicon::Effective( unsigned int index ) const
{
	if( index >= PT_COUNT )
		return 0.0f;

	//A tube type is an OVERRIDE, not a write. The host's parameters are never
	//touched, so there is nothing for a host that restates its own values to
	//argue with -- which is the whole reason this is not the fleet's
	//copy-based preset mechanism. See Tubes.h.
	const int type = static_cast< int >( std::lround( params[ PT_TYPE ] ) );
	if( type >= 1 && type <= tubes::kCount )
	{
		for( int j = 0; j < tubes::kParamCount; ++j )
			if( kTubeParamIDs[ j ] == index )
				return tubes::kTubes[ type - 1 ].v[ j ];
	}

	return params[ index ];
}

const unsigned int* Plumbicon::TubeParamIDsForTest( int& count )
{
	count = tubes::kParamCount;
	return kTubeParamIDs;
}

//---------------------------------------------------------------------------
FFResult Plumbicon::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Assembled rather than written out, because the target model in the
	//middle of each of them is one shared string. Held in locals so the
	//pointers handed to Compile outlive the call.
	const std::string targetSource    = TargetShaderSource();
	const std::string heldSource      = HeldShaderSource();
	const std::string brightSource    = BrightShaderSource();
	const std::string compositeSource = CompositeShaderSource();

	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &targetShader, targetSource.c_str(), "target" },
		{ &heldShader, heldSource.c_str(), "held" },
		{ &brightShader, brightSource.c_str(), "bright" },
		{ &blurShader, kBlurShader, "blur" },
		{ &compositeShader, compositeSource.c_str(), "composite" },
	};

	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Plumbicon: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Plumbicon: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	current     = 0;
	fieldIndex  = 0;
	stateWidth  = 0;
	stateHeight = 0;
	heldReady   = false;

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Plumbicon::EnsureBuffers( int pictureWidth, int pictureHeight, bool wantHeld )
{
	//NEAREST on the state and held buffers, because they are DATA and not a
	//picture. Every pass that reads them reads them texel for texel; a
	//GL_LINEAR fetch at a texel centre is meant to return that texel exactly,
	//and on a buffer that feeds back into itself once per field, "meant to"
	//is not a thing to rest a charge store on.
	const bool ok =
		state[ 0 ].Ensure( pictureWidth, pictureHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest )
		&& state[ 1 ].Ensure( pictureWidth, pictureHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( !ok )
		return false;

	if( wantHeld )
	{
		if( !held[ 0 ].Ensure( pictureWidth, pictureHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest )
		    || !held[ 1 ].Ensure( pictureWidth, pictureHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
			return false;
	}
	else if( heldReady )
	{
		//Leaving Two Fields really does have to give the memory back. Two
		//picture-sized RGBA32F buffers is 265 MB at 4K, and an operator who
		//auditioned the mode once should not still be paying for it.
		held[ 0 ].Destroy();
		held[ 1 ].Destroy();
	}

	//Quarter size, and 16F rather than 32F: this is a blurred copy of a
	//picture on its way to being added to another picture, so half a float's
	//worth of mantissa is three digits more than the eye or the output will
	//ever see. It is also the difference between 4 MB and 8 MB a buffer at 4K.
	const int bloomWidth  = std::max( 8, pictureWidth / 4 );
	const int bloomHeight = std::max( 8, pictureHeight / 4 );
	if( !bloom[ 0 ].Ensure( bloomWidth, bloomHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
	    || !bloom[ 1 ].Ensure( bloomWidth, bloomHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
	    || !narrow.Ensure( bloomWidth, bloomHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
	    || !wide.Ensure( bloomWidth, bloomHeight, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
		return false;

	const bool resized = pictureWidth != stateWidth || pictureHeight != stateHeight;
	if( resized || ( wantHeld && !heldReady ) )
	{
		//PassBuffer::Ensure() reuses a buffer that already matches and cannot
		//tell us whether it reallocated, so the size is tracked here. A
		//target carried across a resize is not "a frame of noise": it is a
		//charge store, so whatever was on it decays over the next second of
		//programme, and the operator sees the last composition fading out of
		//this one.
		state[ 0 ].Clear();
		state[ 1 ].Clear();
		if( wantHeld )
		{
			held[ 0 ].Clear();
			held[ 1 ].Clear();
		}
		bloom[ 0 ].Clear();
		bloom[ 1 ].Clear();
		narrow.Clear();
		wide.Clear();

		current     = 0;
		fieldIndex  = 0;
		stateWidth  = pictureWidth;
		stateHeight = pictureHeight;

		diag::info( "target rebuilt: " + std::to_string( pictureWidth ) + "x"
		            + std::to_string( pictureHeight ) + " = "
		            + std::to_string( ( static_cast< long long >( pictureWidth ) * pictureHeight * 16
		                                * ( wantHeld ? 4 : 2 ) )
		                              / ( 1024 * 1024 ) )
		            + " MB of state" );
	}

	heldReady = wantHeld;
	return true;
}

//---------------------------------------------------------------------------
FFResult Plumbicon::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and ONLY that (SDK
	//b1afaf9, FFGLScopedFBOBinding.cpp). So every pass's ResizeViewPort()
	//leaks out into the pass after it, and the composite -- which draws to
	//the host's own framebuffer and so has no buffer of its own to size
	//itself from -- inherits whatever the last pass left. Here that would be
	//the quarter-size bloom buffer, and the effect would render into the
	//bottom-left quarter of the frame with the rest left transparent.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// What the controls say, with the tube type applied.
	//---------------------------------------------------------------------
	const int fieldMode = std::clamp( static_cast< int >( std::lround( params[ PT_FIELD_MODE ] ) ),
	                                  0, kFieldCount - 1 );
	const bool twoFields = fieldMode == kFieldTwoFields;

	const float photoGain = SensitivityFromParam( Effective( PT_SENSITIVITY ) );
	const float gamma     = TransferGammaFromParam( Effective( PT_GAMMA ) );
	const float dark      = DarkCurrentFromParam( Effective( PT_DARK ) );
	const float capacity  = CapacityFromParam( Effective( PT_CAPACITY ) );

	//Lag Amount and Beam Current multiply into ONE number, deliberately: they
	//are the same physical quantity reached from two directions. Beam Current
	//is what the tube is -- it is what Type moves -- and Lag Amount is how
	//much of it the operator wants today.
	const float beam = BeamCurrentFromParam( Effective( PT_BEAM ) )
	                   * LagScaleFromParam( Effective( PT_LAG ) );

	const float leak      = LeakFromParam( Effective( PT_RECOVERY ) );
	const float burnRise  = BurnRiseFromParam( Effective( PT_BURN_RATE ) );
	const float burnFall  = BurnFallFromParam( Effective( PT_BURN_RECOVERY ) );
	const float burnDepth = BurnDepthFromParam( Effective( PT_BURN_DEPTH ) );

	const float halation  = HalationFromParam( Effective( PT_HALATION ) );
	const float radius    = HalationRadiusFromParam( Effective( PT_HALATION_RADIUS ) );
	const float threshold = BloomThresholdFromParam( Effective( PT_BLOOM_THRESHOLD ) );
	const float halo      = HaloFromParam( Effective( PT_HALO ) );

	const float noise = NoiseFromParam( Effective( PT_NOISE ) );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding CLEARS
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and
	// the symptom is the dangerous part -- correct on every frame except the
	// one that allocates.
	//---------------------------------------------------------------------
	if( !EnsureBuffers( pictureWidth, pictureHeight, twoFields ) )
	{
		diag::error( "could not allocate the target at "
		             + std::to_string( pictureWidth ) + "x" + std::to_string( pictureHeight )
		             + " - RGBA32F state is 32 MB a buffer at 1080p and 133 MB at 4K" );
		return FF_FAIL;
	}

	const int next = 1 - current;

	//The field's parity, and the previous field's. In Frame mode both are -1,
	//which the shader reads as "every line".
	const int fieldParity = twoFields ? static_cast< int >( fieldIndex & 1u ) : -1;
	const int prevParity  = twoFields ? static_cast< int >( ( fieldIndex + 1u ) & 1u ) : -1;

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	const float halfTexelX        = 0.5f / static_cast< float >( pictureWidth );
	const float halfTexelY        = 0.5f / static_cast< float >( pictureHeight );

	//---------------------------------------------------------------------
	// 1. The target. One field: light in, charge stored, beam to come.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( state[ next ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		state[ next ].ResizeViewPort();
		ScopedShaderBinding shader( targetShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding sourceTexture( picture.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding stateTexture( state[ current ].TextureID() );

		targetShader.Set( "InputTexture", 0 );
		targetShader.Set( "StateTexture", 1 );
		targetShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		targetShader.Set( "HalfTexel", halfTexelX, halfTexelY );
		targetShader.Set( "PhotoGain", photoGain );
		targetShader.Set( "Gamma", gamma );
		targetShader.Set( "DarkCurrent", dark );
		targetShader.Set( "Capacity", capacity );
		targetShader.Set( "Beam", beam );
		targetShader.Set( "Leak", leak );
		targetShader.Set( "BurnRise", burnRise );
		targetShader.Set( "BurnFall", burnFall );
		targetShader.Set( "BurnDepth", burnDepth );
		glUniform1i( glGetUniformLocation( targetShader.GetGLID(), "PrevParity" ), prevParity );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The held signal. Two Fields only -- in Frame mode the beam visits
	//    every line every field, so the signal is min( charge, beam ) and
	//    there is nothing to hold.
	//---------------------------------------------------------------------
	if( twoFields )
	{
		ScopedFBOBinding fbo( held[ next ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		held[ next ].ResizeViewPort();
		ScopedShaderBinding shader( heldShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding stateTexture( state[ next ].TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding heldTexture( held[ current ].TextureID() );

		heldShader.Set( "StateTexture", 0 );
		heldShader.Set( "HeldTexture", 1 );
		heldShader.Set( "Beam", beam );
		glUniform1i( glGetUniformLocation( heldShader.GetGLID(), "FieldParity" ), fieldParity );
		quad.Draw();
	}

	//In Frame mode the held sampler is pointed at the state buffer. It is
	//never read -- UseHeld is zero -- but a sampler bound to texture 0 logs
	//"unit 0 is unloadable and bound to sampler type (Float)" once per
	//instance, from a plugin that is working perfectly.
	const GLuint signalState = state[ next ].TextureID();
	const GLuint signalHeld  = twoFields ? held[ next ].TextureID() : signalState;
	const int useHeld        = twoFields ? 1 : 0;

	//---------------------------------------------------------------------
	// 3-7. The optics. Skipped entirely when neither control asks for them,
	//      which is five passes off the frame.
	//---------------------------------------------------------------------
	if( halation > 0.0f || halo > 0.0f )
	{
		const float bloomWidth  = static_cast< float >( bloom[ 0 ].GetWidth() );
		const float bloomHeight = static_cast< float >( bloom[ 0 ].GetHeight() );

		{
			ScopedFBOBinding fbo( bloom[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			bloom[ 0 ].ResizeViewPort();
			ScopedShaderBinding shader( brightShader.GetGLID() );

			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding stateTexture( signalState );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding heldTexture( signalHeld );

			brightShader.Set( "StateTexture", 0 );
			brightShader.Set( "HeldTexture", 1 );
			brightShader.Set( "Beam", beam );
			brightShader.Set( "Threshold", threshold );
			brightShader.Set( "SourceTexel",
			                  1.0f / static_cast< float >( pictureWidth ),
			                  1.0f / static_cast< float >( pictureHeight ) );
			glUniform1i( glGetUniformLocation( brightShader.GetGLID(), "UseHeld" ), useHeld );
			quad.Draw();
		}

		//Narrow, then wide. The halation is the narrow one; the DIFFERENCE
		//between them is the Image Orthicon's halo ring, which is why the
		//two are separate buffers rather than one chain run further.
		struct Stage
		{
			PassBuffer* from;
			PassBuffer* to;
			float x, y;
		};
		const float stepX = radius / bloomWidth;
		const float stepY = radius / bloomHeight;
		const Stage stages[] = {
			{ &bloom[ 0 ], &bloom[ 1 ], stepX, 0.0f },
			{ &bloom[ 1 ], &narrow, 0.0f, stepY },
			{ &narrow, &bloom[ 1 ], stepX * kWideRatio, 0.0f },
			{ &bloom[ 1 ], &wide, 0.0f, stepY * kWideRatio },
		};

		for( const Stage& stage : stages )
		{
			ScopedFBOBinding fbo( stage.to->GetGLID(), ScopedFBOBinding::RB_REVERT );
			stage.to->ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding sourceTexture( stage.from->TextureID() );

			blurShader.Set( "SourceTexture", 0 );
			blurShader.Set( "Direction", stage.x, stage.y );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 8. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		//Back to the host's viewport. See the note where it was captured.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding sourceTexture( picture.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding stateTexture( signalState );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding heldTexture( signalHeld );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding narrowTexture( narrow.TextureID() );
		ScopedSamplerActivation sampler4( 4 );
		Scoped2DTextureBinding wideTexture( wide.TextureID() );

		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "StateTexture", 1 );
		compositeShader.Set( "HeldTexture", 2 );
		compositeShader.Set( "NarrowTexture", 3 );
		compositeShader.Set( "WideTexture", 4 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		compositeShader.Set( "HalfTexel", halfTexelX, halfTexelY );
		compositeShader.Set( "Beam", beam );
		compositeShader.Set( "Halation", halation );
		compositeShader.Set( "Halo", halo );
		compositeShader.Set( "Monochrome", Effective( PT_MONOCHROME ) );
		compositeShader.Set( "NoiseAmount", noise );
		compositeShader.Set( "MixAmount", Effective( PT_MIX ) );
		glUniform1i( glGetUniformLocation( compositeShader.GetGLID(), "UseHeld" ), useHeld );
		glUniform1ui( glGetUniformLocation( compositeShader.GetGLID(), "FieldIndex" ), fieldIndex );
		quad.Draw();
	}

	current = next;
	++fieldIndex;

	if( ++clockFrames == 60 )
		diag::info( "60 fields rendered; host clock raw=" + std::to_string( hostTime )
		            + " (unused: the model counts fields, not seconds)" );

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Plumbicon::DeInitGL()
{
	targetShader.FreeGLResources();
	heldShader.FreeGLResources();
	brightShader.FreeGLResources();
	blurShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	state[ 0 ].Destroy();
	state[ 1 ].Destroy();
	held[ 0 ].Destroy();
	held[ 1 ].Destroy();
	bloom[ 0 ].Destroy();
	bloom[ 1 ].Destroy();
	narrow.Destroy();
	wide.Destroy();

	stateWidth  = 0;
	stateHeight = 0;
	current     = 0;
	fieldIndex  = 0;
	heldReady   = false;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Plumbicon::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Plumbicon::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	//The SLIDER, not the effective value. A host asking what a parameter is
	//set to must get back what it set, or a saved composition reloads with
	//the tube type's values baked into the sliders.
	return params[ index ];
}

//---------------------------------------------------------------------------
char* Plumbicon::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Plumbicon::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Plumbicon::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
