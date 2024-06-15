//    Hyllian's CRT Shader

//    Copyright (C) 2011-2024 Hyllian - sergiogdb@gmail.com

//    Permission is hereby granted, free of charge, to any person obtaining a copy
//    of this software and associated documentation files (the "Software"), to deal
//    in the Software without restriction, including without limitation the rights
//    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//    copies of the Software, and to permit persons to whom the Software is
//    furnished to do so, subject to the following conditions:

//    The above copyright notice and this permission notice shall be included in
//    all copies or substantial portions of the Software.

//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//    THE SOFTWARE.

/*
[configuration]

[OptionRangeFloat]
GUIName = HIGH RESOLUTION SCANLINES
OptionName = SCANLINES_HIRES
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName =VERTICAL SCANLINES
OptionName = VSCANLINES
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = BEAM PROFILE
OptionName = BEAM_PROFILE
MinValue = 0.0
MaxValue = 2.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = HORIZONTAL FILTER PROFILE
OptionName = HFILTER_PROFILE
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = COLOR BOOST
OptionName = COLOR_BOOST
MinValue = 1.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 1.40

[OptionRangeFloat]
GUIName = SHARPNESS HACK
OptionName = SHARPNESS_HACK
MinValue = 1.0
MaxValue = 4.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = PHOSPHOR LAYOUT
OptionName = PHOSPHOR_LAYOUT
MinValue = 0.0
MaxValue = 15.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = MASK INTENSITY
OptionName = MASK_INTENSITY
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.65

[OptionRangeFloat]
GUIName = MIN BEAM WIDTH
OptionName = BEAM_MIN_WIDTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.86

[OptionRangeFloat]
GUIName = MAX BEAM WIDTH
OptionName = BEAM_MAX_WIDTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = SCANLINES STRENGTH
OptionName = SCANLINES_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.58

[OptionRangeFloat]
GUIName = SCANLINES CUTOFF
OptionName = SCANLINES_CUTOFF
MinValue = 0.0
MaxValue = 1000.0
StepAmount = 1.0
DefaultValue = 390.0

[OptionRangeFloat]
GUIName = MONITOR SUBPIXELS LAYOUT
OptionName = MONITOR_SUBPIXELS
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = ANTI RINGING
OptionName = CRT_ANTI_RINGING
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = INPUT GAMMA
OptionName = CRT_InputGamma
MinValue = 1.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 2.4

[OptionRangeFloat]
GUIName = OUTPUT GAMMA
OptionName = CRT_OutputGamma
MinValue = 1.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 2.2

[/configuration]
*/


#define GAMMA_IN(color)     pow(color, vec3(GetOption(CRT_InputGamma), GetOption(CRT_InputGamma), GetOption(CRT_InputGamma)))
#define GAMMA_OUT(color)    pow(color, vec3(1.0 / GetOption(CRT_OutputGamma), 1.0 / GetOption(CRT_OutputGamma), 1.0 / GetOption(CRT_OutputGamma)))

const vec3 Y = vec3(0.2627, 0.6780, 0.0593);


//    A collection of CRT mask effects that work with LCD subpixel structures for
//    small details

//    author: hunterk
//    license: public domain

// Mask code pasted from subpixel_masks.h. Masks 3 and 4 added.
vec3 mask_weights(vec2 coord, float mask_intensity, int phosphor_layout, float monitor_subpixels){
   vec3 weights = vec3(1.,1.,1.);
   float on = 1.;
   float off = 1.-mask_intensity;
   vec3 red     = monitor_subpixels==1.0 ? vec3(on,  off, off) : vec3(off, off, on );
   vec3 green   = vec3(off, on,  off);
   vec3 blue    = monitor_subpixels==1.0 ? vec3(off, off, on ) : vec3(on,  off, off);
   vec3 magenta = vec3(on,  off, on );
   vec3 yellow  = monitor_subpixels==1.0 ? vec3(on,  on,  off) : vec3(off, on,  on );
   vec3 cyan    = monitor_subpixels==1.0 ? vec3(off, on,  on ) : vec3(on,  on,  off);
   vec3 black   = vec3(off, off, off);
   vec3 white   = vec3(on,  on,  on );
   int w, z = 0;
   
   // This pattern is used by a few layouts, so we'll define it here
   vec3 aperture_weights = mix(magenta, green, floor(mod(coord.x, 2.0)));
   
   if(phosphor_layout == 0) return weights;

   else if(phosphor_layout == 1){
      // classic aperture for RGB panels; good for 1080p, too small for 4K+
      // aka aperture_1_2_bgr
      weights  = aperture_weights;
      return weights;
   }

   else if(phosphor_layout == 2){
      // Classic RGB layout; good for 1080p and lower
      vec3 bw3[3] = vec3[](red, green, blue);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = bw3[z];
      return weights;
   }

   else if(phosphor_layout == 3){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 1080p and lower
      vec3 bw3[3] = vec3[](black, white, black);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = bw3[z];
      return weights;
   }

   else if(phosphor_layout == 4){
      // reduced TVL aperture for RGB panels. Good for 4k.
      // aperture_2_4_rgb
      
      vec3 big_ap_rgb[4] = vec3[](red, yellow, cyan, blue);
      
      w = int(floor(mod(coord.x, 4.0)));
      
      weights = big_ap_rgb[w];
      return weights;
   }
   
   else if(phosphor_layout == 5){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 4k 
      vec3 bw4[4] = vec3[](black, black, white, white);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = bw4[z];
      return weights;
   }

   else if(phosphor_layout == 6){
      // aperture_1_4_rgb; good for simulating lower 
      vec3 ap4[4] = vec3[](red, green, blue, black);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = ap4[z];
      return weights;
   }

   else if(phosphor_layout == 7){
      // 2x2 shadow mask for RGB panels; good for 1080p, too small for 4K+
      // aka delta_1_2x1_bgr
      vec3 inverse_aperture = mix(green, magenta, floor(mod(coord.x, 2.0)));
      weights               = mix(aperture_weights, inverse_aperture, floor(mod(coord.y, 2.0)));
      return weights;
   }

   else if(phosphor_layout == 8){
      // delta_2_4x1_rgb
      vec3 delta[2][4] = {
         {red, yellow, cyan, blue},
         {cyan, blue, red, yellow}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta[w][z];
      return weights;
   }

   else if(phosphor_layout == 9){
      // delta_1_4x1_rgb; dunno why this is called 4x1 when it's obviously 4x2 /shrug
      vec3 delta1[2][4] = {
         {red,  green, blue, black},
         {blue, black, red,  green}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta1[w][z];
      return weights;
   }
   
   else if(phosphor_layout == 10){
      // delta_2_4x2_rgb
      vec3 delta[4][4] = {
         {red,  yellow, cyan, blue},
         {red,  yellow, cyan, blue},
         {cyan, blue,   red,  yellow},
         {cyan, blue,   red,  yellow}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta[w][z];
      return weights;
   }

   else if(phosphor_layout == 11){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      vec3 slotmask[4][6] = {
         {red, green, blue,    red, green, blue,},
         {red, green, blue,  black, black, black},
         {red, green, blue,    red, green, blue,},
         {black, black, black, red, green, blue,}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 6.0)));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[w][z];
      return weights;
   }

   else if(phosphor_layout == 12){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      vec3 slotmask[4][6] = {
         {black,  white, black,   black,  white, black,},
         {black,  white, black,  black, black, black},
         {black,  white, black,  black,  white, black,},
         {black, black, black,  black,  white, black,}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 6.0)));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[w][z];
      return weights;
   }

   else if(phosphor_layout == 13){
      // based on MajorPainInTheCactus' HDR slot mask
      vec3 slot[4][8] = {
         {red,   green, blue,  black, red,   green, blue,  black},
         {red,   green, blue,  black, black, black, black, black},
         {red,   green, blue,  black, red,   green, blue,  black},
         {black, black, black, black, red,   green, blue,  black}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 8.0)));
      
      weights = slot[w][z];
      return weights;
   }

   else if(phosphor_layout == 14){
      // same as above but for RGB panels
      vec3 slot2[4][10] = {
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {black, green,  green, blue,  blue,  red,   red,    black, black, black},
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {red,   red,    black, black, black, black, green,  green, blue,  blue }
      };
   
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 10.0)));
      
      weights = slot2[w][z];
      return weights;
   }
   
   else if(phosphor_layout == 15){
      // slot_3_7x6_rgb
      vec3 slot[6][14] = {
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  black, black, black,  black,  black, black, black},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {red,   red,   yellow, green, cyan,  blue,  blue,  red,   red,   yellow, green,  cyan,  blue,  blue},
         {black, black, black,  black, black, black, black, black, red,   red,    yellow, green, cyan,  blue}
      };
      
      w = int(floor(mod(coord.y, 6.0)));
      z = int(floor(mod(coord.x, 14.0)));
      
      weights = slot[w][z];
      return weights;
   }


   
   else return weights;
}

// Horizontal cubic filter.
// Some known filters use these values:

//    B = 0.5, C = 0.0        =>  A sharp almost gaussian filter.
//    B = 0.0, C = 0.0        =>  Hermite cubic filter.
//    B = 1.0, C = 0.0        =>  Cubic B-Spline filter.
//    B = 0.0, C = 0.5        =>  Catmull-Rom Spline filter.
//    B = C = 1.0/3.0         =>  Mitchell-Netravali cubic filter.
//    B = 0.3782, C = 0.3109  =>  Robidoux filter.
//    B = 0.2620, C = 0.3690  =>  Robidoux Sharp filter.

// For more info, see: http://www.imagemagick.org/Usage/img_diagrams/cubic_survey.gif

mat4x4 get_hfilter_profile()
{
	float bf = 1.0;
	float cf = 0.0;

	if (GetOption(HFILTER_PROFILE) == 1) {bf = 1.0/3.0;     cf = 1.0/3.0;}

        return mat4x4(            (-bf - 6.0*cf)/6.0,         (3.0*bf + 12.0*cf)/6.0,     (-3.0*bf - 6.0*cf)/6.0,             bf/6.0,
                                        (12.0 - 9.0*bf - 6.0*cf)/6.0, (-18.0 + 12.0*bf + 6.0*cf)/6.0,                      0.0, (6.0 - 2.0*bf)/6.0,
                                       -(12.0 - 9.0*bf - 6.0*cf)/6.0, (18.0 - 15.0*bf - 12.0*cf)/6.0,      (3.0*bf + 6.0*cf)/6.0,             bf/6.0,
                                                   (bf + 6.0*cf)/6.0,                           -cf,                      0.0,               0.0);
         
         
}


#define scanlines_strength (4.0*profile.x)
#define beam_min_width     profile.y
#define beam_max_width     profile.z
#define color_boost        profile.w


vec4 get_beam_profile()
{
	vec4 bp = vec4(GetOption(SCANLINES_STRENGTH), GetOption(BEAM_MIN_WIDTH), GetOption(BEAM_MAX_WIDTH), GetOption(COLOR_BOOST));

	if (BEAM_PROFILE == 1)  bp = vec4(0.58, 0.86, 1.00, 1.60); // Catmull-rom
	if (BEAM_PROFILE == 2)  bp = vec4(0.58, 0.72, 1.00, 1.75); // Catmull-rom

	return bp;
}


void main()
{
    vec2 vTexCoord = GetCoordinates();
    vec2 SourceSize = 1.0 / GetInvNativePixelSize(); // This work with previous build.

    vec4 profile = get_beam_profile();

    vec2 TextureSize = mix(vec2(SourceSize.x * GetOption(SHARPNESS_HACK), SourceSize.y), vec2(SourceSize.x, SourceSize.y * GetOption(SHARPNESS_HACK)), GetOption(VSCANLINES));

    vec2 dx = mix(vec2(1.0/TextureSize.x, 0.0), vec2(0.0, 1.0/TextureSize.y), GetOption(VSCANLINES));
    vec2 dy = mix(vec2(0.0, 1.0/TextureSize.y), vec2(1.0/TextureSize.x, 0.0), GetOption(VSCANLINES));

    vec2 pix_coord = vTexCoord.xy*TextureSize.xy - vec2(0.5, 0.5);

    vec2 tc = ( (SCANLINES_HIRES > 0.5) ? (mix(vec2(floor(pix_coord.x), pix_coord.y), vec2(pix_coord.x, floor(pix_coord.y)), GetOption(VSCANLINES)) + vec2(0.5, 0.5)) : (floor(pix_coord) + vec2(0.5, 0.5)) )/TextureSize;

    pix_coord = mix(pix_coord, pix_coord.yx, GetOption(VSCANLINES));

    vec2 fp = fract(pix_coord);

    vec3 c00 = GAMMA_IN(SampleLocation(tc     - dx     ).xyz);
    vec3 c01 = GAMMA_IN(SampleLocation(tc              ).xyz);
    vec3 c02 = GAMMA_IN(SampleLocation(tc     + dx     ).xyz);
    vec3 c03 = GAMMA_IN(SampleLocation(tc + 2.0*dx     ).xyz);

    vec3 c10 = (SCANLINES_HIRES > 0.5) ? c00 : GAMMA_IN(SampleLocation(tc     - dx +dy ).xyz);
    vec3 c11 = (SCANLINES_HIRES > 0.5) ? c01 : GAMMA_IN(SampleLocation(tc          +dy ).xyz);
    vec3 c12 = (SCANLINES_HIRES > 0.5) ? c02 : GAMMA_IN(SampleLocation(tc     + dx +dy ).xyz);
    vec3 c13 = (SCANLINES_HIRES > 0.5) ? c03 : GAMMA_IN(SampleLocation(tc + 2.0*dx +dy ).xyz);

    mat4x4 invX = get_hfilter_profile();

    mat4x3 color_matrix0 = mat4x3(c00, c01, c02, c03);
    mat4x3 color_matrix1 = mat4x3(c10, c11, c12, c13);
    
    vec4 invX_Px    = vec4(fp.x*fp.x*fp.x, fp.x*fp.x, fp.x, 1.0) * invX;
    vec3 color0     = color_matrix0 * invX_Px;
    vec3 color1     = color_matrix1 * invX_Px;

    // Get min/max samples
    vec3 min_sample0 = min(c01,c02);
    vec3 max_sample0 = max(c01,c02);
    vec3 min_sample1 = min(c11,c12);
    vec3 max_sample1 = max(c11,c12);
    
    // Anti-ringing
    vec3 aux = color0;
    color0 = clamp(color0, min_sample0, max_sample0);
    color0 = mix(aux, color0, GetOption(CRT_ANTI_RINGING) * step(0.0, (c00-c01)*(c02-c03)));
    aux = color1;
    color1 = clamp(color1, min_sample1, max_sample1);
    color1 = mix(aux, color1, GetOption(CRT_ANTI_RINGING) * step(0.0, (c10-c11)*(c12-c13)));

    float pos0 = fp.y;
    float pos1 = 1 - fp.y;

    vec3 lum0 = mix(vec3(beam_min_width), vec3(beam_max_width), color0);
    vec3 lum1 = mix(vec3(beam_min_width), vec3(beam_max_width), color1);

    vec3 d0 = scanlines_strength*pos0/(lum0*lum0+0.0000001);
    vec3 d1 = scanlines_strength*pos1/(lum1*lum1+0.0000001);

    d0 = exp(-d0*d0);
    d1 = exp(-d1*d1);

    vec3 color = (TextureSize.y <= SCANLINES_CUTOFF) ? (color0*d0+color1*d1) : GAMMA_IN(SampleLocation(vTexCoord).xyz);            

    color  = color_boost*GAMMA_OUT(color);

    vec2 mask_coords =vTexCoord.xy * GetWindowSize().xy;

    mask_coords = mix(mask_coords.xy, mask_coords.yx, GetOption(VSCANLINES));

    color.rgb*=GAMMA_OUT(mask_weights(mask_coords,  GetOption(MASK_INTENSITY), int(GetOption(PHOSPHOR_LAYOUT)), GetOption(MONITOR_SUBPIXELS)));

    SetOutput(vec4(color, 1.0));
}
