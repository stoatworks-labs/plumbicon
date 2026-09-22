#pragma once

#include <FFGLSDK.h>

namespace plumbicon
{
/**
    An off-screen buffer for one stage of the chain. tinsel's, with this
    plugin's reasons.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** `Ensure()` is called every frame
    and is a no-op in the overwhelming majority of them. That matters more here
    than in most of the fleet: the state buffers are picture-sized RGBA32F --
    a quarter of a gigabyte at 4K for the pair -- so a reallocation the plugin
    did not need is a quarter of a gigabyte of churn.

    Note what it CANNOT tell you: whether it reallocated. It returns "you have
    a buffer", not "the buffer is new", so `Plumbicon` tracks the width and
    height itself. A charge store carried across a resize is not a frame of
    noise; it is the last composition fading out over the first second of this
    one.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first. One leaked texture here is
    133 MB at 4K, and leaving Two Fields mode releases two of them.

    **It owns its filtering**, because this plugin's buffers want two different
    answers and the difference is load-bearing:

    - the **state** and **held** buffers are `Nearest`, because they are DATA.
      Every pass reads them texel for texel; a `GL_LINEAR` fetch at a texel
      centre is *meant* to return that texel exactly, and on a buffer that
      feeds back into itself once per field, "meant to" is not something to
      rest a charge store on. The cost is that the bright pass has to write its
      box average out by hand.
    - the **bloom** buffers are read *between* texels by the separable blur and
      want `GL_LINEAR`, which is what lets one tap average two texels and
      halves the sample count.

    `Mipmapped` is offered and unused. Nothing in this plugin reduces a buffer
    to a single value, which is the usual reason to want a chain.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back -- and the state buffers
	/// feed back into themselves once per field, so it is noise that takes
	/// `capacity / beam` fields to wash out and may never leave the burn
	/// accumulator at all.
	///
	/// TRUE MEANS "YOU HAVE A BUFFER", NOT "THE BUFFER IS NEW". See the class
	/// note: the caller tracks the size and clears on a resize itself.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. A shader that
	/// wanted it would need it as a uniform: `textureQueryLevels` is GLSL 4.30
	/// and these shaders are 4.10. Unused here; kept with the rest of the
	/// class so this file stays one file across the fleet.
	float MaxMipLevel() const;

	/// Clear to transparent black. The state buffers need this whenever the
	/// picture changes size, so the first field charges an empty target rather
	/// than reading out the last composition.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace plumbicon
