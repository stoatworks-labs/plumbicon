/**
	pbtest -- render Plumbicon offline, and check what its target is doing.

	How long a saturated highlight takes to discharge, how long the tail
	behind a moving one is, and whether two differently over-exposed patches
	come out the same are **facts with closed forms**, not matters of taste.
	This harness drives the real plugin class in a headless CGL context and
	compares what comes back against those forms.

		pbtest --out /tmp/frame.png     a picture, on a moving test card
		pbtest --list                   every parameter, its type and range
		pbtest --lag                    a highlight switched off, against the
		                                exact recursion
		pbtest --comet                  the tail behind a moving highlight
		pbtest --capacity               the target saturates
		pbtest --burn                   the burn accumulator's two poles
		pbtest --passthrough            a beam above capacity is the identity
		pbtest --bench                  the render cost
		pbtest --card /tmp/card.png     the test card on its own
		pbtest --pipe                   raw frames in, raw frames out

	------------------------------------------------- what the checks rest on

	**Nothing here re-implements the model.** The GLSL in `Shaders.cpp` is what
	runs; these checks predict what it should produce from the SAME physical
	constants the plugin uploads, read out of `Controls.h` rather than typed in
	again. A check that carried its own copy of the model would agree with
	itself perfectly and prove nothing.

	**Every tolerance is derived, not fitted.** Each one is justified where it
	is declared, in terms of the arithmetic the GPU actually performs -- ULPs
	of a recursion, the GLSL specification's stated accuracy for `pow` -- and
	never in terms of the number this machine printed first. Four of six
	plugins in a previous round shipped checks calibrated to this Mac's GPU
	and failed on a GPU-less runner, and in all four cases the test was wrong
	rather than the plugin.

	**Every check that can depend on the raster runs at TWO rasters** and
	fails if they disagree. That is cheap and it is the only way to find out.

	------------------------------------------------------------- the clock

	There isn't one. The charge model advances one field per `ProcessOpenGL`
	call and never reads `SetTime` -- see `Controls.h`. `--fps` and the
	synthetic clock exist because the fleet's `--pipe` format has them and one
	filming script drives any plugin in it; here they are inert, and that is
	deliberate rather than an omission.

	`--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | pbtest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Plumbicon.h"
#include "Tubes.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace plumbicon;

namespace
{
//---------------------------------------------------------------------------
// Tolerances. Each one is derived where it is used; these are the primitives.
//---------------------------------------------------------------------------

/// One unit in the last place of a float near `value`. The tolerances below
/// are counted in these rather than written as decimals, so that moving a
/// constant in `Controls.cpp` moves the tolerance with it.
double ulpNear( double value )
{
	const float f = static_cast< float >( std::fabs( value ) );
	return static_cast< double >( std::nextafterf( f, 3.4e38f ) - f );
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card.
//
// Not meant to look nice. Each part of it exercises one control, and the
// whole thing MOVES, because a target that is looking at a still picture
// reaches a steady state in a few fields and then does nothing an operator
// can see:
//
//   - a bright disc on a Lissajous path: the comet tail, and the only thing
//     in the frame bright enough for Halation's bright pass to find
//   - a STATIC over-bright rectangle: burn-in, and blocked highlights. It
//     has to be static, because burn is the one thing here that is about a
//     picture NOT moving
//   - a bar sweeping horizontally: lag against a straight edge, which is
//     where it is readable
//   - six saturated colour bars: Monochrome, and per-channel lag -- a red
//     highlight lags in red and not in green, which is what makes a smear
//     change colour as it goes
//   - a smooth luminance ramp: Transfer Gamma and Target Capacity, both of
//     which need somewhere for a knee to appear
//   - a fine checkerboard: Noise, which is invisible against a flat field
//   - a dark surround: so Dark Current has somewhere to lift
//---------------------------------------------------------------------------
std::vector< float > buildCard( int width, int height, int frame )
{
	std::vector< float > card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	//Incommensurable rates, so the card never repeats within a take.
	const float discX = 0.5f + 0.34f * std::sin( t * 0.1100f );
	const float discY = 0.5f + 0.24f * std::sin( t * 0.0770f + 1.1f );
	const float discR = 0.050f;

	const float barX = 0.5f + 0.40f * std::sin( t * 0.0430f + 2.3f );
	const float barW = 0.018f;

	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//A dark, slightly graded surround.
			float r = 0.03f + 0.04f * v;
			float g = 0.03f + 0.04f * v;
			float b = 0.05f + 0.05f * v;

			//Six saturated bars across the bottom eighth.
			if( v < 0.125f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.05f, 0.05f }, { 0.05f, 1.0f, 0.05f }, { 0.05f, 0.05f, 1.0f },
					{ 0.05f, 1.0f, 1.0f }, { 1.0f, 0.05f, 1.0f }, { 1.0f, 1.0f, 0.05f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			//A smooth ramp above them: where the transfer curve's knee shows.
			else if( v < 0.25f )
			{
				r = g = b = u;
			}
			//A four-pixel checkerboard in the top-left: fine detail, so noise
			//and halation have something to sit against.
			else if( u < 0.22f && v > 0.76f )
			{
				const bool on = ( ( x / 4 ) + ( y / 4 ) ) % 2 == 0;
				r = g = b = on ? 0.80f : 0.06f;
			}

			//The STATIC over-bright plate, top right. Burn-in is the one
			//artefact here that is about something not moving, so it needs
			//something that does not move.
			if( u > 0.74f && u < 0.94f && v > 0.62f && v < 0.80f )
			{
				r = 1.0f;
				g = 0.97f;
				b = 0.88f;
			}

			//The sweeping bar. Straight edges on both sides, so lag on a
			//moving edge is readable rather than merely present.
			if( std::fabs( u - barX ) < barW && v > 0.28f && v < 0.60f )
			{
				r = 0.90f;
				g = 0.95f;
				b = 1.00f;
			}

			//The bright disc, with a soft shoulder so it does not alias as it
			//travels. Measured in picture WIDTHS on both axes.
			const float dx   = u - discX;
			const float dy   = ( v - discY ) / aspect;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.22f ) );
				r                = r + ( 1.0f - r ) * edge;
				g                = g + ( 1.0f - g ) * edge;
				b                = b + ( 1.0f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = std::clamp( r, 0.0f, 1.0f );
			card[ i + 1 ]  = std::clamp( g, 0.0f, 1.0f );
			card[ i + 2 ]  = std::clamp( b, 0.0f, 1.0f );
			card[ i + 3 ]  = 1.0f;
		}
	}

	return card;
}

/// PCG output mix, the same one the shader uses, for the sweep's noise.
uint32_t hashInt( uint32_t seed )
{
	uint32_t state = seed * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

void addNoise( std::vector< float >& card, int frame, float amount )
{
	if( amount <= 0.0f )
		return;

	for( size_t i = 0; i < card.size(); i += 4 )
	{
		const uint32_t seed = hashInt( static_cast< uint32_t >( i / 4 ) * 2654435761u
		                               ^ static_cast< uint32_t >( frame ) );
		const float jitter  = ( static_cast< float >( hashInt( seed ) ) * 2.3283064365386963e-10f - 0.5f )
		                     * amount;
		for( int c = 0; c < 3; ++c )
			card[ i + c ] = std::clamp( card[ i + c ] + jitter, 0.0f, 1.0f );
	}
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

/// A float source texture.
///
/// RGBA32F and not RGBA8, on purpose: a check that predicts an output to
/// within a few ULPs cannot have its INPUT quantised to a 256th first. A host
/// hands over 8-bit; the arithmetic this is checking does not care which, and
/// the picture paths below use the same function.
GLuint makeFloatTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//NEAREST: the plugin samples this texel for texel and must get the texel.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< float > readBackFloat( GLuint fbo, int width, int height )
{
	std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > toBytes( const std::vector< float >& image )
{
	std::vector< unsigned char > bytes( image.size() );
	for( size_t i = 0; i < image.size(); ++i )
		bytes[ i ] = static_cast< unsigned char >( std::clamp( image[ i ], 0.0f, 1.0f ) * 255.0f + 0.5f );
	return bytes;
}

std::vector< float > flipRowsF( const std::vector< float >& image, int width, int height )
{
	std::vector< float > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride,
		             stride * sizeof( float ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
const char* typeName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_BOOLEAN: return "boolean";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_TEXT: return "text";
	default: return "other";
	}
}

unsigned int findParameter( Plumbicon& plugin, const std::string& name )
{
	for( unsigned int i = 0; i < Plumbicon::PT_COUNT; ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
			return i;
	}
	return Plumbicon::PT_COUNT;
}

bool applySetting( Plumbicon& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name   = assignment.substr( 0, equals );
	const std::string value  = assignment.substr( equals + 1 );
	const unsigned int index = findParameter( plugin, name );
	if( index < Plumbicon::PT_COUNT )
	{
		plugin.SetFloatParameter( index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}
	error = "no parameter called '" + name + "'";
	return false;
}

int listParameters()
{
	//No GL. The constructor touches nothing but memory and a log file, which
	//is what lets this run on a machine -- or a CI runner -- where creating a
	//context fails outright.
	Plumbicon plugin;
	std::printf( "%-3s %-18s %-9s %10s %9s %9s\n", "id", "name", "type", "default", "min", "max" );
	for( unsigned int i = 0; i < Plumbicon::PT_COUNT; ++i )
	{
		const char* name        = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );
		RangeStruct range       = plugin.GetParamRange( i );
		if( type == FF_TYPE_OPTION )
			range = { 0.0f, static_cast< float >( plugin.GetNumParamElements( i ) ) - 1.0f };
		std::printf( "%-3u %-18s %-9s %10.4f %9.4f %9.4f\n", i, name ? name : "?", typeName( type ),
		             plugin.GetFloatParameter( i ), range.min, range.max );
	}
	return 0;
}

//===========================================================================
// The rig: one plugin instance, one float source, one float target.
//===========================================================================
class Rig
{
public:
	Rig( int width, int height ) :
		width( width ), height( height )
	{
		source = makeFloatTexture( width, height, nullptr );
		output = makeFloatTexture( width, height, nullptr );
		fbo    = makeFramebuffer( output );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = source;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = fbo;
	}

	~Rig()
	{
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &output );
		glDeleteTextures( 1, &source );
	}

	bool init( const std::vector< std::string >& settings, std::string& error )
	{
		for( const std::string& setting : settings )
			if( !applySetting( plugin, setting, error ) )
				return false;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			error = "InitGL failed -- see the diagnostics log for which shader";
			return false;
		}
		return true;
	}

	bool frame( const std::vector< float >& picture )
	{
		glBindTexture( GL_TEXTURE_2D, source );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		//SetTime is driven even though the model ignores it, because a host
		//drives it and the plugin must be exercised the way a host does.
		plugin.SetTime( static_cast< double >( fields ) / 60.0 );
		++fields;
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	std::vector< float > read() const
	{
		return readBackFloat( fbo, width, height );
	}

	/// Drive a parameter mid-take, the way an operator's slider would.
	void set( unsigned int index, float value )
	{
		plugin.SetFloatParameter( index, value );
	}

	/// The value at (x, y) in readback order: row 0 is the BOTTOM row, which
	/// is also gl_FragCoord.y = 0.5. The two agree, which is what lets the
	/// field-parity check say which line the beam was on.
	float at( const std::vector< float >& image, int x, int y, int channel ) const
	{
		return image[ ( static_cast< size_t >( y ) * width + x ) * 4 + channel ];
	}

	int width, height;

private:
	Plumbicon plugin;
	GLuint source = 0, output = 0, fbo = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = {};
	ProcessOpenGLStruct process    = {};
	int fields                     = 0;
};

/// A flat field of one colour.
std::vector< float > flat( int width, int height, float level )
{
	std::vector< float > picture( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < picture.size(); i += 4 )
	{
		picture[ i + 0 ] = picture[ i + 1 ] = picture[ i + 2 ] = level;
		picture[ i + 3 ]                                       = 1.0f;
	}
	return picture;
}

//===========================================================================
// The physical constants a set of slider positions means.
//
// Read out of Controls.h, NEVER typed in again. The plugin uploads these
// exact floats, so a prediction built from them is a prediction about the
// arithmetic the GPU performs rather than about a number that was rounded
// twice on the way here.
//===========================================================================
struct Physics
{
	float sensitivity, capacity, beam, gamma, dark, leak;
	float burnRise, burnFall, burnDepth;
};

Physics physicsOf( float sensitivity, float capacity, float beamParam, float lagParam,
                   float gammaParam, float darkParam, float leakParam,
                   float riseParam, float fallParam, float depthParam )
{
	Physics p;
	p.sensitivity = SensitivityFromParam( sensitivity );
	p.capacity    = CapacityFromParam( capacity );
	p.beam        = BeamCurrentFromParam( beamParam ) * LagScaleFromParam( lagParam );
	p.gamma       = TransferGammaFromParam( gammaParam );
	p.dark        = DarkCurrentFromParam( darkParam );
	p.leak        = LeakFromParam( leakParam );
	p.burnRise    = BurnRiseFromParam( riseParam );
	p.burnFall    = BurnFallFromParam( fallParam );
	p.burnDepth   = BurnDepthFromParam( depthParam );
	return p;
}

/// Everything off but the charge store: no burn bias, no optics, no noise,
/// full wet. Each check adds the two or three settings it is actually about.
std::vector< std::string > quiet()
{
	return {
		"Type=0",//Custom: no tube pins anything
		"Field Mode=0",
		"Burn Depth=0",
		"Halation=0",
		"Halo=0",
		"Monochrome=0",
		"Noise=0",
		"Mix=1",
	};
}

//===========================================================================
// --lag
//===========================================================================
/**
	A highlight switched off decays exactly as the recursion predicts.

	With no light, no dark current and no leakage, one field of the model is

	    charge <- charge - min( charge, beam )

	and nothing else. That recursion is **linear, not exponential**: from a
	saturated target the charge is

	    charge( f ) = max( 0, capacity - f * beam )

	and the signal the beam takes is `min( charge( f ), beam )`. So the tail
	is flat at full amplitude for `floor( capacity / beam ) - 1` fields, has
	one partial field, and then stops DEAD. An exponential never stops, and a
	check written against the textbook 2% settling time would be measuring a
	fit rather than the model -- that is galvo's lesson, applied before it
	could cost anything here.

	**Tolerance.** The GPU evaluates `charge - min( charge, beam )` in float32,
	once per field. `min` is exact; the subtraction rounds at most half a ULP
	of the larger operand, and the largest operand is the capacity. Over `f`
	fields the accumulated error is bounded by `f * ulp( capacity ) / 2`. The
	tolerance is **eight times** that bound -- generous against rounding, and
	still four orders of magnitude tighter than any wrong constant could hide
	in, since a wrong beam current misses by a whole field of signal.

	**Raster.** Nothing in this check refers to a pixel: the source is a flat
	field, so every pixel of every raster holds the same number. It is run at
	two rasters and the two must agree, which is how that claim is made rather
	than assumed.
*/
int runLagCheck()
{
	//Chosen so that capacity / beam is a little over five: long enough that
	//the flat part of the tail is several fields, short enough to watch it
	//stop.
	const std::vector< std::string > sliders = {
		"Sensitivity=0.5",     //2.0 charge per field at white -- above capacity
		"Target Capacity=0.30",//1.828
		"Beam Current=0.5",    //0.3464
		"Lag Amount=0",        //exactly 1.0, so the beam is the beam
		"Transfer Gamma=0.5",  //exactly 1.0
		"Dark Current=0",      //exactly 0
		"Recovery=0",          //exactly 0: the PURE recursion
	};
	const Physics p = physicsOf( 0.5f, 0.30f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f );

	constexpr int kCharge = 12;///< fields of white; two would do
	constexpr int kDark   = 10;///< fields to watch it go

	const double tolerance = 8.0 * kDark * ulpNear( p.capacity ) * 0.5;

	std::printf( "  capacity %.6f, beam %.6f -> %.3f fields to discharge\n",
	             p.capacity, p.beam, p.capacity / p.beam );
	std::printf( "  tolerance %.3g = 8 x %d fields x half a ULP of the capacity\n",
	             tolerance, kDark );

	struct Size
	{
		int w, h;
	};
	const Size rasters[] = { { 64, 36 }, { 320, 180 } };

	int failures = 0;
	std::vector< std::vector< double > > measured;

	for( const Size& size : rasters )
	{
		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );

		Rig rig( size.w, size.h );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "lag: %s\n", error.c_str() );
			return 1;
		}

		const std::vector< float > white = flat( size.w, size.h, 1.0f );
		const std::vector< float > black = flat( size.w, size.h, 0.0f );

		for( int i = 0; i < kCharge; ++i )
			if( !rig.frame( white ) )
			{
				std::fprintf( stderr, "lag: ProcessOpenGL failed\n" );
				return 1;
			}

		std::vector< double > row;
		for( int f = 1; f <= kDark; ++f )
		{
			if( !rig.frame( black ) )
			{
				std::fprintf( stderr, "lag: ProcessOpenGL failed\n" );
				return 1;
			}
			const std::vector< float > image = rig.read();
			//Four widely separated pixels, so a check that only looked at one
			//corner cannot pass on a plugin that only works in one corner.
			double worst = 0.0;
			const double first = rig.at( image, 1, 1, 0 );
			const int probes[ 4 ][ 2 ] = { { 1, 1 },
			                               { size.w - 2, 1 },
			                               { 1, size.h - 2 },
			                               { size.w / 2, size.h / 2 } };
			for( const auto& probe : probes )
				worst = std::max( worst, std::fabs( rig.at( image, probe[ 0 ], probe[ 1 ], 0 ) - first ) );
			if( worst > tolerance )
			{
				std::printf( "  %dx%d field %d: the frame is not flat (%.3g across four probes)  FAILED\n",
				             size.w, size.h, f, worst );
				++failures;
			}
			row.push_back( first );
		}
		measured.push_back( row );
	}

	//The closed form, and the measurement, side by side.
	std::printf( "\n  field    predicted      %dx%d      %dx%d\n",
	             rasters[ 0 ].w, rasters[ 0 ].h, rasters[ 1 ].w, rasters[ 1 ].h );
	for( int f = 1; f <= kDark; ++f )
	{
		const double charge    = std::max( 0.0, double( p.capacity ) - double( f ) * double( p.beam ) );
		const double predicted = std::min( charge, double( p.beam ) );

		const double a  = measured[ 0 ][ static_cast< size_t >( f - 1 ) ];
		const double b  = measured[ 1 ][ static_cast< size_t >( f - 1 ) ];
		const bool okA  = std::fabs( a - predicted ) <= tolerance;
		const bool okB  = std::fabs( b - predicted ) <= tolerance;
		const bool same = std::fabs( a - b ) <= tolerance;

		std::printf( "  %5d  %11.7f  %11.7f  %11.7f  %s\n", f, predicted, a, b,
		             ( okA && okB && same ) ? "ok" : "FAILED" );
		if( !( okA && okB && same ) )
			++failures;
	}

	//And the headline: the tail STOPS, at the field the ratio names, and not
	//before it. An exponential does neither -- it is never zero and it has no
	//distinguished field. Both halves are asserted, because "it reached zero"
	//alone would also be true of a tail that stopped three fields early.
	const int fields = static_cast< int >( std::ceil( double( p.capacity ) / double( p.beam ) ) );
	bool stopped     = true;
	for( const std::vector< double >& row : measured )
	{
		stopped = stopped && row[ static_cast< size_t >( fields ) - 1 ] == 0.0
		          && row[ static_cast< size_t >( fields ) - 2 ] > 0.0;
	}
	std::printf( "\n  ceil( capacity / beam ) = %d: field %d still carries signal and field %d is\n"
	             "  EXACTLY zero -- the tail stops dead, which no exponential does: %s\n",
	             fields, fields - 1, fields, stopped ? "ok" : "FAILED" );
	if( !stopped )
		++failures;

	//-------------------------------------------------------------------
	// Two Fields: the beam visits a line every OTHER field, so that line's
	// tail lasts twice as long. Checked against an exact simulation of the
	// same recursion with the parity in it -- the algebraic form above does
	// not survive the alternation, and inventing one that "nearly" does is
	// how a check stops being a check.
	//
	// Two ADJACENT rows, and the test does not say which parity is which:
	// one of them must discharge on even fields and the other on odd, and
	// which is which depends on the raster's origin, not on the physics.
	//-------------------------------------------------------------------
	{
		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );
		for( std::string& s : settings )
			if( s == "Field Mode=0" )
				s = "Field Mode=1";

		Rig rig( 64, 36 );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "lag: %s\n", error.c_str() );
			return 1;
		}

		const std::vector< float > white = flat( 64, 36, 1.0f );
		const std::vector< float > black = flat( 64, 36, 0.0f );

		std::vector< double > rowA, rowB;
		for( int i = 0; i < kCharge; ++i )
			rig.frame( white );
		for( int f = 0; f < kDark * 2; ++f )
		{
			rig.frame( black );
			const std::vector< float > image = rig.read();
			rowA.push_back( rig.at( image, 10, 10, 0 ) );
			rowB.push_back( rig.at( image, 10, 11, 0 ) );
		}

		//The mirror. Field n's beam pass uses parity n & 1; the field AFTER
		//it undoes that pass, which is why the plugin carries both parities
		//as uniforms.
		auto simulate = [ & ]( int lineParity ) {
			std::vector< double > out;
			double charge = 0.0, heldSignal = 0.0;
			for( int n = 0; n < kCharge + kDark * 2; ++n )
			{
				const double light   = n < kCharge ? 1.0 : 0.0;
				const int parity     = n & 1;
				const int prevParity = ( n + 1 ) & 1;
				const double prevBeam = ( lineParity == prevParity ) ? double( p.beam ) : 0.0;

				double residue = charge - std::min( charge, prevBeam );
				residue -= residue * double( p.leak );
				charge = std::min( residue + double( p.sensitivity ) * light, double( p.capacity ) );
				if( lineParity == parity )
					heldSignal = std::min( charge, double( p.beam ) );
				if( n >= kCharge )
					out.push_back( heldSignal );
			}
			return out;
		};

		const std::vector< double > even = simulate( 0 );
		const std::vector< double > odd  = simulate( 1 );

		auto worstAgainst = []( const std::vector< double >& got, const std::vector< double >& want ) {
			double worst = 0.0;
			for( size_t i = 0; i < got.size(); ++i )
				worst = std::max( worst, std::fabs( got[ i ] - want[ i ] ) );
			return worst;
		};

		//Either assignment of the two rows to the two parities, whichever the
		//raster's origin gives.
		const double straight = std::max( worstAgainst( rowA, even ), worstAgainst( rowB, odd ) );
		const double swapped  = std::max( worstAgainst( rowA, odd ), worstAgainst( rowB, even ) );
		const double best     = std::min( straight, swapped );
		const double twoFieldTolerance = 8.0 * kDark * 2 * ulpNear( p.capacity ) * 0.5;

		std::printf( "  two fields: adjacent rows follow the alternating recursion to %.3g "
		             "(tolerance %.3g)  %s\n",
		             best, twoFieldTolerance, best <= twoFieldTolerance ? "ok" : "FAILED" );
		if( best > twoFieldTolerance )
			++failures;

		//And the point of the mode: the tail is twice as long.
		auto lastLit = []( const std::vector< double >& v, double level ) {
			int n = 0;
			for( size_t i = 0; i < v.size(); ++i )
				if( v[ i ] > level )
					n = static_cast< int >( i ) + 1;
			return n;
		};
		const int twoFieldLength = lastLit( rowA, 0.0 );
		std::printf( "  two fields: the tail runs %d fields against %d in Frame mode  %s\n",
		             twoFieldLength, fields,
		             twoFieldLength >= 2 * fields - 1 ? "ok" : "FAILED" );
		if( twoFieldLength < 2 * fields - 1 )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "lag: ok" : "lag: FAILED" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --comet
//===========================================================================
/**
	A highlight translating at v pixels a field leaves a tail of the length
	the discharge recursion predicts, and **nothing draws it**.

	A pixel the highlight has left behind has had `f` dark reads, where
	`f = ceil( d / v )` for a pixel `d` pixels behind the trailing edge. Its
	signal is the same `min( max( 0, capacity - f * beam ), beam )` the lag
	check just verified. So the tail, measured as the run of pixels still
	reading at least half a full signal, is

	    v * floor( capacity / beam - 0.5 )

	pixels long, for integer `v` and integer pixel positions.

	**Tolerance: one pixel**, as the spec asks -- but the check also asserts
	that the last lit pixel and the first dark one are each at least 0.15 of a
	full signal clear of the half-signal threshold. Without that, a tail
	measured at a level the signal happens to cross near a pixel boundary
	would be a coin flip dressed up as a measurement, and the right response
	is to re-choose the constants rather than to widen the tolerance.

	**Raster.** The prediction is in PIXELS and the motion is in PIXELS, so
	the only thing the raster decides is whether the tail fits. That
	requirement is stated in the code and checked, and the whole thing runs at
	two widths.
*/
int runCometCheck()
{
	const std::vector< std::string > sliders = {
		"Sensitivity=0.5", "Target Capacity=0.30", "Beam Current=0.5",
		"Lag Amount=0", "Transfer Gamma=0.5", "Dark Current=0", "Recovery=0",
	};
	const Physics p = physicsOf( 0.5f, 0.30f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f );

	constexpr int kVelocity = 4; ///< pixels a field
	constexpr int kPatch    = 64;///< pixels wide; 16 fields of exposure at v=4
	constexpr int kStart    = 16;
	constexpr int kFields   = 24;

	const double ratio    = double( p.capacity ) / double( p.beam );
	const int tailFields  = static_cast< int >( std::floor( ratio - 0.5 ) );
	const int predicted   = kVelocity * tailFields;
	const double half     = 0.5 * double( p.beam );

	//The margins. A tail measured at a threshold the signal crosses near a
	//pixel boundary is a coin flip, not a measurement.
	const double atLast  = std::min( double( p.capacity ) - double( tailFields ) * double( p.beam ),
	                                 double( p.beam ) );
	const double atNext  = std::max( 0.0, std::min( double( p.capacity ) - double( tailFields + 1 ) * double( p.beam ),
	                                                double( p.beam ) ) );
	const double marginA = ( atLast - half ) / double( p.beam );
	const double marginB = ( half - atNext ) / double( p.beam );

	std::printf( "  v = %d px/field, patch %d px, capacity/beam = %.3f\n", kVelocity, kPatch, ratio );
	std::printf( "  predicted tail %d px = %d px/field x %d fields above half a signal\n",
	             predicted, kVelocity, tailFields );
	std::printf( "  margins at the threshold: %+.3f and %+.3f of a full signal (need > 0.15)\n",
	             marginA, marginB );

	int failures = 0;
	if( marginA < 0.15 || marginB < 0.15 )
	{
		std::printf( "  the constants put the tail's end too close to the threshold  FAILED\n" );
		++failures;
	}

	struct Size
	{
		int w, h;
	};
	const Size rasters[] = { { 320, 64 }, { 1280, 128 } };

	for( const Size& size : rasters )
	{
		const int trailing = kStart + kVelocity * kFields;
		if( trailing + kPatch > size.w || trailing - predicted - 2 < 0 )
		{
			std::printf( "  %dx%d is too small to hold the patch and its tail  FAILED\n", size.w, size.h );
			++failures;
			continue;
		}

		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );

		Rig rig( size.w, size.h );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "comet: %s\n", error.c_str() );
			return 1;
		}

		for( int t = 0; t <= kFields; ++t )
		{
			std::vector< float > picture( static_cast< size_t >( size.w ) * size.h * 4, 0.0f );
			for( size_t i = 3; i < picture.size(); i += 4 )
				picture[ i ] = 1.0f;

			const int x0 = kStart + kVelocity * t;
			for( int y = 0; y < size.h; ++y )
				for( int x = x0; x < x0 + kPatch && x < size.w; ++x )
				{
					const size_t i   = ( static_cast< size_t >( y ) * size.w + x ) * 4;
					picture[ i + 0 ] = picture[ i + 1 ] = picture[ i + 2 ] = 1.0f;
				}

			if( !rig.frame( picture ) )
			{
				std::fprintf( stderr, "comet: ProcessOpenGL failed\n" );
				return 1;
			}
		}

		const std::vector< float > image = rig.read();
		const int row                    = size.h / 2;

		int tail = 0;
		for( int d = 1; d <= trailing; ++d )
		{
			if( rig.at( image, trailing - d, row, 0 ) < half )
				break;
			tail = d;
		}

		//And the pixel one past the end really is dark, rather than the scan
		//having stopped for some other reason.
		const double beyond = rig.at( image, trailing - tail - 1, row, 0 );
		const bool ok       = std::abs( tail - predicted ) <= 1 && beyond < half;

		std::printf( "  %4dx%-4d tail %3d px against %3d predicted, next pixel %.5f  %s\n",
		             size.w, size.h, tail, predicted, beyond, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "comet: ok" : "comet: FAILED" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --capacity
//===========================================================================
/**
	The target saturates, and past that more light is not more charge.

	Two patches, one ten times past capacity and one a hundred times past it,
	must come out at **exactly** the same value -- and that value must be the
	capacity, not merely equal to each other.

	**Tolerance: zero, bitwise, and that is a claim about the arithmetic
	rather than a hope.** The only place the two patches differ is the value
	handed to `min( residue + photo, capacity )`, and GLSL's `min` is exact:
	when the first argument exceeds the second it returns the second
	unchanged, whatever the first was. Everything downstream of that clamp
	then runs identical operations on identical bits. A pass-through, by
	contrast, does NOT get to claim bitwise equality, and this file says why
	at `--passthrough`.

	A third patch, well BELOW capacity, is checked to come out different.
	Without it a plugin that emitted a constant would pass this check with
	full marks.
*/
int runCapacityCheck()
{
	const std::vector< std::string > sliders = {
		"Sensitivity=1",       //4.0 charge per field at white: the most there is
		"Target Capacity=0",   //0.04: the least there is
		"Beam Current=0.5",    //0.3464, comfortably above capacity
		"Lag Amount=0", "Transfer Gamma=0.5", "Dark Current=0", "Recovery=0",
	};
	const Physics p = physicsOf( 1.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f );

	//Light levels: 10x and 100x capacity, and one at a quarter of it.
	const float ten     = 10.0f * p.capacity / p.sensitivity;
	const float hundred = 100.0f * p.capacity / p.sensitivity;
	const float under   = 0.25f * p.capacity / p.sensitivity;

	std::printf( "  capacity %.6f, beam %.6f, sensitivity %.3f\n", p.capacity, p.beam, p.sensitivity );
	std::printf( "  patches at L = %.4f (10x), %.4f (100x) and %.6f (0.25x)\n", ten, hundred, under );

	if( hundred > 1.0f )
	{
		std::printf( "  a hundred times capacity is past white; the constants do not reach it  FAILED\n" );
		return 1;
	}

	struct Size
	{
		int w, h;
	};
	const Size rasters[] = { { 96, 48 }, { 384, 216 } };

	int failures = 0;
	for( const Size& size : rasters )
	{
		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );

		Rig rig( size.w, size.h );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "capacity: %s\n", error.c_str() );
			return 1;
		}

		std::vector< float > picture( static_cast< size_t >( size.w ) * size.h * 4, 0.0f );
		for( int y = 0; y < size.h; ++y )
			for( int x = 0; x < size.w; ++x )
			{
				const size_t i   = ( static_cast< size_t >( y ) * size.w + x ) * 4;
				const int third  = x * 3 / size.w;
				const float level = third == 0 ? ten : ( third == 1 ? hundred : under );
				picture[ i + 0 ] = picture[ i + 1 ] = picture[ i + 2 ] = level;
				picture[ i + 3 ]                                       = 1.0f;
			}

		for( int i = 0; i < 8; ++i )
			if( !rig.frame( picture ) )
			{
				std::fprintf( stderr, "capacity: ProcessOpenGL failed\n" );
				return 1;
			}

		const std::vector< float > image = rig.read();
		const int row                    = size.h / 2;
		const float a                    = rig.at( image, size.w / 6, row, 0 );
		const float b                    = rig.at( image, size.w / 2, row, 0 );
		const float c                    = rig.at( image, size.w * 5 / 6, row, 0 );

		const bool equal    = a == b;//bitwise: see the note above
		const bool isCap    = a == p.capacity;
		const bool notConst = c < p.capacity * 0.5f;

		std::printf( "  %3dx%-3d  10x %.9g   100x %.9g   0.25x %.9g\n", size.w, size.h, a, b, c );
		std::printf( "           equal bitwise: %s   equals the capacity: %s   "
		             "the low patch differs: %s\n",
		             equal ? "yes" : "NO", isCap ? "yes" : "NO", notConst ? "yes" : "NO" );
		if( !( equal && isCap && notConst ) )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "capacity: ok" : "capacity: FAILED" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --burn
//===========================================================================
/**
	The burn accumulator rises on one pole and falls on another, and the
	sensitivity deficit it causes follows both.

	The accumulator is `burn <- burn + ( drive - burn ) * pole`, whose closed
	form for a constant drive D from burn0 is

	    burn( n ) = D + ( burn0 - D ) * ( 1 - pole )^n

	With Sensitivity at exactly 1.0, Burn Depth at 1.0, a beam above capacity
	and a white field, the plugin's output IS `1 - burn`, so the accumulator
	can be read straight off the picture without inferring anything.

	Recovery is measured by taking the light away for a while and then putting
	it back for ONE field: the output of that field uses the burn from the end
	of the dark period, because the target pass applies the burn it inherited
	before updating it.

	**Tolerance.** Each field is a subtract, a multiply and an add in float32,
	any pair of which the compiler may contract into an FMA -- so at most
	about two ULPs of unity per field, and the error is a random walk that the
	bound treats as if it were not. Over `n` fields the tolerance is
	`4 * n * FLT_EPSILON`, which at 700 fields is 3.3e-4 against a quantity
	of order 1. A wrong pole would miss by percent.

	**Raster.** Per pixel, and run at two rasters.
*/
int runBurnCheck()
{
	const std::vector< std::string > sliders = {
		"Sensitivity=0.25",    //exactly 1.0
		"Target Capacity=0.5", //3.02, well above anything here
		"Beam Current=1",      //6.0, above capacity: no lag at all
		"Lag Amount=0", "Transfer Gamma=0.5", "Dark Current=0", "Recovery=0",
		"Burn Rate=1",         //the fastest pole there is
		"Burn Recovery=1",
	};
	const Physics p = physicsOf( 0.25f, 0.5f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f );

	//The drive a white field produces: the luma of (1,1,1), summed the way
	//the shader's dot() sums it.
	const double drive = double( 0.2126f ) + double( 0.7152f ) + double( 0.0722f );

	constexpr int kRise    = 400;
	constexpr int kRecover = 300;
	const int samples[]    = { 1, 25, 50, 100, 200, 400 };

	std::printf( "  rise pole %.3g (tau %.0f fields), fall pole %.3g (tau %.0f fields)\n",
	             p.burnRise, 1.0 / p.burnRise, p.burnFall, 1.0 / p.burnFall );

	struct Size
	{
		int w, h;
	};
	const Size rasters[] = { { 32, 18 }, { 160, 90 } };

	int failures = 0;
	for( const Size& size : rasters )
	{
		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );
		for( std::string& s : settings )
			if( s == "Burn Depth=0" )
				s = "Burn Depth=1";

		Rig rig( size.w, size.h );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "burn: %s\n", error.c_str() );
			return 1;
		}

		const std::vector< float > white = flat( size.w, size.h, 1.0f );
		const std::vector< float > black = flat( size.w, size.h, 0.0f );

		size_t nextSample = 0;
		for( int n = 1; n <= kRise; ++n )
		{
			if( !rig.frame( white ) )
			{
				std::fprintf( stderr, "burn: ProcessOpenGL failed\n" );
				return 1;
			}
			if( nextSample >= sizeof( samples ) / sizeof( samples[ 0 ] ) || n != samples[ nextSample ] )
				continue;
			++nextSample;

			//The output of field n uses the burn from the END of field n-1.
			const double burn      = drive * ( 1.0 - std::pow( 1.0 - double( p.burnRise ), n - 1 ) );
			const double predicted = 1.0 - burn;
			const double got       = rig.at( rig.read(), size.w / 2, size.h / 2, 0 );
			const double tolerance = 4.0 * n * 1.1920929e-7;
			const bool ok          = std::fabs( got - predicted ) <= tolerance;

			std::printf( "  %3dx%-3d rise  field %4d: %.7f against %.7f (tol %.2g)  %s\n",
			             size.w, size.h, n, got, predicted, tolerance, ok ? "ok" : "FAILED" );
			if( !ok )
				++failures;
		}

		const double burnAtEnd = drive * ( 1.0 - std::pow( 1.0 - double( p.burnRise ), kRise ) );

		for( int n = 0; n < kRecover; ++n )
			rig.frame( black );

		rig.frame( white );
		const double recovered = burnAtEnd * std::pow( 1.0 - double( p.burnFall ), kRecover );
		const double predicted = 1.0 - recovered;
		const double got       = rig.at( rig.read(), size.w / 2, size.h / 2, 0 );
		const double tolerance = 4.0 * ( kRise + kRecover ) * 1.1920929e-7;
		const bool ok          = std::fabs( got - predicted ) <= tolerance;

		std::printf( "  %3dx%-3d fall  after %4d dark fields: %.7f against %.7f (tol %.2g)  %s\n",
		             size.w, size.h, kRecover, got, predicted, tolerance, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "burn: ok" : "burn: FAILED" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --passthrough
//===========================================================================
/**
	With the beam above capacity and everything else at its null, the plugin
	is the identity on its input.

	**The tolerance, and why it is NOT one ULP.**

	Every operation in the path is exact by construction, and each one is
	exact for a reason rather than by luck:

	    residue  = 0 - 0 * leak            leak is exactly 0 at the null,
	                                       and the state starts cleared
	    sens     = gain * ( 1 - 0 * burn ) Burn Depth is exactly 0
	    photo    = sens * lit + 0          Dark Current is exactly 0, and
	                                       Sensitivity is exactly 1.0
	    charge   = min( 0 + photo, K )     K exceeds anything in 0..1
	    signal   = min( charge, beam )     beam exceeds K
	    out      = mix( src, signal, 1 )   x*(1-1) + y*1 = y

	and then there is `pow( light, 1.0 )`, which is **not** the identity on
	any GPU. GLSL 4.10 specifies `pow` as inherited from `exp2`, `log2` and a
	multiply: 3 ULP each outside their near ranges. For light as low as 1/256,
	`log2` returns -8, where 3 ULP is about 2.9e-6 absolute; carried through
	`exp2` that is `ln2 * 2.9e-6` relative, about 2.0e-6, plus `exp2`'s own 3
	ULP. The bound is therefore about **2.4e-6 relative**, and the tolerance
	is **8e-6 absolute** -- a little over three times the specified bound, and
	nowhere near loose enough to hide a wrong constant, which would miss by
	percent.

	Claiming one ULP here would be claiming something the specification does
	not give, and that is exactly the shape of the mistake that broke vocoder:
	an FMA contracted on one side of a cancellation moves the last bit, and a
	check written to the bit fails on the next driver. The honest answer is to
	derive the bound and say so.

	The check prints the worst error and where it was, so a failure is
	diagnosable rather than merely red.
*/
int runPassthroughCheck()
{
	const std::vector< std::string > sliders = {
		"Sensitivity=0.25",    //exactly 1.0
		"Target Capacity=0.2", //1.232, above anything in 0..1
		"Beam Current=1",      //6.0, above the capacity
		"Lag Amount=0",        //exactly 1.0
		"Transfer Gamma=0.5",  //exactly 1.0
		"Dark Current=0",      //exactly 0
		"Recovery=0",          //exactly 0
	};
	const Physics p = physicsOf( 0.25f, 0.2f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f );

	constexpr double kTolerance = 8.0e-6;

	std::printf( "  sensitivity %.9g (want exactly 1), gamma %.9g (want exactly 1)\n",
	             p.sensitivity, p.gamma );
	std::printf( "  beam %.6f >= capacity %.6f >= 1: the beam empties the target every field\n",
	             p.beam, p.capacity );
	std::printf( "  tolerance %.3g absolute, from the GLSL spec's accuracy for pow -- NOT one ULP\n",
	             kTolerance );

	int failures = 0;
	if( p.sensitivity != 1.0f || p.gamma != 1.0f || p.dark != 0.0f || p.leak != 0.0f )
	{
		std::printf( "  the null settings are not exact -- a Controls.cpp range has moved  FAILED\n" );
		++failures;
	}

	struct Size
	{
		int w, h;
	};
	const Size rasters[] = { { 96, 64 }, { 480, 270 } };

	for( const Size& size : rasters )
	{
		std::vector< std::string > settings = quiet();
		settings.insert( settings.end(), sliders.begin(), sliders.end() );

		Rig rig( size.w, size.h );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "passthrough: %s\n", error.c_str() );
			return 1;
		}

		//Every level from 0 to 1, plus an eighth-of-a-step offset so the
		//values are not all binary fractions -- pow is at its worst on
		//arguments whose log2 is not tidy.
		std::vector< float > picture( static_cast< size_t >( size.w ) * size.h * 4 );
		for( int y = 0; y < size.h; ++y )
			for( int x = 0; x < size.w; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * size.w + x ) * 4;
				const float u  = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( size.w );
				const float v  = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( size.h );
				picture[ i + 0 ] = u;
				picture[ i + 1 ] = v * 0.731f;
				picture[ i + 2 ] = std::clamp( u * v * 1.37f, 0.0f, 1.0f );
				picture[ i + 3 ] = 1.0f;
			}

		//Three fields, because the first one has an empty target behind it
		//and the claim is about the steady state as well as the first frame.
		for( int i = 0; i < 3; ++i )
			if( !rig.frame( picture ) )
			{
				std::fprintf( stderr, "passthrough: ProcessOpenGL failed\n" );
				return 1;
			}

		const std::vector< float > image = rig.read();
		double worst                     = 0.0;
		int worstAt[ 3 ]                 = { 0, 0, 0 };
		double worstIn = 0.0, worstOut = 0.0;
		double alphaWorst = 0.0;

		for( int y = 0; y < size.h; ++y )
			for( int x = 0; x < size.w; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * size.w + x ) * 4;
				for( int c = 0; c < 3; ++c )
				{
					const double d = std::fabs( double( image[ i + c ] ) - double( picture[ i + c ] ) );
					if( d > worst )
					{
						worst      = d;
						worstAt[ 0 ] = x;
						worstAt[ 1 ] = y;
						worstAt[ 2 ] = c;
						worstIn    = picture[ i + c ];
						worstOut   = image[ i + c ];
					}
				}
				alphaWorst = std::max( alphaWorst,
				                       std::fabs( double( image[ i + 3 ] ) - double( picture[ i + 3 ] ) ) );
			}

		const bool ok = worst <= kTolerance && alphaWorst == 0.0;
		std::printf( "  %3dx%-3d worst %.3g at (%d,%d) channel %d: %.9g in, %.9g out; alpha %.3g  %s\n",
		             size.w, size.h, worst, worstAt[ 0 ], worstAt[ 1 ], worstAt[ 2 ],
		             worstIn, worstOut, alphaWorst, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "passthrough: ok" : "passthrough: FAILED" );
	return failures == 0 ? 0 : 1;
}

//===========================================================================
// --bench
//===========================================================================
double benchAt( int width, int height, int frames, const std::vector< std::string >& settings )
{
	Rig rig( width, height );
	std::string error;
	if( !rig.init( settings, error ) )
	{
		std::fprintf( stderr, "bench: %s\n", error.c_str() );
		return 0.0;
	}

	//The warm-up pays for allocating the state buffers, which at 4K is a
	//third of a gigabyte and is not something to time by accident.
	constexpr int kWarmup = 20;
	for( int i = 0; i < kWarmup; ++i )
		rig.frame( buildCard( width, height, i ) );
	glFinish();

	//The card is rebuilt on the CPU every field and that is not free, so it
	//is built ONCE here and reused: this measures ProcessOpenGL, not a
	//trigonometry loop.
	const std::vector< float > picture = buildCard( width, height, kWarmup );

	const auto start = std::chrono::steady_clock::now();
	for( int i = 0; i < frames; ++i )
		rig.frame( picture );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	return std::chrono::duration< double >( end - start ).count() * 1000.0
	       / static_cast< double >( frames );
}

int runBench( int frames, const std::vector< std::string >& settings )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state memory\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, settings );
		const double mb = 2.0 * size.width * size.height * 16.0 / ( 1024.0 * 1024.0 );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.0f MB\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0, mb );
	}

	std::printf( "\nThe cost is one picture-sized pass plus five small ones, so it is close to\n"
	             "linear in pixels. The state memory column is the headline number for this\n"
	             "plugin rather than the time: two RGBA32F buffers at picture size, doubled\n"
	             "again in Two Fields mode. RGBA32F is not caution -- see Plumbicon.h.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"pbtest -- render and check the Plumbicon camera-tube effect\n"
		"\n"
		"  --out PATH        render the moving test card through the plugin (default /tmp/plumbicon.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        fields to render before reading back (default 30)\n"
		"  --fps N           synthetic clock rate. INERT: the model counts fields (default 60)\n"
		"  --noise F         per-field noise on the SOURCE, 0..1 (not the plugin's Noise control)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its type, default and range, then exit\n"
		"  --lag             a highlight switched off, against the exact recursion\n"
		"  --comet           the tail behind a moving highlight, in pixels\n"
		"  --capacity        the target saturates\n"
		"  --burn            the burn accumulator's two poles\n"
		"  --passthrough     a beam above capacity is the identity\n"
		"  --bench           time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n"
		"\n"
		"Every check but --list needs a GPU. --list needs no GL context at all,\n"
		"which is what lets it run on a runner where creating one fails.\n" );
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, applied when the frame
// number is reached. The fleet's format, so one filming script drives any of
// the plugins.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	while( std::getline( file, line ) )
	{
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );

		std::istringstream stream( line );
		int frame = 0;
		if( !( stream >> frame ) )
			continue;

		std::string rest;
		std::getline( stream, rest );
		const size_t lastSpace = rest.find_last_of( " \t" );
		if( lastSpace == std::string::npos )
			continue;

		const std::string value = rest.substr( lastSpace + 1 );
		std::string name        = rest.substr( 0, lastSpace );
		while( !name.empty() && ( name.front() == ' ' || name.front() == '\t' ) )
			name.erase( name.begin() );
		while( !name.empty() && ( name.back() == ' ' || name.back() == '\t' ) )
			name.pop_back();

		tracks[ name ].push_back( { frame, std::strtof( value.c_str(), nullptr ) } );
	}

	for( auto& track : tracks )
		std::sort( track.second.begin(), track.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( track[ i ].first < frame )
			continue;
		const float span = static_cast< float >( track[ i ].first - track[ i - 1 ].first );
		const float t    = span > 0.0f ? ( frame - track[ i - 1 ].first ) / span : 0.0f;
		return track[ i - 1 ].second + ( track[ i ].second - track[ i - 1 ].second ) * t;
	}
	return track.back().second;
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/plumbicon.png";
	std::string cardPath;
	std::string scriptPath;
	int width  = 1280;
	int height = 720;
	int frames = 30;
	float noise = 0.0f;
	bool wantList = false, wantLag = false, wantComet = false, wantCapacity = false;
	bool wantBurn = false, wantPassthrough = false, wantBench = false, wantPipe = false;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			if( std::sscanf( argv[ ++i ], "%dx%d", &width, &height ) != 2 )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			++i;//accepted and inert; see the file header
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--lag" )
			wantLag = true;
		else if( argument == "--comet" )
			wantComet = true;
		else if( argument == "--capacity" )
			wantCapacity = true;
		else if( argument == "--burn" )
			wantBurn = true;
		else if( argument == "--passthrough" )
			wantPassthrough = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "size and frames must all be positive\n" );
		return 2;
	}

	//Answered before a context is made, so it still works on a machine -- or
	//a CI runner -- where creating one fails outright.
	if( wantList )
		return listParameters();

	if( !cardPath.empty() )
	{
		//Flipped, like every other picture this writes: the card is built in
		//GL's order, row 0 at the bottom, and a PNG's first row is its top.
		//Writing it unflipped makes the card and the rendered frame disagree
		//about which way up they are, which is a confusing way to look at a
		//comet tail.
		const std::vector< float > card = flipRowsF( buildCard( width, height, 0 ), width, height );
		if( !writePng( cardPath, width, height, toBytes( card ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( wantLag )
		result = runLagCheck();
	else if( wantComet )
		result = runCometCheck();
	else if( wantCapacity )
		result = runCapacityCheck();
	else if( wantBurn )
		result = runBurnCheck();
	else if( wantPassthrough )
		result = runPassthroughCheck();
	else if( wantBench )
		result = runBench( frames, settings );
	else
	{
		Rig rig( width, height );
		std::string error;
		if( !rig.init( settings, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
			return 1;
		}

		if( wantPipe )
		{
			//Raw RGBA in, raw RGBA out, one frame at a time.
			std::map< unsigned int, Track > automation;
			if( !scriptPath.empty() )
			{
				const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
				if( !error.empty() )
				{
					std::fprintf( stderr, "%s\n", error.c_str() );
					return 2;
				}
				//Resolve names up front and refuse an unknown one. A
				//misspelled name that silently did nothing would produce a
				//take that looks deliberate and is wrong.
				Plumbicon naming;
				for( const auto& entry : tracks )
				{
					const unsigned int index = findParameter( naming, entry.first );
					if( index >= Plumbicon::PT_COUNT )
					{
						std::fprintf( stderr,
						              "script names '%s', which is not a parameter (try --list)\n",
						              entry.first.c_str() );
						return 2;
					}
					automation[ index ] = entry.second;
				}
			}

			std::vector< unsigned char > raw( static_cast< size_t >( width ) * height * 4 );
			std::vector< float > picture( raw.size() );

			for( int index = 0;; ++index )
			{
				size_t got = 0;
				while( got < raw.size() )
				{
					const ssize_t n = read( STDIN_FILENO, raw.data() + got, raw.size() - got );
					if( n <= 0 )
						break;
					got += static_cast< size_t >( n );
				}
				//A partial frame at the end of a pipe is the end of the
				//stream, not a frame to render: half a frame of garbage
				//through a plugin with a charge store poisons every frame
				//after it.
				if( got < raw.size() )
					break;

				//Flipped on the way in because a raw frame arrives top row
				//first and GL wants bottom row first.
				for( int y = 0; y < height; ++y )
					for( int x = 0; x < width * 4; ++x )
						picture[ static_cast< size_t >( y ) * width * 4 + x ] =
							raw[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] / 255.0f;

				//Applied through the plugin's own setter, so a cue moves the
				//same thing an operator's slider would.
				for( const auto& track : automation )
					rig.set( track.first, valueAt( track.second, index ) );

				if( !rig.frame( picture ) )
					break;

				const std::vector< unsigned char > out =
					toBytes( flipRowsF( rig.read(), width, height ) );
				size_t written = 0;
				while( written < out.size() )
				{
					const ssize_t n = write( STDOUT_FILENO, out.data() + written, out.size() - written );
					if( n <= 0 )
						break;
					written += static_cast< size_t >( n );
				}
			}
		}
		else
		{
			//A still, at the end of a run of fields. THE CARD MOVES: a target
			//looking at a still picture settles in a few fields and then does
			//nothing anybody can see.
			for( int frame = 0; frame < frames; ++frame )
			{
				std::vector< float > picture = buildCard( width, height, frame );
				addNoise( picture, frame, noise );
				if( !rig.frame( picture ) )
				{
					std::fprintf( stderr, "ProcessOpenGL failed on field %d\n", frame );
					result = 1;
					break;
				}
			}

			if( result == 0 )
			{
				const std::vector< unsigned char > image =
					toBytes( flipRowsF( rig.read(), width, height ) );
				if( !writePng( outPath, width, height, image ) )
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					result = 1;
				}
				else
					std::printf( "wrote %s (%dx%d, %d fields)\n", outPath.c_str(), width, height, frames );
			}
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
