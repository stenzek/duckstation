#ifndef _HELPER_FUNCTIONS_AND_MACROS_H
#define _HELPER_FUNCTIONS_AND_MACROS_H

/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2020 Alex Gunter
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


float4 tex2D_nograd(sampler2D tex, float2 tex_coords)
{
    return tex2Dlod(tex, float4(tex_coords, 0, 0), 0.0);
}

// ReShade 4 does not permit the use of functions or the ternary operator
// outside of a function definition. This is a problem for this port
// because the original crt-royale shader makes heavy use of these
// constructs at the root level.

// These preprocessor definitions are a workaround for this limitation.
// Note that they are strictly intended for defining complex global
// constants. I doubt they're more performant than the built-in
// equivalents, so I recommend using the built-ins whenever you can.


#define macro_sign(c) -((int) ((c) != 0)) * -((int) ((c) > 0))
#define macro_abs(c) (c) * macro_sign(c)

#define macro_min(c, d) (c) * ((int) ((c) <= (d))) + (d) * ((int) ((c) > (d)))
#define macro_max(c, d) (c) * ((int) ((c) >= (d))) + (d) * ((int) ((c) < (d)))
#define macro_clamp(c, l, u) macro_min(macro_max(c, l), u)

#define macro_ceil(c) (float) ((int) (c) + (int) (((int) (c)) < (c)))

#define macro_cond(c, a, b) float(c) * (a) + float(!(c)) * (b)



////////////////////////  COMMON MATHEMATICAL CONSTANTS  ///////////////////////

static const float pi = 3.141592653589;
//  We often want to find the location of the previous texel, e.g.:
//      const float2 curr_texel = uv * texture_size;
//      const float2 prev_texel = floor(curr_texel - float2(0.5)) + float2(0.5);
//      const float2 prev_texel_uv = prev_texel / texture_size;
//  However, many GPU drivers round incorrectly around exact texel locations.
//  We need to subtract a little less than 0.5 before flooring, and some GPU's
//  require this value to be farther from 0.5 than others; define it here.
//      const float2 prev_texel =
//          floor(curr_texel - float2(under_half)) + float2(0.5);
static const float under_half = 0.4995;

//  Avoid dividing by zero; using a macro overloads for float, float2, etc.:
#define FIX_ZERO(c) (macro_max(macro_abs(c), 0.0000152587890625))   //  2^-16

// #define fmod(x, y) ((x) - (y) * floor((x)/(y) + FIX_ZERO(0.0)))
#define fmod(x, y) (frac((x) / (y)) * (y))

#endif  //  _HELPER_FUNCTIONS_AND_MACROS_H