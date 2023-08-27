////////////////////////////////////////////////////////////////////////////////
// Triangular Dither                                                          //
// By The Sandvich Maker                                                      //
// Ported to ReShade by TreyM                                                 //
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Usage:                                                                     //
//   Include this file in your shader like so: #include "TriDither.fx"        //
//                                                                            //
//   For shader developers, use this syntax to do a function call in your     //
//   code as the last thing before exiting a given shader. You should dither  //
//   anytime data is going to be truncated to a lower bitdepth. Color input   //
//   must be a float3 value.                                                  //
//                                                                            //
//  input.rgb += TriDither(input.rgb, uv, bits);                              //
//                                                                            //
//     "bits" is an integer number that determines the bit depth              //
//      being dithered to. Usually 8, sometimes 10                            //
//      You can automate this by letting Reshade decide like so:              //
//                                                                            //
//  input += TriDither(input, uv, BUFFER_COLOR_BIT_DEPTH);                    //
//                                                                            //
//      Manual setup looks something like this for an 8-bit backbuffer:       //
//                                                                            //
//  input.rgb += TriDither(input.rgb, uv, 8);                                 //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

uniform float DitherTimer < source = "timer"; >;
#define       remap(v, a, b) (((v) - (a)) / ((b) - (a)))

float rand21(float2 uv)
{
    float2 noise = frac(sin(dot(uv, float2(12.9898, 78.233) * 2.0)) * 43758.5453);
    return (noise.x + noise.y) * 0.5;
}

float rand11(float x)
{
    return frac(x * 0.024390243);
}

float permute(float x)
{
    return ((34.0 * x + 1.0) * x) % 289.0;
}

float3 TriDither(float3 color, float2 uv, int bits)
{
    float bitstep = exp2(bits) - 1.0;
    float lsb = 1.0 / bitstep;
    float lobit = 0.5 / bitstep;
    float hibit = (bitstep - 0.5) / bitstep;

    float3 m = float3(uv, rand21(uv + (DitherTimer * 0.001))) + 1.0;
    float h = permute(permute(permute(m.x) + m.y) + m.z);

    float3 noise1, noise2;
    noise1.x = rand11(h); h = permute(h);
    noise2.x = rand11(h); h = permute(h);
    noise1.y = rand11(h); h = permute(h);
    noise2.y = rand11(h); h = permute(h);
    noise1.z = rand11(h); h = permute(h);
    noise2.z = rand11(h);

    float3 lo = saturate(remap(color.xyz, 0.0, lobit));
    float3 hi = saturate(remap(color.xyz, 1.0, hibit));
    float3 uni = noise1 - 0.5;
    float3 tri = noise1 - noise2;
	return lerp(uni, tri, min(lo, hi)) * lsb;
}
