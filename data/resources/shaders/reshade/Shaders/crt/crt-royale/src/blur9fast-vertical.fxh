/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2014 TroggleMonkey
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to
//  deal in the Software without restriction, including without limitation the
//  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
//  sell copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//  
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
//  IN THE SOFTWARE.


/////////////////////////////  SETTINGS MANAGEMENT  ////////////////////////////

//  PASS SETTINGS:
//  gamma-management.h needs to know what kind of pipeline we're using and
//  what pass this is in that pipeline.  This will become obsolete if/when we
//  can #define things like this in the .cgp preset file.
//#define GAMMA_ENCODE_EVERY_FBO
//#define FIRST_PASS
//#define LAST_PASS
//#define SIMULATE_CRT_ON_LCD
//#define SIMULATE_GBA_ON_LCD
//#define SIMULATE_LCD_ON_CRT
//#define SIMULATE_GBA_ON_CRT


//////////////////////////////////  INCLUDES  //////////////////////////////////

#include "../include/gamma-management.fxh"
#include "../include/blur-functions.fxh"

/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p3
{
    float2 blur_dxdy        : TEXCOORD1;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////


// Vertex shader generating a triangle covering the entire screen
void VS_Blur9Fast_Vertical(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p3 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
/*
    float2 texture_size = 1.0/NormalizedNativePixelSize;
    float2 output_size  = (ViewportSize*BufferToViewportRatio);
    float2 video_size   = 1.0/NormalizedNativePixelSize;
*/
//    float2 texture_size = float2(320.0, 240.0);
    float2 texture_size = BLUR9FAST_VERTICAL_texture_size;
    float2 output_size  = VIEWPORT_SIZE;
   // float2 output_size  = VIEWPORT_SIZE/4.0;
//    float2 output_size  = VIEWPORT_SIZE*NormalizedNativePixelSize/float2(320.0, 240.0);
//    float2 output_size  = 1.0/NormalizedNativePixelSize;

	//  Get the uv sample distance between output pixels.  Blurs are not generic
    //  Gaussian resizers, and correct blurs require:
    //  1.) IN.output_size == IN.video_size * 2^m, where m is an integer <= 0.
    //  2.) mipmap_inputN = "true" for this pass in .cgp preset if m != 0
    //  3.) filter_linearN = "true" except for 1x scale nearest neighbor blurs
    //  Gaussian resizers would upsize using the distance between input texels
    //  (not output pixels), but we avoid this and consistently blur at the
    //  destination size.  Otherwise, combining statically calculated weights
    //  with bilinear sample exploitation would result in terrible artifacts.
    const float2 dxdy_scale = video_size/output_size;
	const float2 dxdy = dxdy_scale/texture_size;
    //  This blur is vertical-only, so zero out the horizontal offset:
	OUT.blur_dxdy = float2(0.0, dxdy.y);
}

///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Blur9Fast_Vertical(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p3 VAR) : SV_Target
{
	float3 color = tex2Dblur9fast(BLOOM_APPROX, vTexCoord, VAR.blur_dxdy);
    //  Encode and output the blurred image:
    return encode_output(float4(color, 1.0));
}
