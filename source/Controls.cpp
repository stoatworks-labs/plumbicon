#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace plumbicon
{
namespace
{
float clamp01( float p )
{
	return std::min( 1.0f, std::max( 0.0f, p ) );
}
} // namespace

//---------------------------------------------------------------------------
// Tube
//---------------------------------------------------------------------------
float SensitivityFromParam( float p )
{
	//0.25 * 4.0 is exactly 1.0 in binary, and the pass-through check depends
	//on it. Both factors are powers of two; nothing here rounds.
	return clamp01( p ) * 4.0f;
}

float CapacityFromParam( float p )
{
	//A target that saturates below white would clip every picture, so the
	//bottom of the range is still a quarter of full scale rather than zero.
	return 0.25f + clamp01( p ) * 5.75f;
}

float BeamCurrentFromParam( float p )
{
	//Logarithmic over 300:1. The top end is 6.0, which is the top of the
	//capacity range: at maximum the beam can always empty a full target in
	//one pass, and there is no lag at all. That is not a curiosity -- it is
	//the setting the pass-through check needs.
	return 0.02f * std::pow( 300.0f, clamp01( p ) );
}

float TransferGammaFromParam( float p )
{
	//0.5 + 0.5 is exactly 1.0. The obvious 0.45..1.4 range does not have a
	//slider position that lands on unity gamma, and pow( L, 0.9999999 ) is
	//not the identity.
	return 0.5f + clamp01( p );
}

float DarkCurrentFromParam( float p )
{
	const float q = clamp01( p );
	return 0.08f * q * q;//exactly 0 at the null
}

//---------------------------------------------------------------------------
// Lag
//---------------------------------------------------------------------------
float LagScaleFromParam( float p )
{
	const float q = clamp01( p );
	return 1.0f / ( 1.0f + 24.0f * q * q );//exactly 1.0 at the null
}

float LeakFromParam( float p )
{
	const float q = clamp01( p );
	return 0.5f * q * q;//exactly 0 at the null
}

//---------------------------------------------------------------------------
// Burn
//---------------------------------------------------------------------------
float BurnRiseFromParam( float p )
{
	const float q = clamp01( p );
	//5e-3 per field is a time constant of 200 fields -- about three seconds
	//at 60. Anything faster stops being burn and starts being lag.
	return 5.0e-3f * q * q;
}

float BurnFallFromParam( float p )
{
	const float q = clamp01( p );
	//Deliberately 2.5x slower than the rise at the same slider position. A
	//tube etches faster than it recovers.
	return 2.0e-3f * q * q;
}

float BurnDepthFromParam( float p )
{
	return clamp01( p );
}

//---------------------------------------------------------------------------
// Optics
//---------------------------------------------------------------------------
float HalationFromParam( float p )
{
	return clamp01( p ) * 1.5f;
}

float HalationRadiusFromParam( float p )
{
	return 0.5f + clamp01( p ) * 11.5f;
}

float BloomThresholdFromParam( float p )
{
	return clamp01( p ) * 2.0f;
}

float HaloFromParam( float p )
{
	return clamp01( p ) * 2.0f;
}

//---------------------------------------------------------------------------
// Output
//---------------------------------------------------------------------------
float NoiseFromParam( float p )
{
	return clamp01( p ) * 0.15f;
}

} // namespace plumbicon
