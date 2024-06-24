#include "ReShade.fxh"


//    Multi-LUT Shader

//    A simple shader that can load 2 LUTs.
//    Can turn LUT off too.



uniform int LUT_selector <
    ui_type = "combo";
    ui_items = "Off\0Grade-RGB\0Grade-Composite\0";
    ui_label = "LUT selector";
    ui_tooltip = "Off: nothing. Grade-RGB: rgb trinitron colors. Grade-Composite: composite trinitron colors.";
> = 1;


texture tLUT1<source="../Textures/multi-LUT/grade-rgb.png";>{Width=1024;Height=32;};
sampler SamplerLUT1{Texture=tLUT1;};

texture tLUT2<source="../Textures/multi-LUT/grade-composite.png";>{Width=1024;Height=32;};
sampler SamplerLUT2{Texture=tLUT2;};

// This shouldn't be necessary but it seems some undefined values can
// creep in and each GPU vendor handles that differently. This keeps
// all values within a safe range
float4 mixfix(float4 a, float4 b, float c)
{
    return (a.z < 1.0) ? lerp(a, b, c) : a;
}

float4 multiLUT(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
    float4 imgColor = tex2D(ReShade::BackBuffer, vTexCoord.xy);

    if (LUT_selector > 0)
    {
        //float LUT_Size = lerp(textureSize(SamplerLUT1, 0).y, textureSize(SamplerLUT2, 0).y, LUT_selector_param - 1.0);
        float LUT_Size = 32.0;
        float4 color1, color2 = float4(0.,0.,0.,0.);
        float red, green, blue1, blue2, mixer = 0.0;
    
        red = ( imgColor.r * (LUT_Size - 1.0) + 0.4999 ) / (LUT_Size * LUT_Size);
        green = ( imgColor.g * (LUT_Size - 1.0) + 0.4999 ) / LUT_Size;
        blue1 = (floor( imgColor.b  * (LUT_Size - 1.0) ) / LUT_Size) + red;
        blue2 = (ceil( imgColor.b  * (LUT_Size - 1.0) ) / LUT_Size) + red;
        mixer = clamp(max((imgColor.b - blue1) / (blue2 - blue1), 0.0), 0.0, 32.0);
    
        if(LUT_selector == 1)
        {
    	    color1 = tex2D(SamplerLUT1, float2( blue1, green ));
            color2 = tex2D(SamplerLUT1, float2( blue2, green )); 
        }
        else
        {
    	    color1 = tex2D(SamplerLUT2, float2( blue1, green ));
    	    color2 = tex2D(SamplerLUT2, float2( blue2, green ));
        }
        imgColor = mixfix(color1, color2, mixer);
    }

    return imgColor;
}



technique multiLUT
{
    pass PS_multiLUT
    {
    	VertexShader = PostProcessVS;
    	PixelShader  = multiLUT;
    }
}
