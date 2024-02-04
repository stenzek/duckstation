#ifndef _DOWNSAMPLING_FUNCTIONS_H
#define _DOWNSAMPLING_FUNCTIONS_H

/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2020 Alex Gunter <akg7634@gmail.com>
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

float3 opaque_linear_downsample(
    const sampler2D tex,
    const float2 texcoord,
    const uint num_pairs,
    const float2 delta_uv
) {
    const uint total_num_samples = num_pairs * 2 + 1;
    const float2 coord_left = texcoord - delta_uv * num_pairs;

    float3 acc = 0;
    for(int i = 0; i < total_num_samples; i++) {
        const float2 coord = coord_left + i * delta_uv;
        acc += tex2D_nograd(tex, coord).rgb;
    }
    
    return acc / total_num_samples;
}


float3 opaque_lanczos_downsample(
    const sampler2D tex,
    const float2 texcoord,
    const uint num_pairs,
    const float2 delta_uv,
    const float num_sinc_lobes,
    const float weight_at_center
) {
    const uint total_num_samples = num_pairs * 2 + 1;
    const float2 coord_left = texcoord - delta_uv * num_pairs;
    const float sinc_dx = num_sinc_lobes / num_pairs;  // 2 * num_sinc_lobes / (total_num_samples - 1)

    float3 acc = 0;
    float w_sum = 0;
    for(int i = 0; i < total_num_samples; i++) {
        const float2 coord = coord_left + i * delta_uv;
        const float sinc_x = i * sinc_dx;

        const float weight = (i != num_pairs) ?
            num_sinc_lobes * sin(pi*sinc_x) * sin(pi*sinc_x/num_sinc_lobes) / (pi*pi * sinc_x*sinc_x) :
            weight_at_center;

        acc += weight * tex2D_nograd(tex, coord).rgb;
        w_sum += weight;
    }
    
    return acc / w_sum;
}

float3 opaque_lanczos_downsample(
    const sampler2D tex,
    const float2 texcoord,
    const uint num_pairs,
    const float2 delta_uv,
    const float num_sinc_lobes
) {
    return opaque_lanczos_downsample(tex, texcoord, num_pairs, delta_uv, num_sinc_lobes, 1);
}

#endif  //  _DOWNSAMPLING_FUNCTIONS_H