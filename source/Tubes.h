#pragma once

/**
	The tube types, as an OVERRIDE of the controls that say what a tube is
	made of.

	------------------------------------------------------- override, not write

	Picking a type does **not** write into `params[]` and does not raise value
	events. `Effective()` in `Plumbicon.cpp` is the one place that reads this
	table, and it is read fresh every frame.

	That is graticule's pattern rather than afterglow's, and it is chosen
	deliberately. A copy-based preset has to survive a host that owns parameter
	state and restates it whenever it likes -- Resolume does exactly that, it
	does not consume the value events a plugin raises, and getting it wrong is
	how a preset dropdown snaps straight back to Custom (reported against
	vertigo as its issue #2 and fixed across seven plugins). An override has
	none of that surface: the host's parameters are never touched, so there is
	nothing for the host to argue with.

	The cost, which is real: while a type is selected the seven controls below
	are inert. The operator's slider still moves and the picture does not. That
	is why `Custom` exists, why the README says so in as many words, and why
	`tools/sweep.py` gives every one of them a `Type=0` context -- without it
	the sweep would report seven dead controls, correctly.

	--------------------------------------------------------- what is NOT here

	`Sensitivity`, `Target Capacity` and `Beam Current` are deliberately left
	free. They are the exposure -- how the camera is set up on the day -- not
	what the tube is. Pinning them would mean a type could not be metered.

	--------------------------------------------------------------- the values

	These are judged, not measured. Nothing here was taken from a tube data
	sheet: they are the ordering and the rough spacing that the literature
	agrees on (a plumbicon lags and burns far less than a vidicon; a saticon
	sits between them; a vidicon's transfer gamma is around 0.65 against a
	plumbicon's near 1.0), expressed in this plugin's own control range. See
	AGENTS.md.
*/

namespace plumbicon::tubes
{

/// The controls a type pins, in table order. The plugin binds these to its own
/// ParamIDs in one array, so the table stays host-agnostic.
enum Param
{
	kGamma,
	kDark,
	kLag,
	kBurnRise,
	kBurnFall,
	kBurnDepth,
	kHalo,
	kParamCount
};

struct Tube
{
	const char* name;
	float v[ kParamCount ];///< host units, 0..1, in Param order
};

inline constexpr int kCount = 4;

/// Element 0 of the Type dropdown is Custom, which is NOT in this table: it
/// means "the sliders are the truth". So a Type VALUE of n names kTubes[n-1].
inline constexpr Tube kTubes[ kCount ] = {
	//                gamma  dark   lag   rise   fall  depth  halo
	//Plumbicon: the lead-oxide target the plugin is named for. Near-linear
	//transfer, low dark current, low lag, and it is the tube that made
	//colour broadcast cameras usable in a studio rather than a furnace.
	{ "Plumbicon",   { 0.50f, 0.05f, 0.18f, 0.12f, 0.55f, 0.18f, 0.00f } },

	//Vidicon: antimony trisulphide. Cheap, sensitive, and famous for
	//smearing everything and keeping a picture of whatever you left it
	//pointing at. Gamma 0.65 -> param 0.15.
	{ "Vidicon",     { 0.15f, 0.55f, 0.72f, 0.62f, 0.22f, 0.75f, 0.00f } },

	//Saticon: selenium-arsenic-tellurium. Between the two, which is the
	//whole reason it existed. Gamma 0.8 -> param 0.30.
	{ "Saticon",     { 0.30f, 0.25f, 0.42f, 0.35f, 0.38f, 0.40f, 0.00f } },

	//Image Orthicon: the one with the black halo. Everything else here is
	//ordinary; the halo is the signature, and it is the one term in this
	//plugin that does not fall out of the charge store.
	{ "Image Orthicon", { 0.50f, 0.30f, 0.30f, 0.30f, 0.45f, 0.30f, 0.75f } },
};

} // namespace plumbicon::tubes
