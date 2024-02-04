#ifndef _BLURRING_H
#define _BLURRING_H

/////////////////////////////  GPL LICENSE NOTICE  /////////////////////////////

//  crt-royale: A full-featured CRT shader, with cheese.
//  Copyright (C) 2014 TroggleMonkey <trogglemonkey@gmx.com>
//
//  crt-royale-reshade: A port of TroggleMonkey's crt-royale from libretro to ReShade.
//  Copyright (C) 2020 Alex Gunter <akg7634@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program; if not, write to the Free Software Foundation, Inc., 59 Temple
//  Place, Suite 330, Boston, MA 02111-1307 USA



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


#include "../lib/gamma-management.fxh"
#include "../lib/blur-functions.fxh"

#include "shared-objects.fxh"


void blurHorizontalVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 blur_dxdy : TEXCOORD1
) {
    PostProcessVS(id, position, texcoord);

    //  Get the uv sample distance between output pixels.  Blurs are not generic
    //  Gaussian resizers, and correct blurs require:
    //  1.) OutputSize == InputSize * 2^m, where m is an integer <= 0.
    //  2.) mipmap_inputN = "true" for this pass in the preset if m != 0
    //  3.) filter_linearN = "true" except for 1x scale nearest neighbor blurs
    //  Gaussian resizers would upsize using the distance between input texels
    //  (not output pixels), but we avoid this and consistently blur at the
    //  destination size.  Otherwise, combining statically calculated weights
    //  with bilinear sample exploitation would result in terrible artifacts.
    static const float2 output_size = TEX_BLURHORIZONTAL_SIZE;
    static const float2 dxdy = 1.0 / output_size;
    //  This blur is vertical-only, so zero out the horizontal offset:
    blur_dxdy = float2(dxdy.x, 0.0);
}

void blurHorizontalPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float2 blur_dxdy : TEXCOORD1,

    out float4 color : SV_Target
) {
    static const float3 blur_color = tex2Dblur9fast(samplerBlurVertical, texcoord, blur_dxdy, get_intermediate_gamma());
    //  Encode and output the blurred image:
    // color = encode_output(float4(blur_color, 1.0), 1.0);
    color = encode_output(float4(blur_color, 1.0), get_intermediate_gamma());
}


void blurVerticalVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 blur_dxdy : TEXCOORD1
) {
    PostProcessVS(id, position, texcoord);

    //  Get the uv sample distance between output pixels.  Blurs are not generic
    //  Gaussian resizers, and correct blurs require:
    //  1.) OutputSize == InputSize * 2^m, where m is an integer <= 0.
    //  2.) mipmap_inputN = "true" for this pass in the preset if m != 0
    //  3.) filter_linearN = "true" except for 1x scale nearest neighbor blurs
    //  Gaussian resizers would upsize using the distance between input texels
    //  (not output pixels), but we avoid this and consistently blur at the
    //  destination size.  Otherwise, combining statically calculated weights
    //  with bilinear sample exploitation would result in terrible artifacts.
    static const float2 output_size = TEX_BLURVERTICAL_SIZE;
    static const float2 dxdy = 1.0 / output_size;
    //  This blur is vertical-only, so zero out the horizontal offset:
    blur_dxdy = float2(0.0, dxdy.y);
}

void blurVerticalPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float2 blur_dxdy : TEXCOORD1,

    out float4 color : SV_Target
) {
    static const float3 blur_color = tex2Dblur9fast(samplerBloomApproxHoriz, texcoord, blur_dxdy, get_intermediate_gamma());
    //  Encode and output the blurred image:
    // color = encode_output(float4(blur_color, 1.0), 1.0);
    color = encode_output(float4(blur_color, 1.0), get_intermediate_gamma());
}

#endif  //  _BLURRING_H