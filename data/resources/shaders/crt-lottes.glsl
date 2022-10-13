// PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER
//
//   by Timothy Lottes
//
// This is more along the style of a really good CGA arcade monitor.
// With RGB inputs instead of NTSC.
// The shadow mask example has the mask rotated 90 degrees for less chromatic aberration.
//
// Left it unoptimized to show the theory behind the algorithm.
//
// It is an example what I personally would want as a display option for pixel art games.
// Please take and use, change, or whatever.

/*
[configuration]

[OptionRangeFloat]
GUIName = Scanline Weight
OptionName = hardScan
MinValue = -20.0
MaxValue = 0.0
StepAmount = 1.0
DefaultValue = -8.0

[OptionRangeFloat]
GUIName = Scanline Scale
OptionName = hardPix
MinValue = -20.0
MaxValue = 0.0
StepAmount = 1.0
DefaultValue = -3.0

[OptionRangeFloat]
GUIName = Screen Warp X
OptionName = warpX
MinValue = 0.0
MaxValue = 0.125
StepAmount = 0.01
DefaultValue = 0.031

[OptionRangeFloat]
GUIName = Screen Warp Y
OptionName = warpY
MinValue = 0.0
MaxValue = 0.125
StepAmount = 0.01
DefaultValue = 0.041

[OptionRangeFloat]
GUIName = Mask Dark
OptionName = maskDark
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Mask Light
OptionName = maskLight
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 1.5

[OptionRangeInteger]
GUIName = Shadow Mask Type
OptionName = shadowMask
MinValue = 0
MaxValue = 4
StepAmount = 1
DefaultValue = 3

[OptionRangeFloat]
GUIName = Brightess Boost
OptionName = brightBoost
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Bloom Soft X
OptionName = hardBloomPix
MinValue = -2.0
MaxValue = -0.5
StepAmount = 0.1
DefaultValue = -1.5

[OptionRangeFloat]
GUIName = Bloom Soft Y
OptionName = hardBloomScan
MinValue = -4.0
MaxValue = -1.0
StepAmount = 0.1
DefaultValue = -2.0

[OptionRangeFloat]
GUIName = Bloom Amount
OptionName = bloomAmount
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.15

[OptionRangeFloat]
GUIName = Filter Kernel Shape
OptionName = shape
MinValue = 0.0
MaxValue = 10.0
StepAmount = 0.05
DefaultValue = 2.0

[OptionBool]
GUIName = Scale in Linear Gamma
OptionName = scaleInLinearGamma
DefaultValue = true

[/configuration]
*/

//Uncomment to reduce instructions with simpler linearization
//(fixes HD3000 Sandy Bridge IGP)
//#define SIMPLE_LINEAR_GAMMA
#define DO_BLOOM

// ------------- //

// sRGB to Linear.
// Assuming using sRGB typed textures this should not be needed.
#ifdef SIMPLE_LINEAR_GAMMA
float ToLinear1(float c)
{
    return c;
}
float3 ToLinear(float3 c)
{
    return c;
}
float3 ToSrgb(float3 c)
{
    return pow(c, float3(1.0 / 2.2));
}
#else
float ToLinear1(float c)
{
    if (!OptionEnabled(scaleInLinearGamma)) 
        return c;
    
    return(c<=0.04045) ? c/12.92 : pow((c + 0.055)/1.055, 2.4);
}

float3 ToLinear(float3 c)
{
    if (!OptionEnabled(scaleInLinearGamma)) 
        return c;
    
    return float3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

// Linear to sRGB.
// Assuming using sRGB typed textures this should not be needed.
float ToSrgb1(float c)
{
    if (!OptionEnabled(scaleInLinearGamma)) 
        return c;
    
    return(c<0.0031308 ? c*12.92 : 1.055*pow(c, 0.41666) - 0.055);
}

float3 ToSrgb(float3 c)
{
    if (!OptionEnabled(scaleInLinearGamma)) 
        return c;
    
    return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}
#endif

// Nearest emulated sample given floating point position and texel offset.
// Also zero's off screen.
float3 Fetch(float2 pos,float2 off){
  pos=(floor(pos*GetResolution()+off)+float2(0.5,0.5))*GetInvResolution();
#ifdef SIMPLE_LINEAR_GAMMA
  return ToLinear(GetOption(brightBoost) * pow(SampleLocation(pos.xy).rgb, float3(2.2)));
#else
  return ToLinear(GetOption(brightBoost) * SampleLocation(pos.xy).rgb);
#endif
}

// Distance in emulated pixels to nearest texel.
float2 Dist(float2 pos)
{
    pos = pos*GetOriginalSize().xy;
    
    return -((pos - floor(pos)) - float2(0.5, 0.5));
}
    
// 1D Gaussian.
float Gaus(float pos, float scale)
{
    return exp2(scale*pow(abs(pos), GetOption(shape)));
}

// 3-tap Gaussian filter along horz line.
float3 Horz3(float2 pos, float off)
{
    float3 b    = Fetch(pos, float2(-1.0, off));
    float3 c    = Fetch(pos, float2( 0.0, off));
    float3 d    = Fetch(pos, float2( 1.0, off));
    float dst = Dist(pos).x;

    // Convert distance to weight.
    float scale = GetOption(hardPix);
    float wb = Gaus(dst-1.0,scale);
    float wc = Gaus(dst+0.0,scale);
    float wd = Gaus(dst+1.0,scale);

    // Return filtered sample.
    return (b*wb+c*wc+d*wd)/(wb+wc+wd);
}

// 5-tap Gaussian filter along horz line.
float3 Horz5(float2 pos,float off){
    float3 a = Fetch(pos,float2(-2.0, off));
    float3 b = Fetch(pos,float2(-1.0, off));
    float3 c = Fetch(pos,float2( 0.0, off));
    float3 d = Fetch(pos,float2( 1.0, off));
    float3 e = Fetch(pos,float2( 2.0, off));
    
    float dst = Dist(pos).x;
    // Convert distance to weight.
    float scale = GetOption(hardPix);
    float wa = Gaus(dst - 2.0, scale);
    float wb = Gaus(dst - 1.0, scale);
    float wc = Gaus(dst + 0.0, scale);
    float wd = Gaus(dst + 1.0, scale);
    float we = Gaus(dst + 2.0, scale);
    
    // Return filtered sample.
    return (a*wa+b*wb+c*wc+d*wd+e*we)/(wa+wb+wc+wd+we);
}
  
// 7-tap Gaussian filter along horz line.
float3 Horz7(float2 pos,float off)
{
    float3 a = Fetch(pos, float2(-3.0, off));
    float3 b = Fetch(pos, float2(-2.0, off));
    float3 c = Fetch(pos, float2(-1.0, off));
    float3 d = Fetch(pos, float2( 0.0, off));
    float3 e = Fetch(pos, float2( 1.0, off));
    float3 f = Fetch(pos, float2( 2.0, off));
    float3 g = Fetch(pos, float2( 3.0, off));

    float dst = Dist(pos).x;
    // Convert distance to weight.
    float scale = GetOption(hardBloomPix);
    float wa = Gaus(dst - 3.0, scale);
    float wb = Gaus(dst - 2.0, scale);
    float wc = Gaus(dst - 1.0, scale);
    float wd = Gaus(dst + 0.0, scale);
    float we = Gaus(dst + 1.0, scale);
    float wf = Gaus(dst + 2.0, scale);
    float wg = Gaus(dst + 3.0, scale);

    // Return filtered sample.
    return (a*wa+b*wb+c*wc+d*wd+e*we+f*wf+g*wg)/(wa+wb+wc+wd+we+wf+wg);
}
  
// Return scanline weight.
float Scan(float2 pos, float off)
{
    float dst = Dist(pos).y;

    return Gaus(dst + off, GetOption(hardScan));
}
  
// Return scanline weight for bloom.
float BloomScan(float2 pos, float off)
{
    float dst = Dist(pos).y;
    
    return Gaus(dst + off, GetOption(hardBloomScan));
}

// Allow nearest three lines to effect pixel.
float3 Tri(float2 pos)
{
    float3 a = Horz3(pos,-1.0);
    float3 b = Horz5(pos, 0.0);
    float3 c = Horz3(pos, 1.0);
    
    float wa = Scan(pos,-1.0); 
    float wb = Scan(pos, 0.0);
    float wc = Scan(pos, 1.0);
    
    return a*wa + b*wb + c*wc;
}
  
// Small bloom.
float3 Bloom(float2 pos)
{
    float3 a = Horz5(pos,-2.0);
    float3 b = Horz7(pos,-1.0);
    float3 c = Horz7(pos, 0.0);
    float3 d = Horz7(pos, 1.0);
    float3 e = Horz5(pos, 2.0);

    float wa = BloomScan(pos,-2.0);
    float wb = BloomScan(pos,-1.0); 
    float wc = BloomScan(pos, 0.0);
    float wd = BloomScan(pos, 1.0);
    float we = BloomScan(pos, 2.0);

    return a*wa+b*wb+c*wc+d*wd+e*we;
}
  
// Distortion of scanlines, and end of screen alpha.
float2 Warp(float2 pos)
{
    pos  = pos*2.0-1.0;    
    pos *= float2(1.0 + (pos.y*pos.y)*GetOption(warpX), 1.0 + (pos.x*pos.x)*GetOption(warpY));
    
    return pos*0.5 + 0.5;
}
  
// Shadow mask.
float3 Mask(float2 pos)
{
    float3 mask = float3(GetOption(maskDark), GetOption(maskDark), GetOption(maskDark));
  
    // Very compressed TV style shadow mask.
    if (GetOption(shadowMask) == 1) 
    {
        float line_ = GetOption(maskLight);
        float odd = 0.0;
        
        if (fract(pos.x*0.166666666) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) * 0.5) < 0.5) line_ = GetOption(maskDark);  
        
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.r = GetOption(maskLight);
        else if (pos.x < 0.666) mask.g = GetOption(maskLight);
        else                    mask.b = GetOption(maskLight);
        mask*=line_;  
    } 

    // Aperture-grille.
    else if (GetOption(shadowMask) == 2) 
    {
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.r = GetOption(maskLight);
        else if (pos.x < 0.666) mask.g = GetOption(maskLight);
        else                    mask.b = GetOption(maskLight);
    } 

    // Stretched VGA style shadow mask (same as prior shaders).
    else if (GetOption(shadowMask) == 3) 
    {
        pos.x += pos.y*3.0;
        pos.x  = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.r = GetOption(maskLight);
        else if (pos.x < 0.666) mask.g = GetOption(maskLight);
        else                    mask.b = GetOption(maskLight);
    }

    // VGA style shadow mask.
    else if (GetOption(shadowMask) == 4) 
    {
        pos.xy  = floor(pos.xy*float2(1.0, 0.5));
        pos.x  += pos.y*3.0;
        pos.x   = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.r = GetOption(maskLight);
        else if (pos.x < 0.666) mask.g = GetOption(maskLight);
        else                    mask.b = GetOption(maskLight);
    }

    return mask;
}

void main()
{
    float2 pos = Warp(GetCoordinates());
    float3 outColor = Tri(pos);

#ifdef DO_BLOOM
    //Add Bloom
    outColor.rgb += Bloom(pos)*GetOption(bloomAmount);
#endif

    if (GetOption(shadowMask) > 0.0)
        outColor.rgb *= Mask(gl_FragCoord.xy * 1.000001);
    
#ifdef GL_ES    /* TODO/FIXME - hacky clamp fix */
    float2 bordertest = (pos);
    if ( bordertest.x > 0.0001 && bordertest.x < 0.9999 && bordertest.y > 0.0001 && bordertest.y < 0.9999)
        outColor.rgb = outColor.rgb;
    else
        outColor.rgb = float3(0.0);
#endif
    SetOutput(float4(ToSrgb(outColor.rgb), 1.0));
} 
