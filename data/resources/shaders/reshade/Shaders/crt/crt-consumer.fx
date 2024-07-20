#include "ReShade.fxh"


/*
   CRT-Consumer

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/



uniform float PRE_SCALE <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 4.0;
	ui_step = 0.1;
	ui_label = "Pre-Scale Sharpening";
> = 1.5;

uniform float blurx <
	ui_type = "drag";
	ui_min = -4.0;
	ui_max = 4.0;
	ui_step = 0.05;
	ui_label = "Convergence X";
> = 0.25;

uniform float blury <
	ui_type = "drag";
	ui_min = -4.0;
	ui_max = 4.0;
	ui_step = 0.05;
	ui_label = "Convergence Y";
> = -0.1;

uniform float warpx <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.12;
	ui_step = 0.01;
	ui_label = " Curvature X";
> = 0.03;

uniform float warpy <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.12;
	ui_step = 0.01;
	ui_label = " Curvature Y";
> = 0.04;

uniform float corner <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.10;
	ui_step = 0.01;
	ui_label = " Corner size";
> = 0.03;

uniform float smoothness <
	ui_type = "drag";
	ui_min = 100.0;
	ui_max = 600.0;
	ui_step = 5.0;
	ui_label = " Border Smoothness";
> = 400.0;

uniform bool inter <
	ui_type = "radio";
	ui_label = "Interlacing Toggle";
> = true;

uniform float Downscale <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 8.0;
	ui_step = 1.;
	ui_label = "Interlacing Downscale Scanlines";
> = 2.0;

uniform float scanlow <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 15.0;
	ui_step = 1.0;
	ui_label = "Beam low";
> = 6.0;

uniform float scanhigh <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 15.0;
	ui_step = 1.0;
	ui_label = "Beam high";
> = 8.0;

uniform float beamlow <
	ui_type = "drag";
	ui_min = 0.5;
	ui_max = 2.5;
	ui_step = 0.05;
	ui_label = "Scanlines dark";
> = 1.45;

uniform float beamhigh <
	ui_type = "drag";
	ui_min = 0.5;
	ui_max = 2.5;
	ui_step = 0.05;
	ui_label = "Scanlines bright";
> = 1.05;

uniform float preserve <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
	ui_label = "Protect White On Masks";
> = 0.98;

uniform float brightboost1 <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 3.0;
	ui_step = 0.05;
	ui_label = "Bright boost dark pixels";
> = 1.25;

uniform float brightboost2 <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 3.0;
	ui_step = 0.05;
	ui_label = "Bright boost bright pixels";
> = 1.0;

uniform float glow <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 6.0;
	ui_step = 1.0;
	ui_label = "Glow pixels per axis";
> = 3.0;

uniform float quality <
	ui_type = "drag";
	ui_min = 0.25;
	ui_max = 4.0;
	ui_step = 0.05;
	ui_label = "Glow quality";
> = 1.0;

uniform float glow_str <
	ui_type = "drag";
	ui_min = 0.0001;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Glow intensity";
> = 0.3;

uniform float nois <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 32.0;
	ui_step = 1.0;
	ui_label = "Add Noise";
> = 0.0;

uniform float postbr <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.5;
	ui_step = 0.02;
	ui_label = "Post Brightness";
> = 1.0;

uniform float palette_fix <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 1.0;
	ui_label = "Palette Fixes. Sega, PUAE Atari ST dark colors";
> = 0.0;

uniform float Shadowmask <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 8.0;
	ui_step = 1.;
	ui_label = "Mask Type";
> = 0.0;

uniform float masksize <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 2.0;
	ui_step = 1.0;
	ui_label = "Mask Size";
> = 1.0;

uniform float MaskDark <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.1;
	ui_label = "Mask dark";
> = 0.2;

uniform float MaskLight <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.1;
	ui_label = "Mask light";
> = 1.5;

uniform float slotmask <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Slot Mask Strength";
> = 0.0;

uniform float slotwidth <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 6.0;
	ui_step = 0.5;
	ui_label = "Slot Mask Width";
> = 2.0;

uniform float double_slot <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 2.0;
	ui_step = 1.0;
	ui_label = "Slot Mask Height: 2x1 or 4x1";
> = 1.0;

uniform float slotms <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 2.0;
	ui_step = 1.0;
	ui_label = "Slot Mask Size";
> = 1.0;

uniform float GAMMA_OUT <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 4.0;
	ui_step = 0.05;
	ui_label = "Gamma Out";
> = 2.25;

uniform float sat <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Saturation";
> = 1.0;

uniform float contrast <
	ui_type = "drag";
	ui_min = 0.00;
	ui_max = 2.00;
	ui_step = 0.05;
	ui_label = "Contrast, 1.0:Off";
> = 1.0;

uniform float WP <
	ui_type = "drag";
	ui_min = -100.0;
	ui_max = 100.0;
	ui_step = 5.;
	ui_label = "Color Temperature %";
> = 0.0;

uniform float rg <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Red-Green Tint";
> = 0.0;

uniform float rb <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Red-Blue Tint";
> = 0.0;

uniform float gr <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Green-Red Tint";
> = 0.0;

uniform float gb <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Green-Blue Tint";
> = 0.0;

uniform float br <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Blue-Red Tint";
> = 0.0;

uniform float bg <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Blue-Green Tint";
> = 0.0;

uniform bool vignette <
	ui_type = "radio";
	ui_label = "Vignette On/Off";
> = false;

uniform float vpower <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
	ui_label = "Vignette Power";
> = 0.15;

uniform float vstr <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 50.0;
	ui_step = 1.0;
	ui_label = "Vignette strength";
> = 40.0;

uniform bool alloff <
	ui_type = "radio";
	ui_label = "Switch off shader";
> = false;


uniform float  FrameCount < source = "framecount"; >;
uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;
uniform float  ViewportX < source = "viewportx"; >;
uniform float  ViewportY < source = "viewporty"; >;
uniform float  ViewportWidth < source = "viewportwidth"; >;
uniform float  ViewportHeight < source = "viewportheight"; >;
uniform float2 ViewportOffset < source = "viewportoffset"; >;
uniform float  BufferWidth < source = "bufferwidth"; >;
uniform float  BufferHeight < source = "bufferheight"; >;
uniform float  NativeWidth < source = "nativewidth"; >;
uniform float  NativeHeight < source = "nativeheight"; >;
uniform float  InternalWidth < source = "internalwidth"; >;
uniform float  InternalHeight < source = "internalheight"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

#define iTime (float(FrameCount)/2.0)
#define iTimer (float(FrameCount)/60.0)

#define SourceSize (float4(1.0/NormalizedNativePixelSize,NormalizedNativePixelSize))
#define OutputSize (ViewportSize*BufferToViewportRatio)

float2 Warp(float2 pos)
{
    pos  = pos * 2.0 - 1.0;    
    pos *= float2(1.0 + (pos.y * pos.y) * warpx, 1.0 + (pos.x * pos.x) * warpy);
    return pos * 0.5 + 0.5;
} 

float sw(float y, float l)
{
    float beam = lerp(scanlow, scanhigh, y);
    float scan = lerp(beamlow,  beamhigh, l);
    float ex = y * scan;
    return exp2(-beam * ex * ex);
}

float3 mask(float2 x, float3 col, float l)
{
    x = floor(x / masksize);        
  
    if (Shadowmask == 0.0)
    {
        float m = frac(x.x * 0.4999);
        if (m < 0.4999) return float3(1.0,             MaskDark, 1.0);
        else            return float3(MaskDark, 1.0,             MaskDark);
    }
   
    else if (Shadowmask == 1.0)
    {
        float3 Mask = float3(MaskDark, MaskDark, MaskDark);
        float line = MaskLight;
        float odd  = 0.0;

        if (frac(x.x / 6.0) < 0.5) odd = 1.0;
        if (frac((x.y + odd) / 2.0) < 0.5) line = MaskDark;

        float m = frac(x.x / 3.0);
        if      (m < 0.333) Mask.b = MaskLight;
        else if (m < 0.666) Mask.g = MaskLight;
        else                Mask.r = MaskLight;
        
        Mask *= line; 
        return Mask; 
    } 
    
    else if (Shadowmask == 2.0)
    {
        float m = frac(x.x*0.3333);
        if (m < 0.3333) return float3(MaskDark,  MaskDark,  MaskLight);
        if (m < 0.6666) return float3(MaskDark,  MaskLight, MaskDark);
        else            return float3(MaskLight, MaskDark,  MaskDark);
    }

    if (Shadowmask == 3.0)
    {
        float m = frac(x.x * 0.5);
        if (m < 0.5) return float3(1.0, 1.0, 1.0);
        else         return float3(MaskDark, MaskDark, MaskDark);
    }
   
    else if (Shadowmask == 4.0)
    {   
        float3 Mask = float3(col.rgb);
        float line = MaskLight;
        float odd  = 0.0;

        if (frac(x.x / 4.0) < 0.5) odd = 1.0;
        if (frac((x.y + odd) / 2.0) < 0.5) line = MaskDark;

        float m = frac(x.x / 2.0);
        if  (m < 0.5) { Mask.r = 1.0; Mask.b = 1.0; }
        else  Mask.g = 1.0;   

        Mask *= line;  
        return Mask;
    } 

    else if (Shadowmask == 5.0)
    {
        float3 Mask = float3(1.0, 1.0, 1.0);

        if (frac(x.x / 4.0) < 0.5)   
        {
            if (frac(x.y / 3.0) < 0.666)
            {
                if (frac(x.x / 2.0) < 0.5) Mask = float3(1.0,             MaskDark, 1.0);
                else                        Mask = float3(MaskDark, 1.0,             MaskDark);
            }
            else Mask *= l;
        }
        else if (frac(x.x / 4.0) >= 0.5)   
        {
            if (frac(x.y / 3.0) > 0.333) 
            {
                if (frac(x.x / 2.0) < 0.5) Mask = float3(1.0,             MaskDark, 1.0); 
                else                        Mask = float3(MaskDark, 1.0,             MaskDark);
            }
            else Mask *= l;
        }

        return Mask;
    }

    else if (Shadowmask == 6.0)
    {
        float3 Mask = float3(MaskDark, MaskDark, MaskDark);
        if (frac(x.x / 6.0) < 0.5)   
        {
            if (frac(x.y / 4.0) < 0.75)  
            {
                if      (frac(x.x / 3.0) < 0.3333) Mask.r = MaskLight; 
                else if (frac(x.x / 3.0) < 0.6666) Mask.g = MaskLight; 
                else                                Mask.b = MaskLight;
            }
            else Mask * l * 0.9;
        }
        else if (frac(x.x / 6.0) >= 0.5)   
        {
            if (frac(x.y / 4.0) >= 0.5 || frac(x.y / 4.0) < 0.25)  
            {
                if      (frac(x.x / 3.0) < 0.3333) Mask.r = MaskLight; 
                else if (frac(x.x / 3.0) < 0.6666) Mask.g = MaskLight;
                else                                Mask.b = MaskLight;
            }
            else Mask * l * 0.9;
        }
        return Mask;
    }

    else if (Shadowmask == 7.0)
    {
        float m = frac(x.x * 0.3333);

        if (m < 0.3333) return float3(MaskDark,          MaskLight,         MaskLight * col.b); //Cyan
        if (m < 0.6666) return float3(MaskLight * col.r, MaskDark,          MaskLight);         //Magenta
        else            return float3(MaskLight,         MaskLight * col.g, MaskDark);          //Yellow
    }

    else if (Shadowmask == 8.0)
    {
        float3 Mask = float3(MaskDark, MaskDark, MaskDark);

        float bright = MaskLight;
        float left   = 0.0;
        if (frac(x.x / 6.0) < 0.5) left = 1.0;
             
        float m = frac(x.x / 3.0);
        if      (m < 0.333) Mask.b = 0.9;
        else if (m < 0.666) Mask.g = 0.9;
        else                Mask.r = 0.9;
        
        if ((x.y % 2.0) == 1.0 && left == 1.0 || (x.y % 2.0) == 0.0 && left == 0.0) 
            Mask *= bright; 
      
        return Mask; 
    } 
    
    else return float3(1.0, 1.0, 1.0);
}

float SlotMask(float2 pos, float3 c)
{
    if (slotmask == 0.0) return 1.0;
    
    pos = floor(pos / slotms);
    float mx = pow(max(max(c.r, c.g), c.b), 1.33);
    float mlen = slotwidth * 2.0;
    float px = frac(pos.x / mlen);
    float py = floor(frac(pos.y / (2.0 * double_slot)) * 2.0 * double_slot);
    float slot_dark = lerp(1.0 - slotmask, 1.0 - 0.80 * slotmask, mx);
    float slot = 1.0 + 0.7 * slotmask * (1.0 - mx);
    
    if      (py == 0.0                && px <  0.5) slot = slot_dark; 
    else if (py == double_slot && px >= 0.5) slot = slot_dark;       
    
    return slot;
}

float4x4 contrastMatrix(float contrast)
{   
    float t = (1.0 - contrast) / 2.0;
    
    return float4x4(contrast, 0,               0,               0,
                0,               contrast, 0,               0,
                0,               0,               contrast, 0,
                t,               t,               t,               1);
}

float3x3 vign(float l, float2 tex)
{
    float2 vpos = tex;
    vpos *= 1.0 - vpos.xy;
    
    float vig = vpos.x * vpos.y * vstr;
    vig = min(pow(vig, vpower), 1.0); 
    if (vignette == false) vig = 1.0;
   
    return float3x3(vig, 0,   0,
                0,   vig, 0,
                0,   0,   vig);
}

float3 saturation(float3 textureColor)
{
    float luminance = length(textureColor.rgb) * 0.5775;

    float3 luminanceWeighting = float3(0.4, 0.5, 0.1);
    if (luminance < 0.5) luminanceWeighting.rgb = (luminanceWeighting.rgb * luminanceWeighting.rgb) 
                                                + (luminanceWeighting.rgb * luminanceWeighting.rgb);

    luminance = dot(textureColor.rgb, luminanceWeighting);
    float3 greyScaleColor = float3(luminance, luminance, luminance);

    float3 res = float3(lerp(greyScaleColor, textureColor.rgb, sat));
    return res;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

float3 glow0 (float2 texcoord, float3 col)
{

   // the more quality, the smaller the offset and better quality, less visible glow too
     float2 size = SourceSize.zw/quality;
     
     float3 c01;
     float3 sum = float3(0.0, 0.0, 0.0);
   
   // glow = pixels per axis, the more the slower!

    for (float x = -glow; x <= glow; x = x+1.0)
     {

   // multiply texture, the more far away the less pronounced
        float factor = 1.0/glow;
        for (float y = -glow; y <= glow; y = y+1.0)
        {

        float2 offset = float2(x, y) * size;

         c01 = tex2D(sBackBuffer, texcoord + offset).rgb*factor; c01 = c01*c01;
          
                sum += c01;
        }
    }
  
    return (glow_str * sum / (glow * glow )) ;
}
    
///////////////////////////////////////////////////////////////////////////////////////////////////////////

float noise(float2 co)
{
    return frac(sin(iTimer * dot(co.xy ,float2(12.9898,78.233))) * 43758.5453);
}

float corner0(float2 coord)
{
    coord = (coord - float2(0.5, 0.5)) * 1.0 + float2(0.5, 0.5);
    coord = min(coord, float2(1.0, 1.0) - coord) * float2(1.0, SourceSize.y / SourceSize.x);
    
    float2 cdist = float2(corner, corner);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));

    return clamp((cdist.x - dist) * smoothness, 0.0, 1.0);
}  

static const float3x3 D65_to_XYZ = float3x3(
           0.4306190,  0.2220379,  0.0201853,
           0.3415419,  0.7066384,  0.1295504,
           0.1783091,  0.0713236,  0.9390944);

static const float3x3 XYZ_to_D65 = float3x3(
           3.0628971, -0.9692660,  0.0678775,
          -1.3931791,  1.8760108, -0.2288548,
          -0.4757517,  0.0415560,  1.0693490);
           
static const float3x3 D50_to_XYZ = float3x3(
           0.4552773,  0.2323025,  0.0145457,
           0.3675500,  0.7077956,  0.1049154,
           0.1413926,  0.0599019,  0.7057489);
           
static const float3x3 XYZ_to_D50 = float3x3(
           2.9603944, -0.9787684,  0.0844874,
          -1.4678519,  1.9161415, -0.2545973,
          -0.4685105,  0.0334540,  1.4216174);         


float4 PS_CRT_CONSUMER(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
    float2 pos = Warp(vTexCoord.xy);
    float2 tex_size = SourceSize.xy;  

    float2 pC4 = (pos + 0.5/tex_size);
    float2 fp = frac(pos * tex_size);
    if (inter == false && tex_size.y > 400.0){ fp.y = frac(pos.y * tex_size.y*1.0/Downscale);} 

    float4 res = float4(1.0, 1.0, 1.0, 1.0);
    
    if (alloff == true) 
        res = tex2D(sBackBuffer, pC4); 
    else
    {

   float2 texel = pos * tex_size;
   float2 texel_floored = floor(texel);

   float scale = PRE_SCALE;
   float region_range = 0.5 - 0.5 / scale;

   // Figure out where in the texel to sample to get correct pre-scaled bilinear.
   // Uses the hardware bilinear interpolator to avoid having to sample 4 times manually.

   float2 center_dist = fp - 0.5;

   float2 fpp = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;

   float2 mod_texel = texel_floored + fpp;
   float2 coords = mod_texel / SourceSize.xy;

        float3 sample1 = tex2D(sBackBuffer, float2(coords.x + blurx*SourceSize.z, coords.y - blury*SourceSize.w)).rgb;
        float3 sample2 = tex2D(sBackBuffer, coords).rgb;
        float3 sample3 = tex2D(sBackBuffer, float2(coords.x - blurx*SourceSize.z, coords.y + blury*SourceSize.w )).rgb;
        
        float3 color = float3(sample1.r * 0.5  + sample2.r * 0.5, 
                          sample1.g * 0.25 + sample2.g * 0.5 + sample3.g * 0.25,
                          sample2.b * 0.5  + sample3.b * 0.5);
        if (palette_fix != 0.0) 
        {
            if (palette_fix == 1.0) color = color* 1.0667;
            else if (palette_fix == 2.0) color = color * 2.0;
        }

        //COLOR TEMPERATURE FROM GUEST.R-DR.VENOM
        if (WP != 0.0)
        {
            float3 warmer = mul(color, D50_to_XYZ);
            warmer = mul(warmer, XYZ_to_D65); 
            
            float3 cooler = mul(color, D65_to_XYZ);
            cooler = mul(cooler, XYZ_to_D50);
            
            float m = abs(WP) / 100.0;
            float3 comp = (WP < 0.0) ? cooler : warmer;
            comp = clamp(comp, 0.0, 1.0);   
            
            color = float3(lerp(color, comp, m));
        }

     float3x3 hue = float3x3 (1., rg,  rb,                 //red tint
                      gr,  1., gb,                  //green tint
                      br,  bg,  1.);                //blue tint

        color = mul(color, hue);

        color = (2.0*pow(color,float3(2.8, 2.8, 2.8))) - pow(color,float3(3.6, 3.6, 3.6));

        float lum = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;

        float f = frac(fp.y -0.5);
        
        if (inter == true && tex_size.y > 400.0) color = color; 
        else
        {color = color * sw(f,lum) + color * sw (1.0-f,lum);}
        
        float lum1 = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;

        
        color *= lerp(mask((vTexCoord * OutputSize.xy), color,lum1), float3(1.0, 1.0, 1.0), lum1*preserve);
        

        if (slotmask != 0.0) color *= SlotMask((vTexCoord * OutputSize.xy) * 1.0001, color);
        
        color *= lerp(brightboost1, brightboost2, max(max(color.r, color.g), color.b));    

    

        color = pow(color,float3(1.0 / GAMMA_OUT, 1.0 / GAMMA_OUT, 1.0 / GAMMA_OUT));
                if (glow_str != 0.0) color += glow0(coords,color);

        if (sat    != 1.0) color  = saturation(color);
        if (corner != 0.0) color *= corner0(pC4);
        if (nois   != 0.0) color *= 1.0 + noise(coords * 2.0) / nois;

        color *= lerp(1.0, postbr, lum);
        res = float4(color, 1.0);
        if (contrast != 1.0) res = mul(res, contrastMatrix(contrast));
        if (inter == true && SourceSize.y > 400.0 && frac(iTime) < 0.5) res = res * 0.95;
        res.rgb = mul(res.rgb, vign(lum, vTexCoord));

    }
    
    return res;
}



technique CRT_CONSUMER
{
   pass
   {
   	VertexShader = PostProcessVS;
   	PixelShader  = PS_CRT_CONSUMER;
   }
}
