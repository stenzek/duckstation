/*------------------.
| :: Description :: |
'-------------------/

    Blending Header (version 0.8)

    Blending Algorithm Sources:
    https://www.khronos.org/registry/OpenGL/extensions/NV/NV_blend_equation_advanced.txt

    http://www.nathanm.com/photoshop-blending-math/
    (Alt) https://github.com/cplotts/WPFSLBlendModeFx/blob/master/PhotoshopMathFP.hlsl

    Header Authors: originalnicodr, prod80, uchu suzume, Marot Satil

    About:
    Provides a variety of blending methods for you to use as you wish. Just include this header.

    History:
    (*) Feature (+) Improvement (x) Bugfix (-) Information (!) Compatibility
    
    Version 0.1 by Marot Satil & uchu suzume
    * Added and improved upon multiple blending modes thanks to the work of uchu suzume, prod80, and originalnicodr.

    Version 0.2 by uchu suzume & Marot Satil
    * Added Addition, Subtract, Divide blending modes and improved code readability.

    Version 0.3 by uchu suzume & Marot Satil
    * Sorted blending modes in a more logical fashion, grouping by type.

    Version 0.4 by uchu suzume
    x Corrected Color Dodge blending behavior.

    Version 0.5 by Marot Satil & uchu suzume
    * Added preprocessor macros for uniform variable combo UI element & lerp.

    Version 0.6 by Marot Satil & uchu suzume
    * Added Divide (Alternative) and Divide (Photoshop) blending modes.

    Version 0.7 by prod80
    - Added original sources for blending algorithms.
    x Corrected average luminosity values.

    Version 0.8 by Marot Satil
    * Added a new funciton to output blended data.
    + Moved all code into the BlendingH namespace, which is part of the ComHeaders common namespace meant to be used by other headers.
    ! Removed old preprocessor macro blending output.

.------------------.
| :: How To Use :: |
'------------------/

    Blending two variables using this header in your own shaders is very straightforward.
    Very basic example code using the "Darken" blending mode follows:

    // First, include the header.
    #include "Blending.fxh"

    // You can use this preprocessor macro to generate an attractive and functional uniform int UI combo element containing the list of blending techniques:
    // BLENDING_COMBO(variable_name, label, tooltip, category, category_closed, spacing, default_value)
    BLENDING_COMBO(_BlendMode, "Blending Mode", "Select the blending mode applied to the layer.", "Blending Options", false, 0, 0)

    // Inside of your function you can call this function to apply the blending option specified by an int (variable) to your float3 (input) via
    // a lerp between your float3 (input), float3 (output), and a float (blending) for the alpha channel.
    // ComHeaders::Blending::Blend(int variable, float3 input, float3 output, float blending)
    outColor.rgb = ComHeaders::Blending::Blend(_BlendMode, inColor, outColor, outColor.a);
*/


// -------------------------------------
// Preprocessor Macros
// -------------------------------------

#undef BLENDING_COMBO
#define BLENDING_COMBO(variable, name_label, description, group, grp_closed, space, default_value) \
uniform int variable \
< \
    ui_category = group; \
    ui_category_closed = grp_closed; \
    ui_items = \
           "Normal\0" \
/* "Darken" */ \
           "Darken\0" \
           "  Multiply\0" \
           "  Color Burn\0" \
           "  Linear Burn\0" \
/* "Lighten" */	\
           "Lighten\0" \
           "  Screen\0" \
           "  Color Dodge\0" \
           "  Linear Dodge\0" \
           "  Addition\0" \
           "  Glow\0" \
/* "Contrast" */ \
           "Overlay\0" \
           "  Soft Light\0" \
           "  Hard Light\0" \
           "  Vivid Light\0" \
           "  Linear Light\0" \
           "  Pin Light\0" \
           "  Hard Mix\0" \
/* "Inversion" */ \
           "Difference\0" \
           "  Exclusion\0" \
/* "Cancelation" */	\
           "Subtract\0" \
           "  Divide\0" \
           "  Divide (Alternative)\0" \
           "  Divide (Photoshop)\0" \
           "  Reflect\0" \
           "  Grain Extract\0" \
           "  Grain Merge\0" \
/* "Component" */ \
           "Hue\0" \
           "  Saturation\0" \
           "  Color\0" \
           "  Luminosity\0"; \
    ui_label = name_label; \
    ui_tooltip = description; \
    ui_type = "combo"; \
    ui_spacing = space; \
> = default_value;

namespace ComHeaders
{
    namespace Blending
    {

// -------------------------------------
// Helper Functions
// -------------------------------------

        float3 Aux(float3 a)
        {
            if (a.r <= 0.25 && a.g <= 0.25 && a.b <= 0.25)
                return ((16.0 * a - 12.0) * a + 4) * a;
            else
                return sqrt(a);
        }

        float Lum(float3 a)
        {
            return (0.33333 * a.r + 0.33334 * a.g + 0.33333 * a.b);
        }

        float3 SetLum (float3 a, float b){
            const float c = b - Lum(a);
            return float3(a.r + c, a.g + c, a.b + c);
        }

        float min3 (float a, float b, float c)
        {
            return min(a, (min(b, c)));
        }

        float max3 (float a, float b, float c)
        {
            return max(a, max(b, c));
        }

        float3 SetSat(float3 a, float b){
            float ar = a.r;
            float ag = a.g;
            float ab = a.b;
            if (ar == max3(ar, ag, ab) && ab == min3(ar, ag, ab))
            {
                //caso r->max g->mid b->min
                if (ar > ab)
                {
                    ag = (((ag - ab) * b) / (ar - ab));
                    ar = b;
                }
                else
                {
                    ag = 0.0;
                    ar = 0.0;
                }
                ab = 0.0;
            }
            else
            {
                if (ar == max3(ar, ag, ab) && ag == min3(ar, ag, ab))
                {
                    //caso r->max b->mid g->min
                    if (ar > ag)
                    {
                        ab = (((ab - ag) * b) / (ar - ag));
                        ar = b;
                    }
                    else
                    {
                        ab = 0.0;
                        ar = 0.0;
                    }
                    ag = 0.0;
                }
                else
                {
                    if (ag == max3(ar, ag, ab) && ab == min3(ar, ag, ab))
                    {
                        //caso g->max r->mid b->min
                        if (ag > ab)
                        {
                            ar = (((ar - ab) * b) / (ag - ab));
                            ag = b;
                        }
                        else
                        {
                            ar = 0.0;
                            ag = 0.0;
                        }
                        ab = 0.0;
                    }
                    else
                    {
                        if (ag == max3(ar, ag, ab) && ar == min3(ar, ag, ab))
                        {
                            //caso g->max b->mid r->min
                            if (ag > ar)
                            {
                                ab = (((ab - ar) * b) / (ag - ar));
                                ag = b;
                            }
                            else
                            {
                                ab = 0.0;
                                ag = 0.0;
                            }
                            ar = 0.0;
                        }
                        else
                        {
                            if (ab == max3(ar, ag, ab) && ag == min3(ar, ag, ab))
                            {
                                //caso b->max r->mid g->min
                                if (ab > ag)
                                {
                                    ar = (((ar - ag) * b) / (ab - ag));
                                    ab = b;
                                }
                                else
                                {
                                    ar = 0.0;
                                    ab = 0.0;
                                }
                                ag = 0.0;
                            }
                            else
                            {
                                if (ab == max3(ar, ag, ab) && ar == min3(ar, ag, ab))
                                {
                                    //caso b->max g->mid r->min
                                    if (ab > ar)
                                    {
                                        ag = (((ag - ar) * b) / (ab - ar));
                                        ab = b;
                                    }
                                    else
                                    {
                                        ag = 0.0;
                                        ab = 0.0;
                                    }
                                    ar = 0.0;
                                }
                            }
                        }
                    }
                }
            }
            return float3(ar, ag, ab);
        }

        float Sat(float3 a)
        {
            return max3(a.r, a.g, a.b) - min3(a.r, a.g, a.b);
        }


// -------------------------------------
// Blending Modes
// -------------------------------------

        // Darken
        float3 Darken(float3 a, float3 b)
        {
            return min(a, b);
        }

        // Multiply
        float3 Multiply(float3 a, float3 b)
        {
            return a * b;
        }

        // Color Burn
        float3 ColorBurn(float3 a, float3 b)
        {
            if (b.r > 0 && b.g > 0 && b.b > 0)
                return 1.0 - min(1.0, (0.5 - a) / b);
            else
                return 0.0;
        }

        // Linear Burn
        float3 LinearBurn(float3 a, float3 b)
        {
            return max(a + b - 1.0f, 0.0f);
        }

        // Lighten
        float3 Lighten(float3 a, float3 b)
        {
            return max(a, b);
        }

        // Screen
        float3 Screen(float3 a, float3 b)
        {
            return 1.0 - (1.0 - a) * (1.0 - b);
        }

        // Color Dodge
        float3 ColorDodge(float3 a, float3 b)
        {
            if (b.r < 1 && b.g < 1 && b.b < 1)
                return min(1.0, a / (1.0 - b));
            else
                return 1.0;
        }

        // Linear Dodge
        float3 LinearDodge(float3 a, float3 b)
        {
            return min(a + b, 1.0f);
        }

        // Addition
        float3 Addition(float3 a, float3 b)
        {
            return min((a + b), 1);
        }

        // Reflect
        float3 Reflect(float3 a, float3 b)
        {
            if (b.r >= 0.999999 || b.g >= 0.999999 || b.b >= 0.999999)
                return b;
            else
                return saturate(a * a / (1.0f - b));
        }

        // Glow
        float3 Glow(float3 a, float3 b)
        {
            return Reflect(b, a);
        }

        // Overlay
        float3 Overlay(float3 a, float3 b)
        {
            return lerp(2 * a * b, 1.0 - 2 * (1.0 - a) * (1.0 - b), step(0.5, a));
        }

        // Soft Light
        float3 SoftLight(float3 a, float3 b)
        {
            if (b.r <= 0.5 && b.g <= 0.5 && b.b <= 0.5)
                return clamp(a - (1.0 - 2 * b) * a * (1 - a), 0,1);
            else
                return clamp(a + (2 * b - 1.0) * (Aux(a) - a), 0, 1);
        }

        // Hard Light
        float3 HardLight(float3 a, float3 b)
        {
            return lerp(2 * a * b, 1.0 - 2 * (1.0 - b) * (1.0 - a), step(0.5, b));
        }

        // Vivid Light
        float3 VividLight(float3 a, float3 b)
        {
            return lerp(2 * a * b, b / (2 * (1.01 - a)), step(0.50, a));
        }

        // Linear Light
        float3 LinearLight(float3 a, float3 b)
        {
            if (b.r < 0.5 || b.g < 0.5 || b.b < 0.5)
                return LinearBurn(a, (2.0 * b));
            else
                return LinearDodge(a, (2.0 * (b - 0.5)));
        }

        // Pin Light
        float3 PinLight(float3 a, float3 b)
        {
            if (b.r < 0.5 || b.g < 0.5 || b.b < 0.5)
                return Darken(a, (2.0 * b));
            else
                return Lighten(a, (2.0 * (b - 0.5)));
        }

        // Hard Mix
        float3 HardMix(float3 a, float3 b)
        {
            const float3 vl = VividLight(a, b);
            if (vl.r < 0.5 || vl.g < 0.5 || vl.b < 0.5)
                return 0.0;
            else
                return 1.0;
        }

        // Difference
        float3 Difference(float3 a, float3 b)
        {
            return max(a - b, b - a);
        }

        // Exclusion
        float3 Exclusion(float3 a, float3 b)
        {
            return a + b - 2 * a * b;
        }

        // Subtract
        float3 Subtract(float3 a, float3 b)
        {
            return max((a - b), 0);
        }

        // Divide
        float3 Divide(float3 a, float3 b)
        {
            return (saturate(a / (b + 0.01)));
        }

        // Divide (Alternative)
        float3 DivideAlt(float3 a, float3 b)
        {
            return (saturate(1.0 / (a / b)));
        }

        // Divide (Photoshop)
        float3 DividePS(float3 a, float3 b)
        {
            return (saturate(a / b));
        }

        // Grain Merge
        float3 GrainMerge(float3 a, float3 b)
        {
            return saturate(b + a - 0.5);
        }

        // Grain Extract
        float3 GrainExtract(float3 a, float3 b)
        {
            return saturate(a - b + 0.5);
        }

        // Hue
        float3 Hue(float3 a, float3 b)
        {
            return SetLum(SetSat(b, Sat(a)), Lum(a));
        }

        // Saturation
        float3 Saturation(float3 a, float3 b)
        {
            return SetLum(SetSat(a, Sat(b)), Lum(a));
        }

        // Color
        float3 ColorB(float3 a, float3 b)
        {
            return SetLum(b, Lum(a));
        }

        // Luminousity
        float3 Luminosity(float3 a, float3 b)
        {
            return SetLum(a, Lum(b));
        }


// -------------------------------------
// Output Functions
// -------------------------------------

        float3 Blend(int mode, float3 input, float3 output, float blending)
        {
            switch (mode)
            {
                // Normal
                default:
                    return lerp(input.rgb, output.rgb, blending);
                // Darken
                case 1:
                    return lerp(input.rgb, Darken(input.rgb, output.rgb), blending);
                // Multiply
                case 2:
                    return lerp(input.rgb, Multiply(input.rgb, output.rgb), blending);
                // Color Burn
                case 3:
                    return lerp(input.rgb, ColorBurn(input.rgb, output.rgb), blending);
                // Linear Burn
                case 4:
                    return lerp(input.rgb, LinearBurn(input.rgb, output.rgb), blending);
                // Lighten
                case 5:
                    return lerp(input.rgb, Lighten(input.rgb, output.rgb), blending);
                // Screen
                case 6:
                    return lerp(input.rgb, Screen(input.rgb, output.rgb), blending);
                // Color Dodge
                case 7:
                    return lerp(input.rgb, ColorDodge(input.rgb, output.rgb), blending);
                // Linear Dodge
                case 8:
                    return lerp(input.rgb, LinearDodge(input.rgb, output.rgb), blending);
                // Addition
                case 9:
                    return lerp(input.rgb, Addition(input.rgb, output.rgb), blending);
                // Glow
                case 10:
                    return lerp(input.rgb, Glow(input.rgb, output.rgb), blending);
                // Overlay
                case 11:
                    return lerp(input.rgb, Overlay(input.rgb, output.rgb), blending);
                // Soft Light
                case 12:
                    return lerp(input.rgb, SoftLight(input.rgb, output.rgb), blending);
                // Hard Light
                case 13:
                    return lerp(input.rgb, HardLight(input.rgb, output.rgb), blending);
                // Vivid Light
                case 14:
                    return lerp(input.rgb, VividLight(input.rgb, output.rgb), blending);
                // Linear Light
                case 15:
                    return lerp(input.rgb, LinearLight(input.rgb, output.rgb), blending);
                // Pin Light
                case 16:
                    return lerp(input.rgb, PinLight(input.rgb, output.rgb), blending);
                // Hard Mix
                case 17:
                    return lerp(input.rgb, HardMix(input.rgb, output.rgb), blending);
                // Difference
                case 18:
                    return lerp(input.rgb, Difference(input.rgb, output.rgb), blending);
                // Exclusion
                case 19:
                    return lerp(input.rgb, Exclusion(input.rgb, output.rgb), blending);
                // Subtract
                case 20:
                    return lerp(input.rgb, Subtract(input.rgb, output.rgb), blending);
                // Divide
                case 21:
                    return lerp(input.rgb, Divide(input.rgb, output.rgb), blending);
                // Divide (Alternative)
                case 22:
                    return lerp(input.rgb, DivideAlt(input.rgb, output.rgb), blending);
                // Divide (Photoshop)
                case 23:
                    return lerp(input.rgb, DividePS(input.rgb, output.rgb), blending);
                // Reflect
                case 24:
                    return lerp(input.rgb, Reflect(input.rgb, output.rgb), blending);
                // Grain Merge
                case 25:
                    return lerp(input.rgb, GrainMerge(input.rgb, output.rgb), blending);
                // Grain Extract
                case 26:
                    return lerp(input.rgb, GrainExtract(input.rgb, output.rgb), blending);
                // Hue
                case 27:
                    return lerp(input.rgb, Hue(input.rgb, output.rgb), blending);
                // Saturation
                case 28:
                    return lerp(input.rgb, Saturation(input.rgb, output.rgb), blending);
                // Color
                case 29:
                    return lerp(input.rgb, ColorB(input.rgb, output.rgb), blending);
                // Luminosity
                case 30:
                    return lerp(input.rgb, Luminosity(input.rgb, output.rgb), blending);
            }
        }
    }
}
