#ifndef _INPUT_BLURRING_H
#define _INPUT_BLURRING_H

/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2022 Alex Gunter
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


// Theoretically this could go in blurring.fxh
// But that file has a bunch of GPL stuff in it.
// Keeping it separate makes it easier to communicate that this portion is
// available under the MIT license.

#include "../lib/downsampling-functions.fxh"

#include "content-box.fxh"
#include "shared-objects.fxh"


void preblurVertPS(
    in const float4 pos : SV_Position,
    in const float2 texcoord : TEXCOORD0,

    out float4 color : SV_Target
) {
    const float2 texcoord_uncropped = texcoord;

    const float2 max_delta_uv = float2(0.0, rcp(content_size.y)) * preblur_effect_radius;
    const float2 delta_uv = max_delta_uv * rcp(max(preblur_sampling_radius.y, 1));

	color = float4(opaque_linear_downsample(
		ReShade::BackBuffer,
		texcoord_uncropped,
        preblur_sampling_radius.y,
		delta_uv
	), 1);
}

void preblurHorizPS(
    in const float4 pos : SV_Position,
    in const float2 texcoord : TEXCOORD0,

    out float4 color : SV_Target
) {
    const float2 max_delta_uv = float2(rcp(content_size.x), 0.0) * preblur_effect_radius;
    const float2 delta_uv = max_delta_uv * rcp(max(preblur_sampling_radius.x, 1));
	
	color = float4(opaque_linear_downsample(
		samplerPreblurVert,
		texcoord,
        preblur_sampling_radius.x,
		delta_uv
	), 1);
}

#endif  //  _INPUT_BLURRING_H