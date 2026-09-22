#pragma once

/**
	Host parameters (0..1) to the target's own units.

	------------------------------------------------------------ the time unit

	**Everything in this plugin is measured in FIELDS, and one field is one
	`ProcessOpenGL` call.** There is no `dt`, no seconds and no clock anywhere
	in the charge model.

	That is not a shortcut, it is what the physics is quoted in. A camera
	tube's lag is a *field* figure -- "third-field lag, 3%" is how a tube is
	specified, and a 50 Hz tube and a 60 Hz tube with the same third-field
	figure behave identically per field and differently per second. So
	`Sensitivity` is charge per field per unit of light, `Beam Current` is
	charge removable per field, and `Burn Rate` is a per-field pole.

	Two consequences, and both are honest costs rather than oversights:

	- the look depends on the composition's frame rate, because a field is a
	  rendered frame. A 30 fps project holds a smear for twice as long in
	  wall-clock terms as a 60 fps one. A real camera does exactly that.
	- a host that renders the same composition frame twice (two outputs, a
	  preview plus a record) advances the target twice. Nothing in FFGL lets a
	  plugin with memory know that happened; afterglow has the same property
	  and says so too.

	The pay-off is that every number the harness checks is exact arithmetic on
	uniforms it can read out of these functions, with no clock to calibrate and
	no frame rate to assume. See `tools/pbtest/main.cpp`.

	--------------------------------------------------------------- the ranges

	Four of these mappings are chosen so that a specific slider position lands
	on a specific value EXACTLY in binary, because a harness check depends on
	it:

	- `SensitivityFromParam( 0.25 ) == 1.0` exactly (0.25 * 4.0).
	- `TransferGammaFromParam( 0.5 ) == 1.0` exactly (0.5 + 0.5), which is why
	  the gamma range is 0.5..1.5 rather than the more natural 0.45..1.4.
	- `DarkCurrentFromParam( 0 ) == 0` and `LeakFromParam( 0 ) == 0` exactly,
	  so the "pure" recursion the `--lag` check predicts really is
	  `charge -= min( charge, beam )` with nothing else in it.
	- `LagScaleFromParam( 0 ) == 1.0` exactly, so Beam Current means what it
	  says when Lag Amount is at its null.

	Move a range and a check that was exact becomes approximately true, which
	is the worst of both.
*/

namespace plumbicon
{

//---------------------------------------------------------------------------
// Tube
//---------------------------------------------------------------------------

/// Charge deposited per field per unit of incident light, before burn.
/// 0..1 -> 0..4. A value of 1.0 is unity: one unit of light makes one unit of
/// charge in one field, which is the setting the pass-through check uses.
float SensitivityFromParam( float p );

/// The most charge the target can hold, in the same units. 0.04..6.0. Light
/// past this is simply not stored -- which is why a highlight blocks up and
/// stops carrying detail rather than getting brighter.
///
/// The ratio `Capacity / Beam Current` is the LENGTH OF THE TAIL in fields,
/// and `Beam Current / Sensitivity` is the light level at which the target
/// starts blocking up. They are independent, which is why both controls
/// exist.
float CapacityFromParam( float p );

/// Charge the beam can remove in one field. 0.02..6.0, logarithmic, because
/// the interesting region is the bottom: a beam that can only take a fifth of
/// a saturated target per pass is where lag lives, and a linear control would
/// spend nine tenths of its travel above the point where anything happens.
///
/// At the top of its range it is above the top of the capacity range, so the
/// beam can always empty a full target in one pass and there is no lag at
/// all. That is the setting `pbtest --passthrough` uses.
float BeamCurrentFromParam( float p );

/// The transfer characteristic's exponent. 1.0 is a plumbicon (near-linear);
/// 0.65 is a vidicon. 0.5..1.5.
float TransferGammaFromParam( float p );

/// Charge per field that arrives whether or not there is any light -- the
/// target's own leakage. Lifts the blacks and, because it is charge like any
/// other, gives the dark parts of the picture lag of their own. 0..0.08,
/// squared so the useful bottom of the range is not one pixel of travel.
float DarkCurrentFromParam( float p );

//---------------------------------------------------------------------------
// Lag
//---------------------------------------------------------------------------

/// Multiplier on the beam current. 1.0 at the null, 1/25 at the top.
///
/// Lag Amount and Beam Current multiply into one number and that is
/// deliberate: they are the same physical quantity reached from two
/// directions. Beam Current is what the tube IS -- it is what `Type` moves --
/// and Lag Amount is how much of it the operator wants today.
float LagScaleFromParam( float p );

/// Fraction of the residual charge that leaks away on its own between scans,
/// per field. The target is not a perfect capacitor; a real one recovers from
/// a smear a little faster than the beam alone would take it.
///
/// Exactly zero at the null, which is what makes the `--lag` check's closed
/// form the pure `charge -= min( charge, beam )` recursion rather than that
/// recursion times a decay.
float LeakFromParam( float p );

//---------------------------------------------------------------------------
// Burn
//---------------------------------------------------------------------------

/// The pole of the burn accumulator while it is RISING, per field.
/// 0 .. 5e-3, i.e. a time constant from "never" down to about 200 fields.
float BurnRiseFromParam( float p );

/// The pole while the burn is FALLING, per field. Deliberately a slower range
/// than the rise -- a tube etches faster than it recovers, which is the entire
/// reason burn-in is a thing anybody has ever had to live with.
float BurnFallFromParam( float p );

/// How much of the sensitivity a fully burnt area loses. 0..1.
float BurnDepthFromParam( float p );

//---------------------------------------------------------------------------
// Optics
//---------------------------------------------------------------------------

/// Strength of the scattered light added back. 0..1.5.
float HalationFromParam( float p );

/// Blur radius for the halation, in texels of the quarter-size bloom buffer.
/// 0.5..12.
float HalationRadiusFromParam( float p );

/// Signal level above which light is treated as scattering in the faceplate.
/// 0..2, in the same units as the signal.
float BloomThresholdFromParam( float p );

/// Strength of the Image Orthicon's dark halo. 0..2.
///
/// THIS IS THE ONE TERM IN THE PLUGIN THAT DOES NOT FALL OUT OF THE CHARGE
/// STORE. See AGENTS.md; it is a separate mechanism (redistribution of
/// secondary electrons) bolted on as a subtractive ring, and it is flagged
/// everywhere it appears.
float HaloFromParam( float p );

//---------------------------------------------------------------------------
// Output
//---------------------------------------------------------------------------

/// Peak-to-peak amplitude of the additive noise. 0..0.15.
float NoiseFromParam( float p );

} // namespace plumbicon
