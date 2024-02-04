#ifndef _GAMMA_MANAGEMENT_H
#define _GAMMA_MANAGEMENT_H


/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2014 TroggleMonkey
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

#include "helper-functions-and-macros.fxh"


///////////////////////////////  BASE CONSTANTS  ///////////////////////////////

//  Set standard gamma constants, but allow users to override them:
#ifndef OVERRIDE_STANDARD_GAMMA
    //  Standard encoding gammas:
    static const float ntsc_gamma = 2.2;    //  Best to use NTSC for PAL too?
    static const float pal_gamma = 2.8;     //  Never actually 2.8 in practice
    //  Typical device decoding gammas (only use for emulating devices):
    //  CRT/LCD reference gammas are higher than NTSC and Rec.709 video standard
    //  gammas: The standards purposely undercorrected for an analog CRT's
    //  assumed 2.5 reference display gamma to maintain contrast in assumed
    //  [dark] viewing conditions: http://www.poynton.com/PDFs/GammaFAQ.pdf
    //  These unstated assumptions about display gamma and perceptual rendering
    //  intent caused a lot of confusion, and more modern CRT's seemed to target
    //  NTSC 2.2 gamma with circuitry.  LCD displays seem to have followed suit
    //  (they struggle near black with 2.5 gamma anyway), especially PC/laptop
    //  displays designed to view sRGB in bright environments.  (Standards are
    //  also in flux again with BT.1886, but it's underspecified for displays.)
    static const float crt_reference_gamma_high = 2.5;  //  In (2.35, 2.55)
    static const float crt_reference_gamma_low = 2.35;  //  In (2.35, 2.55)
    static const float lcd_reference_gamma = 2.5;       //  To match CRT
    static const float crt_office_gamma = 2.2;  //  Circuitry-adjusted for NTSC
    static const float lcd_office_gamma = 2.2;  //  Approximates sRGB
#endif  //  OVERRIDE_STANDARD_GAMMA

//  Assuming alpha == 1.0 might make it easier for users to avoid some bugs,
//  but only if they're aware of it.
#ifndef OVERRIDE_ALPHA_ASSUMPTIONS
    static const bool assume_opaque_alpha = false;
#endif


///////////////////////  DERIVED CONSTANTS AS FUNCTIONS  ///////////////////////

//  gamma-management.h should be compatible with overriding gamma values with
//  runtime user parameters, but we can only define other global constants in
//  terms of static constants, not uniform user parameters.  To get around this
//  limitation, we need to define derived constants using functions.

//  Set device gamma constants, but allow users to override them:
#if _OVERRIDE_DEVICE_GAMMA
    //  The user promises to globally define the appropriate constants:
    float get_crt_gamma()    {   return crt_gamma;   }
    float get_gba_gamma()    {   return gba_gamma;   }
    float get_lcd_gamma()    {   return lcd_gamma;   }
#else
    float get_crt_gamma()    {   return crt_reference_gamma_high;    }
    float get_gba_gamma()    {   return 3.5; }   //  Game Boy Advance; in (3.0, 4.0)
    float get_lcd_gamma()    {   return lcd_office_gamma;            }
#endif  //  _OVERRIDE_DEVICE_GAMMA

//  Set decoding/encoding gammas for the first/lass passes, but allow overrides:
#ifdef OVERRIDE_FINAL_GAMMA
    //  The user promises to globally define the appropriate constants:
    float get_intermediate_gamma()   {   return intermediate_gamma;  }
    float get_input_gamma()          {   return input_gamma;         }
    float get_output_gamma()         {   return output_gamma;        }
#else
    //  If we gamma-correct every pass, always use ntsc_gamma between passes to
    //  ensure middle passes don't need to care if anything is being simulated:

    // TODO: Figure out the correct way to configure this now that intermediate
    //   FBOs all use get_intermediate_gamma() directly. Also refer to the
    //   original code to confirm when a shader uses ntsc_gamma despite
    //   GAMMA_ENCODE_EVERY_FBO being undefined.
    // float get_intermediate_gamma()   {   return ntsc_gamma;          }
    float get_intermediate_gamma()   {   return 1.0;                 }
    
    #if GAMMA_SIMULATION_MODE == _SIMULATE_CRT_ON_LCD
        float get_input_gamma()      {   return get_crt_gamma();     }
        float get_output_gamma()     {   return get_lcd_gamma();     }
    #else
    #if GAMMA_SIMULATION_MODE == _SIMULATE_GBA_ON_LCD
        float get_input_gamma()      {   return get_gba_gamma();     }
        float get_output_gamma()     {   return get_lcd_gamma();     }
    #else
    #if GAMMA_SIMULATION_MODE == _SIMULATE_LCD_ON_CRT
        float get_input_gamma()      {   return get_lcd_gamma();     }
        float get_output_gamma()     {   return get_crt_gamma();     }
    #else
    #if GAMMA_SIMULATION_MODE == _SIMULATE_GBA_ON_CRT
        float get_input_gamma()      {   return get_gba_gamma();     }
        float get_output_gamma()     {   return get_crt_gamma();     }
    #else   //  Don't simulate anything:
        float get_input_gamma()      {   return ntsc_gamma;          }
        float get_output_gamma()     {   return ntsc_gamma;          }
    #endif  //  _SIMULATE_GBA_ON_CRT
    #endif  //  _SIMULATE_LCD_ON_CRT
    #endif  //  _SIMULATE_GBA_ON_LCD
    #endif  //  _SIMULATE_CRT_ON_LCD
#endif  //  OVERRIDE_FINAL_GAMMA


//  Set decoding/encoding gammas for the current pass.  Use static constants for
//  linearize_input and gamma_encode_output, because they aren't derived, and
//  they let the compiler do dead-code elimination.
// #ifndef GAMMA_ENCODE_EVERY_FBO
//     #ifdef FIRST_PASS
//         static const bool linearize_input = true;
//         float get_pass_input_gamma()     {   return get_input_gamma();   }
//     #else
//         static const bool linearize_input = false;
//         float get_pass_input_gamma()     {   return 1.0;                 }
//     #endif
//     #ifdef LAST_PASS
//         static const bool gamma_encode_output = true;
//         float get_pass_output_gamma()    {   return get_output_gamma();  }
//     #else
//         static const bool gamma_encode_output = false;
//         float get_pass_output_gamma()    {   return 1.0;                 }
//     #endif
// #else
//     static const bool linearize_input = true;
//     static const bool gamma_encode_output = true;
//     #ifdef FIRST_PASS
//         float get_pass_input_gamma()     {   return get_input_gamma();   }
//     #else
//         float get_pass_input_gamma()     {   return get_intermediate_gamma();    }
//     #endif
//     #ifdef LAST_PASS
//         float get_pass_output_gamma()    {   return get_output_gamma();  }
//     #else
//         float get_pass_output_gamma()    {   return get_intermediate_gamma();    }
//     #endif
// #endif

//  Users might want to know if bilinear filtering will be gamma-correct:
// static const bool gamma_aware_bilinear = !linearize_input;


//////////////////////  COLOR ENCODING/DECODING FUNCTIONS  /////////////////////

float4 encode_output_opaque(const float4 color, const float gamma)
{
    static const float3 g = 1.0 / float3(gamma, gamma, gamma);
    return float4(pow(color.rgb, g), 1);
}

float4 decode_input_opaque(const float4 color, const float gamma)
{
    static const float3 g = float3(gamma, gamma, gamma);
    return float4(pow(color.rgb, g), 1);
}

float4 encode_output(const float4 color, const float gamma)
{
    static const float3 g = 1.0 / float3(gamma, gamma, gamma);
    return float4(pow(color.rgb, g), color.a);
}

float4 decode_input(const float4 color, const float gamma)
{
    static const float3 g = float3(gamma, gamma, gamma);
    return float4(pow(color.rgb, g), color.a);
}

///////////////////////////  TEXTURE LOOKUP WRAPPERS  //////////////////////////

//  "SMART" LINEARIZING TEXTURE LOOKUP FUNCTIONS:
//  Provide a wide array of linearizing texture lookup wrapper functions.  The
//  Cg shader spec Retroarch uses only allows for 2D textures, but 1D and 3D
//  lookups are provided for completeness in case that changes someday.  Nobody
//  is likely to use the *fetch and *proj functions, but they're included just
//  in case.  The only tex*D texture sampling functions omitted are:
//      - tex*Dcmpbias
//      - tex*Dcmplod
//      - tex*DARRAY*
//      - tex*DMS*
//      - Variants returning integers
//  Standard line length restrictions are ignored below for vertical brevity.

//  tex2D:
float4 tex2D_linearize(const sampler2D tex, const float2 tex_coords, const float gamma)
{   return decode_input(tex2D(tex, tex_coords), gamma);   }

float4 tex2D_linearize(const sampler2D tex, const float3 tex_coords, const float gamma)
{   return decode_input(tex2D(tex, tex_coords.xy), gamma);   }

// float4 tex2D_linearize(const sampler2D tex, const float2 tex_coords, const int texel_off, const float gamma)
// {   return decode_input(tex2Dlod(tex, float4(tex_coords.x, tex_coords.y, 0, 0), texel_off), gamma);    }

// float4 tex2D_linearize(const sampler2D tex, const float3 tex_coords, const int texel_off, const float gamma)
// {   return decode_input(tex2Dlod(tex, float4(tex_coords.x, tex_coords.y, 0, 0), texel_off), gamma);    }

//  tex2Dlod:
float4 tex2Dlod_linearize(const sampler2D tex, const float2 tex_coords, const float gamma)
{   return decode_input(tex2Dlod(tex, float4(tex_coords, 0, 0), 0.0), gamma);    }

float4 tex2Dlod_linearize(const sampler2D tex, const float4 tex_coords, const float gamma)
{   return decode_input(tex2Dlod(tex, float4(tex_coords.xy, 0, 0), 0.0), gamma);    }

// float4 tex2Dlod_linearize(const sampler2D tex, const float4 tex_coords, const int texel_off, const float gamma)
// {   return decode_input(tex2Dlod(tex, float4(tex_coords.x, tex_coords.y, 0, 0), texel_off), gamma);     }

#endif  //  _GAMMA_MANAGEMENT_H