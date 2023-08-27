ReShade FX shading language
===========================

# Contents

* [Macros](#macros)
* [Texture object](#texture-object)
* [Sampler object](#sampler-object)
* [Storage object](#storage-object)
* [Uniform variables](#uniform-variables)
* [Structs](#structs)
* [Namespaces](#namespaces)
* [User functions](#user-functions)
* [Intrinsic functions](#intrinsic-functions)
* [Techniques](#techniques)

# Concepts

The ReShade FX shading language is heavily based on the DX9-style HLSL syntax, with a few extensions. For more details on HLSL, check out the Programming Guide: https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-writing-shaders-9 .\
This document will instead primarily focus on syntax and features that are unique to ReShade FX.

### Macros

The ReShade FX compiler predefines certain preprocessor macros, as listed below:
* ``__FILE__`` Current file path
* ``__FILE_NAME__`` Current file name without path
* ``__FILE_STEM__`` Current file name without extension and path
* ``__LINE__`` Current line number
* ``__RESHADE__`` Version of the injector (in the format `MAJOR * 10000 + MINOR * 100 + REVISION`)
* ``__APPLICATION__`` 32-bit truncated Fnv1a hash of the application executable name
* ``__VENDOR__`` Vendor id (e.g. 0x10de for NVIDIA, 0x1002 for AMD)
* ``__DEVICE__`` Device id
* ``__RENDERER__`` Graphics API used to render effects
  * D3D9: 0x9000
  * D3D10: 0xa000 or higher
  * D3D11: 0xb000 or higher (e.g. 0xb100 for D3D11.1)
  * D3D12: 0xc000 or higher
  * OpenGL: 0x10000 or higher (e.g. 0x14300 for OpenGL 4.3)
  * Vulkan: 0x20000 or higher (e.g. 0x21100 for Vulkan 1.1)
* ``BUFFER_WIDTH`` Backbuffer width (essentially the width of the image the application renders to the screen)
* ``BUFFER_HEIGHT`` Backbuffer height
* ``BUFFER_RCP_WIDTH`` Reciprocal of the backbuffer width (equals `1.0 / BUFFER_WIDTH`)
* ``BUFFER_RCP_HEIGHT`` Reciprocal of the backbuffer height (equals `1.0 / BUFFER_HEIGHT`)
* ``BUFFER_COLOR_BIT_DEPTH`` Color bit depth of the backbuffer (8 or 10)
* ``BUFFER_COLOR_SPACE`` Color space type for presentation; 0 = unknown, 1 = sRGB, 2 = scRGB, 3 = HDR10 ST2084, 4 = HDR10 HLG.

Constructs like the following may be interpreted as a configurable UI option. To prevent this, the preprocessor define name can be prefixed with an underscore or made shorter than 8 characters, in which case ReShade will not display it in the UI.
```hlsl
#ifndef MY_PREPROCESSOR_DEFINE
	#define MY_PREPROCESSOR_DEFINE 0
#endif
```

You can disable optimization during shader compilation by adding this line to an effect file:
```c
#pragma reshade skipoptimization
```

### Texture Object

> Textures are multidimensional data containers usually used to store images.

Annotations:

 * ``texture2D imageTex < source = "path/to/image.bmp"; > { ... };``  
 Opens image from the patch specified, resizes it to the texture size and loads it into the texture.\
 ReShade supports Bitmap (\*.bmp), Portable Network Graphics (\*.png), JPEG (\*.jpg), Targa Image (\*.tga) and DirectDraw Surface (\*.dds) files.

 * ``texture2D myTex1 < pooled = true; > { Width = 100; Height = 100; Format = RGBA8; };``  
 ``texture2D myTex2 < pooled = true; > { Width = 100; Height = 100; Format = RGBA8; };``  
 ReShade will attempt to re-use the same memory for textures with the same dimensions and format across effect files if the pooled annotation is set.

ReShade FX allows semantics to be used on texture declarations. This is used to request special textures:

 * ``texture2D texColor : COLOR;``  
 Receives the backbuffer contents (read-only).
 * ``texture2D texDepth : DEPTH;``  
 Receives the game's depth information (read-only).

Declared textures are created at runtime with the parameters specified in their definition body.

```hlsl
texture2D texColorBuffer : COLOR;
texture2D texDepthBuffer : DEPTH;

texture2D texTarget
{
	// The texture dimensions (default: 1x1).
	Width = BUFFER_WIDTH / 2; // Used with texture1D
	Height = BUFFER_HEIGHT / 2; // Used with texture1D and texture2D
	Depth = 1; // Used with texture1D, texture2D and texture3D
	
	// The number of mipmaps including the base level (default: 1).
	MipLevels = 1;
	
	// The internal texture format (default: RGBA8).
	// Available formats:
	//   R8, R16, R16F, R32F, R32I, R32U
	//   RG8, RG16, RG16F, RG32F
	//   RGBA8, RGBA16, RGBA16F, RGBA32F
	//   RGB10A2
	Format = RGBA8;

	// Unspecified properties are set to the defaults shown here.
};

texture3D texIntegerVolume
{
	Width = 10;
	Height = 10;
	Depth = 10;
	Format = R32I; // Single-component integer format, which means sampler and storage have to be of that integer type (sampler3D<int> or storage3D<int>)
};
```

### Sampler Object

> Samplers are the bridge between textures and shaders. They define how a texture is read from and how data outside texel coordinates is sampled. Multiple samplers can refer to the same texture using different options.

```hlsl
sampler2D samplerColor
{
	// The texture to be used for sampling.
	Texture = texColorBuffer;

	// The method used for resolving texture coordinates which are out of bounds.
	// Available values: CLAMP, MIRROR, WRAP or REPEAT, BORDER
	AddressU = CLAMP;
	AddressV = CLAMP;
	AddressW = CLAMP;

	// The magnification, minification and mipmap filtering types.
	// Available values: POINT, LINEAR
	MagFilter = LINEAR;
	MinFilter = LINEAR;
	MipFilter = LINEAR;

	// The maximum mipmap levels accessible.
	MinLOD = 0.0f;
	MaxLOD = 1000.0f;

	// An offset applied to the calculated mipmap level (default: 0).
	MipLODBias = 0.0f;

	// Enable or disable converting  to linear colors when sampling from the
	// texture.
	SRGBTexture = false;

	// Unspecified properties are set to the defaults shown here.
};

sampler2D samplerDepth
{
	Texture = texDepthBuffer;
};
sampler2D samplerTarget
{
	Texture = texTarget;
};
```

### Storage Object

> Storage objects define how a texture should be written to from compute shaders.

```hlsl
storage2D storageTarget
{
	// The texture to be used as storage.
	Texture = texTarget;

	// The mipmap level of the texture to fetch/store.
	MipLevel = 0;
};

storage3D<int> storageIntegerVolume
{
	Texture = texIntegerVolume;
};
```

### Uniform Variables

> Global variables with the `uniform` qualifier are constant across each iteration of a shader per pass and may be controlled via the UI.

Annotations to customize UI appearance:

 * ui_type: Can be `input`, `drag`, `slider`, `combo`, `radio` or `color`
 * ui_min: The smallest value allowed in this variable (required when `ui_type = "drag"` or `ui_type = "slider"`)
 * ui_max: The largest value allowed in this variable (required when `ui_type = "drag"` or `ui_type = "slider"`)
 * ui_step: The value added/subtracted when clicking the button next to the slider
 * ui_items: A list of items for the combo box or radio buttons, each item is terminated with a `\0` character (required when `ui_type = "combo"` or `ui_type = "radio"`)
 * ui_label: Display name of the variable in the UI. If this is missing, the variable name is used instead.
 * ui_tooltip: Text that is displayed when the user hovers over the variable in the UI. Use this for a description.
 * ui_category: Groups values together under a common headline. Note that all variables in the same category also have to be declared next to each other for this to be displayed correctly.
 * ui_category_closed: Set to true to show a category closed by default.
 * ui_spacing: Adds space before the UI widget (multiplied by the value of the annotation).
 * ui_units: Adds units description on the slider/drag bar (only used when `ui_type = "drag"` or `ui_type = "slider"`)
 * hidden: Set to true to hide this technique in the UI.

Annotations are also used to request special runtime values (via the `source` annotation):

 * ``uniform float frametime < source = "frametime"; >;``  
 Time in milliseconds it took for the last frame to complete.
 * ``uniform int framecount < source = "framecount"; >;``  
 Total amount of frames since the game started.
 * ``uniform float4 date < source = "date"; >;``  
 float4(year, month (1 - 12), day of month (1 - 31), time in seconds)
 * ``uniform float timer < source = "timer"; >;``  
 Timer counting time in milliseconds since game start.
 * ``uniform float2 pingpong < source = "pingpong"; min = 0; max = 10; step = 2; smoothing = 0.0; >;``  
 Value that smoothly interpolates between `min` and `max` using `step` as the increase/decrease value every second (so a step value of 1 means the value is increased/decreased by 1 per second).\
 In this case it would go from 0 to 10 in 5 seconds and then back to 0 in another 5 seconds (interpolated every frame).
 The `smoothing` value affects the interpolation curve (0 is linear interpolation and anything else changes the speed depending on how close the current value is to `min` or `max`).
 The second component is either +1 or -1 depending on the direction it currently goes.
 * ``uniform int random_value < source = "random"; min = 0; max = 10; >;``  
 Gets a new random value between min and max every pass.
 * ``uniform bool space_bar_down < source = "key"; keycode = 0x20; mode = ""; >;``  
 True if specified keycode (in this case the spacebar) is pressed and false otherwise.
 If mode is set to "press" the value is true only in the frame the key was initially held down.
 If mode is set to "toggle" the value stays true until the key is pressed a second time.
 * ``uniform bool left_mouse_button_down < source = "mousebutton"; keycode = 0; mode = ""; >;``  
 True if specified mouse button (0 - 4) is pressed and false otherwise.
 If mode is set to "press" the value is true only in the frame the key was initially held down.
 If mode is set to "toggle" the value stays true until the key is pressed a second time.
 * ``uniform float2 mouse_point < source = "mousepoint"; >;``  
 Gets the position of the mouse cursor in screen coordinates.
 * ``uniform float2 mouse_delta < source = "mousedelta"; >;``  
 Gets the movement of the mouse cursor in screen coordinates.
 * ``uniform float2 mouse_value < source = "mousewheel"; min = 0.0; max = 10.0; > = 1.0;``  
 The first component value is modified via the mouse wheel. Starts at 1.0, goes up (but not past 10.0) when mouse wheel is moved forward and down (but not past 0.0) when it is moved backward.
 The second component holds the current wheel state (how much the mouse wheel was moved this frame). It's positive for forward movement, negative for backward movement or zero for no movement.
 * ``uniform bool has_depth < source = "bufready_depth"; >;``  
 True if the application's depth buffer is available in textures declared with `DEPTH`, false if not.
 * ``uniform bool overlay_open < source = "overlay_open"; >;``  
 True if the ReShade in-game overlay is currently open, false if not.
 * ``uniform int active_variable < source = "overlay_active"; >;``  
 Contains the one-based index of the uniform variable currently being modified in the overlay, zero if none.
 * ``uniform int hovered_variable < source = "overlay_hovered"; >;``  
 Contains the one-based index of the uniform variable currently hovered with the cursor in the overlay, zero if none.
 * ``uniform bool screenshot < source = "screenshot"; >;``  
 True if a screenshot is being taken, false if not.

```hlsl
// Initializers are used to specify the default value (zero is used if not specified).
uniform float4 UniformSingleValue = float4(0.0f, 0.0f, 0.0f, 0.0f);

// It is recommended to use constants instead of uniforms if the value is not changing or user-configurable.
static const float4 ConstantSingleValue = float4(0.0f, 0.0f, 0.0f, 0.0f);
```

### Structs

> Structs are user defined data types that can be used as types for variables. These behave the same as in HLSL.

```hlsl
struct MyStruct
{
	int MyField1, MyField2;
	float MyField3;
};
```

### Namespaces

> Namespaces are used to group functions and variables together, which is especially useful to prevent name clashing.
> The "::" operator is used to resolve variables or functions inside other namespaces.

```hlsl
namespace MyNamespace
{
	namespace MyNestedNamespace
	{
		void DoNothing()
		{
		}
	}

	void DoNothing()
	{
		MyNestedNamespace::DoNothing();
	}
}
```

### User functions

Parameter qualifiers:

 * ``in`` Declares an input parameter. Default and implicit if none is used. Functions expect these to be filled with a value.
 * ``out`` Declares an output parameter. The value is filled in the function and can be used in the caller again.
 * ``inout`` Declares a parameter that provides input and also expects output.

Supported flow-control statements:

 * ``if ([condition]) { [statement...] } [else { [statement...] }]``  
 Statements after if are only executed  if condition is true, otherwise the ones after else are executed (if it exists).
 * ``switch ([expression]) { [case [constant]/default]: [statement...] }``  
 Selects the case matching the switch expression or default if non does and it exists.
 * ``for ([declaration]; [condition]; [iteration]) { [statement...] }``  
 Runs the statements in the body as long as the condition is true. The iteration expression is executed after each run.
 * ``while ([condition]) { [statement...] }``  
 Runs the statements in the body as long as the condition is true.
 * ``do { [statement...] } while ([condition]);``  
 Similar to a normal while loop with the difference that the statements are executed at least once.
 * ``break;``  
 Breaks out  of the current loop or switch statement and jumps to the statement after.
 * ``continue;``  
 Jumps directly to the next loop iteration ignoring any left code in the current one.
 * ``return [expression];``  
 Jumps out of the current function, optionally providing a value to the caller.
 * ``discard;``  
 Abort rendering of the current pixel and step out of the shader. Can be used in pixel shaders only.

```hlsl
// Semantics are used to tell the runtime which arguments to connect between shader stages.
// They are ignored on non-entry-point functions (those not used in any pass below).
// Semantics starting with "SV_" are system value semantics and serve a special meaning.
// The following vertex shader demonstrates how to generate a simple fullscreen triangle with the three vertices provided by ReShade (http://redd.it/2j17wk):
void ExampleVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2, -2) + float2(-1, 1), 0, 1);
}

// The following pixel shader simply returns the color of the games output again without modifying it (via the "color" output parameter):
void ExamplePS0(float4 pos : SV_Position, float2 texcoord : TEXCOORD0, out float4 color : SV_Target)
{
	color = tex2D(samplerColor, texcoord);
}

// The following pixel shader takes the output of the previous pass and adds the depth buffer content to the right screen side.
float4 ExamplePS1(float4 pos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
	// Here color information is sampled with "samplerTarget" and thus from "texTarget" (see sampler declaration above),
	// which was set as render target in the previous pass (see the technique definition below) and now contains its output.
	// In this case it is the game output, but downsampled to half because the texture is only half of the screen size.
	float4 color = tex2D(samplerTarget, texcoord);
	
	// Only execute the following code block when on the right half of the screen.
	if (texcoord.x > 0.5f)
	{
		// Sample from the game depth buffer using the "samplerDepth" sampler declared above.
		float depth = tex2D(samplerDepth, texcoord).r;
		
		// Linearize the depth values to better visualize them.
		depth = 2.0 / (-99.0 * depth + 101.0);
		
		color.rgb = depth.rrr;
	}

	return color;
}

// The following compute shader uses shared memory within a thread group:
groupshared int sharedMem[64];
void ExampleCS0(uint3 tid : SV_GroupThreadID)
{
	if (tid.y == 0)
		sharedMem[tid.x] = tid.x;
	barrier();
	if (tid.y == 0 && (tid.x % 2) != 0)
		sharedMem[tid.x] += sharedMem[tid.x + 1];
}

// The following compute shader writes a color gradient to the "texTarget" texture:
void ExampleCS1(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID)
{
	tex2Dstore(storageTarget, id.xy, float4(tid.xy / float2(20 * 64, 2 * 8), 0, 1));
}
```

### Intrinsic functions

ReShade FX supports most of the standard HLSL intrinsics.\
Check out https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-intrinsic-functions for reference on them:

> abs, acos, all, any, asfloat, asin, asint, asuint, atan, atan2, ceil, clamp, cos, cosh, cross, ddx, ddy, degrees, determinant, distance, dot, exp, exp2, faceforward, floor, frac, frexp, fwidth, isinf, isnan, ldexp, length, lerp, log, log10, log2, mad, max, min, modf, mul, normalize, pow, radians, rcp, reflect, refract, round, rsqrt, saturate, sign, sin, sincos, sinh, smoothstep, sqrt, step, tan, tanh, transpose, trunc

In addition to these, ReShade FX provides a few additional ones:

 * ``T tex1D(sampler1D<T> s, float coords)``  
 * ``T tex1D(sampler1D<T> s, float coords, int offset)``  
 * ``T tex2D(sampler2D<T> s, float2 coords)``  
 * ``T tex2D(sampler2D<T> s, float2 coords, int2 offset)``  
 * ``T tex3D(sampler3D<T> s, float3 coords)``  
 * ``T tex3D(sampler3D<T> s, float3 coords, int3 offset)``  
 Samples a texture.\
 See also https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-sample.
 * ``T tex1Dlod(sampler1D<T> s, float4 coords)``  
 * ``T tex1Dlod(sampler1D<T> s, float4 coords, int offset)``  
 * ``T tex2Dlod(sampler2D<T> s, float4 coords)``  
 * ``T tex2Dlod(sampler2D<T> s, float4 coords, int2 offset)``  
 * ``T tex3Dlod(sampler3D<T> s, float4 coords)``  
 * ``T tex3Dlod(sampler3D<T> s, float4 coords, int3 offset)``  
 Samples a texture on a specific mipmap level.\
 The accepted coordinates are in the form `float4(x, y, 0, lod)`.\
 See also https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel.
 * ``T tex1Dfetch(sampler1D<T> s, int coords)``  
 * ``T tex1Dfetch(sampler1D<T> s, int coords, int lod)``  
 * ``T tex1Dfetch(storage1D<T> s, int coords)``  
 * ``T tex2Dfetch(sampler2D<T> s, int2 coords)``  
 * ``T tex2Dfetch(sampler2D<T> s, int2 coords, int lod)``  
 * ``T tex2Dfetch(storage2D<T> s, int2 coords)``  
 * ``T tex3Dfetch(sampler3D<T> s, int3 coords)``  
 * ``T tex3Dfetch(sampler3D<T> s, int3 coords, int lod)``  
 * ``T tex3Dfetch(storage3D<T> s, int3 coords)``  
 Fetches a value from the texture directly without any sampling.\
 See also https://docs.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-load.
 * ``float4 tex2DgatherR(sampler2D s, float2 coords)``  
 * ``float4 tex2DgatherR(sampler2D s, float2 coords, int2 offset)``  
 * ``float4 tex2DgatherG(sampler2D s, float2 coords)``  
 * ``float4 tex2DgatherG(sampler2D s, float2 coords, int2 offset)``  
 * ``float4 tex2DgatherB(sampler2D s, float2 coords)``  
 * ``float4 tex2DgatherB(sampler2D s, float2 coords, int2 offset)``  
 * ``float4 tex2DgatherA(sampler2D s, float2 coords)``  
 * ``float4 tex2DgatherA(sampler2D s, float2 coords, int2 offset)``  
 Gathers the specified component of the four neighboring pixels and returns the result.\
 `tex2DgatherR` for example is equivalent to https://docs.microsoft.com/windows/win32/direct3dhlsl/texture2d-gatherred.  
 The return value is effectively:
 ```
 float4(tex2Dfetch(s, coords * tex2Dsize(s) + int2(0, 1)).comp,
        tex2Dfetch(s, coords * tex2Dsize(s) + int2(1, 1)).comp,
        tex2Dfetch(s, coords * tex2Dsize(s) + int2(0, 1)).comp,
        tex2Dfetch(s, coords * tex2Dsize(s) + int2(0, 0)).comp)
 ```
 * ``int tex1Dsize(sampler1D<T> s)``  
 * ``int tex1Dsize(sampler1D<T> s, int lod)``  
 * ``int tex1Dsize(storage1D<T> s)``  
 * ``int2 tex2Dsize(sampler2D<T> s)``  
 * ``int2 tex2Dsize(sampler2D<T> s, int lod)``  
 * ``int2 tex2Dsize(storage2D<T> s)``  
 * ``int3 tex3Dsize(sampler3D<T> s)``  
 * ``int3 tex3Dsize(sampler3D<T> s, int lod)``  
 * ``int3 tex3Dsize(storage3D<T> s)``  
 Gets the texture dimensions of the specified mipmap level.\
 See also https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-getdimensions
 * ``void tex1Dstore(storage1D<T> s, int coords, T value)``  
 * ``void tex2Dstore(storage2D<T> s, int2 coords, T value)``  
 * ``void tex3Dstore(storage2D<T> s, int3 coords, T value)``  
 Writes the specified value to the texture referenced by the storage. Only valid from within compute shaders.\
 See also https://docs.microsoft.com/windows/win32/direct3dhlsl/sm5-object-rwtexture2d-operatorindex
 * ``void barrier()``  
 Synchronizes threads in a thread group.\
 Is equivalent to https://docs.microsoft.com/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync
 * ``void memoryBarrier()``  
 Waits on the completion of all memory accesses resulting from the use of texture or storage operations.\
 Is equivalent to https://docs.microsoft.com/windows/win32/direct3dhlsl/allmemorybarrier
 * ``void groupMemoryBarrier()``  
 Waits on the completion of all memory accesses within the thread group resulting from the use of texture or storage operations.\
 Is equivalent to https://docs.microsoft.com/windows/win32/direct3dhlsl/groupmemorybarrier
 * ``int atomicAdd(inout int dest, int value)``  
 * ``int atomicAdd(storage1D<int> s, int coords, int value)``  
 * ``int atomicAdd(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicAdd(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedadd
 * ``int atomicAnd(inout int dest, int value)``  
 * ``int atomicAnd(storage1D<int> s, int coords, int value)``  
 * ``int atomicAnd(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicAnd(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedand
 * ``int atomicOr(inout int dest, int value)``  
 * ``int atomicOr(storage1D<int> s, int coords, int value)``  
 * ``int atomicOr(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicOr(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedor
 * ``int atomicXor(inout int dest, int value)``  
 * ``int atomicXor(storage1D<int> s, int coords, int value)``  
 * ``int atomicXor(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicXor(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedxor
 * ``int atomicMin(inout int dest, int value)``  
 * ``int atomicMin(storage1D<int> s, int coords, int value)``  
 * ``int atomicMin(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicMin(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedmin
 * ``int atomicMax(inout int dest, int value)``  
 * ``int atomicMax(storage<int> s, int coords, int value)``  
 * ``int atomicMax(storage<int> s, int2 coords, int value)``  
 * ``int atomicMax(storage<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedmax
 * ``int atomicExchange(inout int dest, int value)``  
 * ``int atomicExchange(storage1D<int> s, int coords, int value)``  
 * ``int atomicExchange(storage2D<int> s, int2 coords, int value)``  
 * ``int atomicExchange(storage3D<int> s, int3 coords, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedexchange
 * ``int atomicCompareExchange(inout int dest, int compare, int value)``  
 * ``int atomicCompareExchange(storage1D<int> s, int coords, int compare, int value)``  
 * ``int atomicCompareExchange(storage2D<int> s, int2 coords, int compare, int value)``  
 * ``int atomicCompareExchange(storage3D<int> s, int3 coords, int compare, int value)``  
 https://docs.microsoft.com/windows/win32/direct3dhlsl/interlockedcompareexchange

### Techniques

> An effect file can have multiple techniques, each representing a full render pipeline, which is executed to apply post-processing effects. ReShade executes all enabled techniques in the order they were defined in the effect file.
> A technique is made up of one or more passes which contain info about which render states to set and what shaders to execute. They are run sequentially starting with the top most declared. A name is optional.
> Each pass can set render states. The default value is used if one is not specified in the pass body.

Annotations:

 * ``technique Name < enabled = true; >``  
 Enable (or disable if false) this technique by default.
 * ``technique Name < enabled_in_screenshot = true; >``  
 Set this to false to disabled this technique while a screenshot is taken.
 * ``technique Name < timeout = 1000; >``  
 Auto-toggle this technique off 1000 milliseconds after it was enabled.\
 This can for example be used to have a technique run a single time only to do some initialization work, via ``technique Name < enabled = true; timeout = 1; >``
 * ``technique Name < toggle = 0x20; togglectrl = false; toggleshift = false; togglealt = false; >``  
 Toggle this technique when the specified key is pressed.
 * ``technique Name < hidden = true; >``  
 Hide this technique in the UI.
 * ``technique Name < ui_label = "My Effect Name"; >``  
 Uses a custom name for the technique in the UI.
 * ``technique Name < ui_tooltip = "My Effect description"; >``  
 Shows the specified text when the user hovers the technique in the UI.

```hlsl
technique Example < ui_tooltip = "This is an example!"; >
{
	pass p0
	{
		// The primitive topology rendered in the draw call.
		// Available values:
		//   POINTLIST, LINELIST, LINESTRIP, TRIANGLELIST, TRIANGLESTRIP
		PrimitiveTopology = TRIANGLELIST; // or PrimitiveType

		// The number of vertices ReShade generates for the draw call.
		// This has different effects on the rendered primitives based on the primitive topology.
		// A triangle list needs 3 separate vertices for every triangle for example, a strip on the other hand reuses the last 2, so only 1 is needed for every additional triangle.
		VertexCount = 3;

		// The following two accept function names declared above which are used as entry points for the shader.
		// Please note that all parameters must have an associated semantic so the runtime can match them between shader stages.
		VertexShader = ExampleVS;
		PixelShader = ExamplePS0;

		// The number of thread groups to dispatch when a compute shader is used.
		DispatchSizeX = 1;
		DispatchSizeY = 1;
		DispatchSizeZ = 1;

		// Compute shaders are specified with the number of threads per thread group in brackets.
		// The following for example will create groups of 64x1x1 threads:
		ComputeShader = ExampleCS0<64,1,1>;
	
		// RenderTarget0 to RenderTarget7 allow to set one or more render targets for rendering to textures.
		// Set them to a texture name declared above in order to write the color output (SV_Target0 to RenderTarget0, SV_Target1 to RenderTarget1, ...) to this texture in this pass.
		// If multiple render targets are used, the dimensions of them has to match each other.
		// If no render targets are set here, RenderTarget0 points to the backbuffer.
		// Be aware that you can only read **OR** write a texture at the same time, so do not sample from it while it is still bound as render target here.
		// RenderTarget and RenderTarget0 are aliases.
		RenderTarget = texTarget;

		// Set to true to clear all bound render targets to zero before rendering.
		ClearRenderTargets = false;

		// Set to false to disable automatic rebuilding of the mipmap chain of all render targets and/or storage objects.
		// This is useful when using a compute shader that writes to specific mipmap levels, rather than relying on the automatic generation.
		GenerateMipMaps = true;
		
		// A mask applied to the color output before it is written to the render target.
		RenderTargetWriteMask = 0xF; // or ColorWriteEnable
		
		// Enable or disable gamma correction applied to the output.
		SRGBWriteEnable = false;

		// BlendEnable0 to BlendEnable7 allow to enable or disable color and alpha blending for the respective render target.
		// Don't forget to also set "ClearRenderTargets" to "false" if you want to blend with existing data in a render target.
		// BlendEnable and BlendEnable0 are aliases,
		BlendEnable = false;

		// The operator used for color and alpha blending.
		// To set these individually for each render target, append the render target index to the pass state name, e.g. BlendOp3 for the fourth render target (zero-based index 3).
		// Available values:
		//   ADD, SUBTRACT, REVSUBTRACT, MIN, MAX
		BlendOp = ADD;
		BlendOpAlpha = ADD;

		// The data source and optional pre-blend operation used for blending.
		// To set these individually for each render target, append the render target index to the pass state name, e.g. SrcBlend3 for the fourth render target (zero-based index 3).
		// Available values:
		//   ZERO, ONE,
		//   SRCCOLOR, SRCALPHA, INVSRCCOLOR, INVSRCALPHA
		//   DESTCOLOR, DESTALPHA, INVDESTCOLOR, INVDESTALPHA
		SrcBlend = ONE;
		SrcBlendAlpha = ONE;
		DestBlend = ZERO;
		DestBlendAlpha = ZERO;
		
		// Enable or disable the stencil test.
		// The depth and stencil buffers are cleared before rendering each pass in a technique.
		StencilEnable = false;

		// The masks applied before reading from/writing to the stencil.
		// Available values:
		//   0-255
		StencilReadMask = 0xFF; // or StencilMask
		StencilWriteMask = 0xFF;
		
		// The function used for stencil testing.
		// Available values:
		//   NEVER, ALWAYS
		//   EQUAL, NEQUAL or NOTEQUAL
		//   LESS, GREATER, LEQUAL or LESSEQUAL, GEQUAL or GREATEREQUAL
		StencilFunc = ALWAYS;

		// The reference value used with the stencil function.
		StencilRef = 0;
		
		// The operation  to  perform  on  the stencil  buffer when  the
		// stencil  test passed/failed or stencil passed  but depth test
		// failed.
		// Available values:
		//   KEEP, ZERO, REPLACE, INCR, INCRSAT, DECR, DECRSAT, INVERT
		StencilPassOp = KEEP; // or StencilPass
		StencilFailOp = KEEP; // or StencilFail
		StencilDepthFailOp = KEEP; // or StencilZFail
	}
	pass p1
	{
		ComputeShader = ExampleCS1<64,8>;
		DispatchSizeX = 20; // 20 * 64 threads total in X dimension
		DispatchSizeY = 2;  //  2 *  8 threads total in Y dimension
	}
	pass p2
	{
		VertexShader = ExampleVS;
		PixelShader = ExamplePS1;
	}
}
```
