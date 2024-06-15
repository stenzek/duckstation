//   Crt-Consumer

//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License
//   as published by the Free Software Foundation; either version 2
//   of the License, or (at your option) any later version.

//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.

//   You should have received a copy of the GNU General Public License
//   along with this program; if not, write to the Free Software
//   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.



/*
[configuration]


[OptionRangeFloat]
GUIName = Pre-Scale Sharpening
OptionName = PRE_SCALE
MinValue = 1.0
MaxValue = 4.0
StepAmount = 0.1
DefaultValue = 1.5

[OptionRangeFloat]
GUIName = Convergence X
OptionName = blurx
MinValue = -4.0
MaxValue = 4.0
StepAmount = 0.05
DefaultValue = 0.25

[OptionRangeFloat]
GUIName = Convergence Y
OptionName = blury
MinValue = -4.0
MaxValue = 4.0
StepAmount = 0.05
DefaultValue = -0.1

[OptionRangeFloat]
GUIName = Curvature X
OptionName = warpx
MinValue = 0.0
MaxValue = 0.12
StepAmount = 0.01
DefaultValue = 0.03

[OptionRangeFloat]
GUIName = Curvature Y
OptionName = warpy
MinValue = 0.0
MaxValue = 0.12
StepAmount = 0.01
DefaultValue = 0.04

[OptionRangeFloat]
GUIName = Corner size
OptionName = corner
MinValue = 0.0
MaxValue = 0.10
StepAmount = 0.01
DefaultValue = 0.03

[OptionRangeFloat]
GUIName = Border Smoothness
OptionName = smoothness
MinValue = 100.0
MaxValue = 600.0
StepAmount = 5.0
DefaultValue = 400.0

[OptionRangeFloat]
GUIName = Interlacing Toggle
OptionName = inter
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Interlacing Downscale Scanlines
OptionName = Downscale
MinValue = 1.0
MaxValue = 8.0
StepAmount = 1.
DefaultValue = 2.0

[OptionRangeFloat]
GUIName = Beam low
OptionName = scanlow
MinValue = 1.0
MaxValue = 15.0
StepAmount = 1.0
DefaultValue = 6.0

[OptionRangeFloat]
GUIName = Beam high
OptionName = scanhigh
MinValue = 1.0
MaxValue = 15.0
StepAmount = 1.0
DefaultValue = 8.0

[OptionRangeFloat]
GUIName = Scanlines dark
OptionName = beamlow
MinValue = 0.5
MaxValue = 2.5
StepAmount = 0.0
DefaultValue = 1.45

[OptionRangeFloat]
GUIName = Scanlines bright
OptionName = beamhigh
MinValue = 0.5
MaxValue = 2.5
StepAmount = 0.0
DefaultValue = 1.05

[OptionRangeFloat]
GUIName = Protect White On Masks
OptionName = preserve
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.98

[OptionRangeFloat]
GUIName = Bright boost dark pixels
OptionName = brightboost1
MinValue = 0.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 1.25

[OptionRangeFloat]
GUIName = Bright boost bright pixels
OptionName = brightboost2
MinValue = 0.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Glow pixels per axis
OptionName = glow
MinValue = 1.0
MaxValue = 6.0
StepAmount = 1.0
DefaultValue = 3.0

[OptionRangeFloat]
GUIName = Glow quality
OptionName = quality
MinValue = 0.25
MaxValue = 4.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Glow intensity
OptionName = glow_str
MinValue = 0.0001
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 0.3

[OptionRangeFloat]
GUIName = Add Noise
OptionName = nois
MinValue = 0.0
MaxValue = 32.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Post Brightness
OptionName = postbr
MinValue = 0.0
MaxValue = 2.5
StepAmount = 0.02
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Palette Fixes. Sega, PUAE Atari ST dark colors 
OptionName = palette_fix
MinValue = 0.0
MaxValue = 2.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Mask Type
OptionName = Shadowmask
MinValue = -1.0
MaxValue = 8.0
StepAmount = 1.
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Mask Size
OptionName = masksize
MinValue = 1.0
MaxValue = 2.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Mask dark
OptionName = MaskDark
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 0.2

[OptionRangeFloat]
GUIName = Mask light
OptionName = MaskLight
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 1.5

[OptionRangeFloat]
GUIName = Slot Mask Strength
OptionName = slotmask
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Slot Mask Width
OptionName = slotwidth
MinValue = 1.0
MaxValue = 6.0
StepAmount = 0.5
DefaultValue = 2.0

[OptionRangeFloat]
GUIName = Slot Mask Height: 2x1 or 4x1
OptionName = double_slot
MinValue = 1.0
MaxValue = 2.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Slot Mask Size
OptionName = slotms
MinValue = 1.0
MaxValue = 2.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Gamma Out
OptionName = GAMMA_OUT
MinValue = 0.0
MaxValue = 4.0
StepAmount = 0.05
DefaultValue = 2.25

[OptionRangeFloat]
GUIName = Saturation
OptionName = sat
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Contrast, 1.0:Off
OptionName = contrast
MinValue = 0.00
MaxValue = 2.00
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Color Temperature %
OptionName = WP
MinValue = -100.0
MaxValue = 100.0
StepAmount = 5.
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Red-Green Tint
OptionName = rg
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Red-Blue Tint
OptionName = rb
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Green-Red Tint
OptionName = gr
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Green-Blue Tint
OptionName = gb
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Blue-Red Tint
OptionName = br
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Blue-Green Tint
OptionName = bg
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.005
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Vignette On/Off
OptionName = vignette
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Vignette Power
OptionName = vpower
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.15

[OptionRangeFloat]
GUIName = Vignette strength
OptionName = vstr
MinValue = 0.0
MaxValue = 50.0
StepAmount = 1.0
DefaultValue = 40.0

[OptionRangeFloat]
GUIName = Switch off shader
OptionName = alloff
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 0.0


[/configuration]
*/

#define iTime  (float(GetTime())/2.0)
#define iTimer (float(GetTime())/60.0)

#define SourceSize (vec4(1.0/GetInvNativePixelSize(),GetInvNativePixelSize()))

vec2 Warp(vec2 pos)
{
    pos  = pos * 2.0 - 1.0;    
    pos *= vec2(1.0 + (pos.y * pos.y) * warpx, 1.0 + (pos.x * pos.x) * warpy);
    return pos * 0.5 + 0.5;
} 

float sw(float y, float l)
{
    float beam = mix(scanlow, scanhigh, y);
    float scan = mix(beamlow,  beamhigh, l);
    float ex = y * scan;
    return exp2(-beam * ex * ex);
}

vec3 mask(vec2 x, vec3 col, float l)
{
    x = floor(x / masksize);        
  
    if (Shadowmask == 0.0)
    {
        float m = fract(x.x * 0.4999);
        if (m < 0.4999) return vec3(1.0,             MaskDark, 1.0);
        else            return vec3(MaskDark, 1.0,             MaskDark);
    }
   
    else if (Shadowmask == 1.0)
    {
        vec3 Mask = vec3(MaskDark, MaskDark, MaskDark);
        float line = MaskLight;
        float odd  = 0.0;

        if (fract(x.x / 6.0) < 0.5) odd = 1.0;
        if (fract((x.y + odd) / 2.0) < 0.5) line = MaskDark;

        float m = fract(x.x / 3.0);
        if      (m < 0.333) Mask.b = MaskLight;
        else if (m < 0.666) Mask.g = MaskLight;
        else                Mask.r = MaskLight;
        
        Mask *= line; 
        return Mask; 
    } 
    
    else if (Shadowmask == 2.0)
    {
        float m = fract(x.x*0.3333);
        if (m < 0.3333) return vec3(MaskDark,  MaskDark,  MaskLight);
        if (m < 0.6666) return vec3(MaskDark,  MaskLight, MaskDark);
        else            return vec3(MaskLight, MaskDark,  MaskDark);
    }

    if (Shadowmask == 3.0)
    {
        float m = fract(x.x * 0.5);
        if (m < 0.5) return vec3(1.0, 1.0, 1.0);
        else         return vec3(MaskDark, MaskDark, MaskDark);
    }
   
    else if (Shadowmask == 4.0)
    {   
        vec3 Mask = vec3(col.rgb);
        float line = MaskLight;
        float odd  = 0.0;

        if (fract(x.x / 4.0) < 0.5) odd = 1.0;
        if (fract((x.y + odd) / 2.0) < 0.5) line = MaskDark;

        float m = fract(x.x / 2.0);
        if  (m < 0.5) { Mask.r = 1.0; Mask.b = 1.0; }
        else  Mask.g = 1.0;   

        Mask *= line;  
        return Mask;
    } 

    else if (Shadowmask == 5.0)
    {
        vec3 Mask = vec3(1.0, 1.0, 1.0);

        if (fract(x.x / 4.0) < 0.5)   
        {
            if (fract(x.y / 3.0) < 0.666)
            {
                if (fract(x.x / 2.0) < 0.5) Mask = vec3(1.0,             MaskDark, 1.0);
                else                        Mask = vec3(MaskDark, 1.0,             MaskDark);
            }
            else Mask *= l;
        }
        else if (fract(x.x / 4.0) >= 0.5)   
        {
            if (fract(x.y / 3.0) > 0.333) 
            {
                if (fract(x.x / 2.0) < 0.5) Mask = vec3(1.0,             MaskDark, 1.0); 
                else                        Mask = vec3(MaskDark, 1.0,             MaskDark);
            }
            else Mask *= l;
        }

        return Mask;
    }

    else if (Shadowmask == 6.0)
    {
        vec3 Mask = vec3(MaskDark, MaskDark, MaskDark);
        if (fract(x.x / 6.0) < 0.5)   
        {
            if (fract(x.y / 4.0) < 0.75)  
            {
                if      (fract(x.x / 3.0) < 0.3333) Mask.r = MaskLight; 
                else if (fract(x.x / 3.0) < 0.6666) Mask.g = MaskLight; 
                else                                Mask.b = MaskLight;
            }
            else Mask * l * 0.9;
        }
        else if (fract(x.x / 6.0) >= 0.5)   
        {
            if (fract(x.y / 4.0) >= 0.5 || fract(x.y / 4.0) < 0.25)  
            {
                if      (fract(x.x / 3.0) < 0.3333) Mask.r = MaskLight; 
                else if (fract(x.x / 3.0) < 0.6666) Mask.g = MaskLight;
                else                                Mask.b = MaskLight;
            }
            else Mask * l * 0.9;
        }
        return Mask;
    }

    else if (Shadowmask == 7.0)
    {
        float m = fract(x.x * 0.3333);

        if (m < 0.3333) return vec3(MaskDark,          MaskLight,         MaskLight * col.b); //Cyan
        if (m < 0.6666) return vec3(MaskLight * col.r, MaskDark,          MaskLight);         //Magenta
        else            return vec3(MaskLight,         MaskLight * col.g, MaskDark);          //Yellow
    }

    else if (Shadowmask == 8.0)
    {
        vec3 Mask = vec3(MaskDark, MaskDark, MaskDark);

        float bright = MaskLight;
        float left   = 0.0;
        if (fract(x.x / 6.0) < 0.5) left = 1.0;
             
        float m = fract(x.x / 3.0);
        if      (m < 0.333) Mask.b = 0.9;
        else if (m < 0.666) Mask.g = 0.9;
        else                Mask.r = 0.9;
        
        if (mod(x.y, 2.0) == 1.0 && left == 1.0 || mod(x.y, 2.0) == 0.0 && left == 0.0) 
            Mask *= bright; 
      
        return Mask; 
    } 
    
    else return vec3(1.0, 1.0, 1.0);
}

float SlotMask(vec2 pos, vec3 c)
{
    if (slotmask == 0.0) return 1.0;
    
    pos = floor(pos / slotms);
    float mx = pow(max(max(c.r, c.g), c.b), 1.33);
    float mlen = slotwidth * 2.0;
    float px = fract(pos.x / mlen);
    float py = floor(fract(pos.y / (2.0 * double_slot)) * 2.0 * double_slot);
    float slot_dark = mix(1.0 - slotmask, 1.0 - 0.80 * slotmask, mx);
    float slot = 1.0 + 0.7 * slotmask * (1.0 - mx);
    
    if      (py == 0.0                && px <  0.5) slot = slot_dark; 
    else if (py == double_slot && px >= 0.5) slot = slot_dark;       
    
    return slot;
}

mat4 contrastMatrix(float contrast)
{   
    float t = (1.0 - contrast) / 2.0;
    
    return mat4(contrast, 0,               0,               0,
                0,               contrast, 0,               0,
                0,               0,               contrast, 0,
                t,               t,               t,               1);
}

mat3 vign(float l)
{
//    vec2 vpos = vTexCoord;
    vec2 vpos = GetCoordinates();
    vpos *= 1.0 - vpos.xy;
    
    float vig = vpos.x * vpos.y * vstr;
    vig = min(pow(vig, vpower), 1.0); 
    if (vignette == 0.0) vig = 1.0;
   
    return mat3(vig, 0,   0,
                0,   vig, 0,
                0,   0,   vig);
}

vec3 saturation(vec3 textureColor)
{
    float luminance = length(textureColor.rgb) * 0.5775;

    vec3 luminanceWeighting = vec3(0.4, 0.5, 0.1);
    if (luminance < 0.5) luminanceWeighting.rgb = (luminanceWeighting.rgb * luminanceWeighting.rgb) 
                                                + (luminanceWeighting.rgb * luminanceWeighting.rgb);

    luminance = dot(textureColor.rgb, luminanceWeighting);
    vec3 greyScaleColor = vec3(luminance, luminance, luminance);

    vec3 res = vec3(mix(greyScaleColor, textureColor.rgb, sat));
    return res;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

vec3 glow0 (vec2 texcoord, vec3 col)
{

   // the more quality, the smaller the offset and better quality, less visible glow too
     vec2 size = SourceSize.zw/quality;
     
     vec3 c01;
     vec3 sum = vec3(0.0);
   
   // glow = pixels per axis, the more the slower!

    for (float x = -glow; x <= glow; x = x+1.0)
     {

   // multiply texture, the more far away the less pronounced
        float factor = 1.0/glow;
        for (float y = -glow; y <= glow; y = y+1.0)
        {

        vec2 offset = vec2(x, y) * size;

         c01 = SampleLocation(texcoord + offset).rgb*factor; c01 = c01*c01;
          
                sum += c01;
        }
    }
  
    return (glow_str * sum / (glow * glow )) ;
}
    
///////////////////////////////////////////////////////////////////////////////////////////////////////////

float noise(vec2 co)
{
    return fract(sin(iTimer * dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}

float corner0(vec2 coord)
{
    coord = (coord - vec2(0.5, 0.5)) * 1.0 + vec2(0.5, 0.5);
    coord = min(coord, vec2(1.0, 1.0) - coord) * vec2(1.0, SourceSize.y / SourceSize.x);
    
    vec2 cdist = vec2(corner, corner);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));

    return clamp((cdist.x - dist) * smoothness, 0.0, 1.0);
}  

const mat3 D65_to_XYZ = mat3(
           0.4306190,  0.2220379,  0.0201853,
           0.3415419,  0.7066384,  0.1295504,
           0.1783091,  0.0713236,  0.9390944);

const mat3 XYZ_to_D65 = mat3(
           3.0628971, -0.9692660,  0.0678775,
          -1.3931791,  1.8760108, -0.2288548,
          -0.4757517,  0.0415560,  1.0693490);
           
const mat3 D50_to_XYZ = mat3(
           0.4552773,  0.2323025,  0.0145457,
           0.3675500,  0.7077956,  0.1049154,
           0.1413926,  0.0599019,  0.7057489);
           
const mat3 XYZ_to_D50 = mat3(
           2.9603944, -0.9787684,  0.0844874,
          -1.4678519,  1.9161415, -0.2545973,
          -0.4685105,  0.0334540,  1.4216174);         

void main()
{
    vec2 vTexCoord  = GetCoordinates();
    vec2 pos = Warp(vTexCoord.xy);
    vec2 tex_size = 1.0 / GetInvNativePixelSize();  
    vec2 OutputSize = GetWindowSize();


    vec2 pC4 = (pos + 0.5/tex_size);
    vec2 fp = fract(pos * tex_size);
    if (inter < 0.5 && tex_size.y > 400.0){ fp.y = fract(pos.y * tex_size.y*1.0/Downscale);} 

    vec4 res = vec4(1.0);
    
    if (alloff == 1.0) 
        res = SampleLocation(pC4); 
    else
    {

   vec2 texel = pos * tex_size;
   vec2 texel_floored = floor(texel);

   float scale = PRE_SCALE;
   float region_range = 0.5 - 0.5 / scale;

   // Figure out where in the texel to sample to get correct pre-scaled bilinear.
   // Uses the hardware bilinear interpolator to avoid having to sample 4 times manually.

   vec2 center_dist = fp - 0.5;

   vec2 fpp = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;

   vec2 mod_texel = texel_floored + fpp;
   vec2 coords = mod_texel / SourceSize.xy;

        vec3 sample1 = SampleLocation(vec2(coords.x + blurx*SourceSize.z, coords.y - blury*SourceSize.w)).rgb;
        vec3 sample2 = SampleLocation(coords).rgb;
        vec3 sample3 = SampleLocation(vec2(coords.x - blurx*SourceSize.z, coords.y + blury*SourceSize.w )).rgb;
        
        vec3 color = vec3(sample1.r * 0.5  + sample2.r * 0.5, 
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
            vec3 warmer = D50_to_XYZ * color;
            warmer = XYZ_to_D65 * warmer; 
            
            vec3 cooler = D65_to_XYZ * color;
            cooler = XYZ_to_D50 * cooler;
            
            float m = abs(WP) / 100.0;
            vec3 comp = (WP < 0.0) ? cooler : warmer;
            comp = clamp(comp, 0.0, 1.0);   
            
            color = vec3(mix(color, comp, m));
        }

     mat3 hue = mat3 (1., rg,  rb,                 //red tint
                      gr,  1., gb,                  //green tint
                      br,  bg,  1.);                //blue tint

        color = hue * color;

        color = (2.0*pow(color,vec3(2.8))) - pow(color,vec3(3.6));

        float lum = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;

        float f = fract(fp.y -0.5);
        
        if (inter > 0.5 && tex_size.y > 400.0) color = color; 
        else
        {color = color * sw(f,lum) + color * sw (1.0-f,lum);}
        
        float lum1 = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;

        
        color *= mix(mask((vTexCoord * OutputSize.xy), color,lum1), vec3(1.0), lum1*preserve);
        

        if (slotmask != 0.0) color *= SlotMask((vTexCoord * OutputSize.xy) * 1.0001, color);
        
        color *= mix(brightboost1, brightboost2, max(max(color.r, color.g), color.b));    

    

        color = pow(color,vec3(1.0 / GAMMA_OUT));
                if (glow_str != 0.0) color += glow0(coords,color);

        if (sat    != 1.0) color  = saturation(color);
        if (corner != 0.0) color *= corner0(pC4);
        if (nois   != 0.0) color *= 1.0 + noise(coords * 2.0) / nois;

        color *= mix(1.0, postbr, lum);
        res = vec4(color, 1.0);
        if (contrast != 1.0) res = contrastMatrix(contrast) * res;
        if (inter > 0.5 && SourceSize.y > 400.0 && fract(iTime) < 0.5) res = res * 0.95;
        res.rgb *= vign(lum);

    }
    
    SetOutput(res);
}
