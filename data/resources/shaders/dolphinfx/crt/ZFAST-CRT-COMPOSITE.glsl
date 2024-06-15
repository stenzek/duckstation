//    zfast_crt - A very simple CRT shader.

//    Copyright (C) 2017 Greg Hogan (SoltanGris42)
//	edited by metallic 77.
//	ported to slang by gregoricavichioli & hunterk.
//      ported to dolphinfx by Hyllian.

//    This program is free software; you can redistribute it and/or modify it
//    under the terms of the GNU General Public License as published by the Free
//    Software Foundation; either version 2 of the License, or (at your option)
//    any later version.


/*
[configuration]

[OptionRangeFloat]
GUIName = Curvature
OptionName = Curvature
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Convergence X-Axis
OptionName = blurx
MinValue = -1.0
MaxValue =  2.0
StepAmount = 0.05
DefaultValue = 0.85

[OptionRangeFloat]
GUIName = Convergence Y-Axis
OptionName = blury
MinValue = -1.0
MaxValue =  1.0
StepAmount = 0.05
DefaultValue = -0.10

[OptionRangeFloat]
GUIName = Scanline Amount (Low)
OptionName = HIGHSCANAMOUNT1
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.4

[OptionRangeFloat]
GUIName = Scanline Amount (High)
OptionName = HIGHSCANAMOUNT2
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.3

[OptionRangeFloat]
GUIName = Mask Type
OptionName = TYPE
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Mask Effect Amount
OptionName = MASK_DARK
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.3

[OptionRangeFloat]
GUIName = Mask/Scanline Fade
OptionName = MASK_FADE
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.7

[OptionRangeFloat]
GUIName = Saturation
OptionName = sat
MinValue = 0.0
MaxValue = 3.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Flicker
OptionName = FLICK
MinValue = 0.0
MaxValue = 50.0
StepAmount = 1.0
DefaultValue = 10.0

[/configuration]
*/

#define pi 3.14159

#define blur_y GetOption(blury)/(SourceSize.y*2.0)
#define blur_x GetOption(blurx)/(SourceSize.x*2.0)
#define iTimer (float(GetTime())*2.0)
#define flicker GetOption(FLICK)/1000.0

// Distortion of scanlines, and end of screen alpha.
vec2 Warp(vec2 pos)
{
    pos  = pos*2.0-1.0;    
    pos *= vec2(1.0 + (pos.y*pos.y)*0.03, 1.0 + (pos.x*pos.x)*0.05);
    
    return pos*0.5 + 0.5;
}


void main()
{
    vec2 vTexCoord  = GetCoordinates();
    vec2 texSize = 1.0 / GetInvNativePixelSize();
    vec4 SourceSize = vec4(texSize, 1.0 / texSize);
    
    float maskFade = 0.3333*GetOption(MASK_FADE);
    float omega    = 2.0*pi*SourceSize.y;

    vec2 pos,corn;
    if (GetOption(Curvature) == 1.0) 
{
    pos = Warp(vTexCoord.xy);
    corn = min(pos,vec2(1.0)-pos); // This is used to mask the rounded
    corn.x = 0.00001/corn.x;           // corners later on
   
}

     else pos = vTexCoord;    
     float OGL2Pos = pos.y*SourceSize.y;
     float cent = floor(OGL2Pos)+0.5;
     float ycoord = cent*SourceSize.w; 
     ycoord = mix(pos.y,ycoord,0.6);
     pos = vec2(pos.x,ycoord);


     vec3 sample1 = sin(iTimer)*flicker + SampleLocation(vec2(pos.x + blur_x, pos.y - blur_y)).rgb;
     vec3 sample2 =                0.5*SampleLocation(pos).rgb;
     vec3 sample3 = sin(iTimer)*flicker + SampleLocation(vec2(pos.x - blur_x, pos.y + blur_y)).rgb;
    
     vec3 colour = vec3 (sample1.r*0.5  + sample2.r, 
                         sample1.g*0.25 + sample2.g + sample3.g*0.25, 
                                          sample2.b + sample3.b*0.5);
    
     vec3 interl = colour;
     vec3 lumweight=vec3(0.22,0.71,0.07);
     float lumsat = dot(colour,lumweight);
   
     vec3 graycolour = vec3(lumsat);
     colour = vec3(mix(graycolour,colour.rgb,sat));

     float SCANAMOUNT = mix(GetOption(HIGHSCANAMOUNT1),GetOption(HIGHSCANAMOUNT2),max(max(colour.r,colour.g),colour.b));
         

    if (SourceSize.y > 400.0) {
    colour ;
    } 
else {
    colour *= SCANAMOUNT * sin(fract(OGL2Pos)*3.14159)+1.0-SCANAMOUNT;
    colour *= SCANAMOUNT * sin(fract(1.0-OGL2Pos)*3.14159)+1.0-SCANAMOUNT;
    colour *= SCANAMOUNT * sin(fract(1.0+OGL2Pos)*3.14159)+1.0-SCANAMOUNT;
    }

     float steps; if (GetOption(TYPE) == 0.0) steps = 0.5; else steps = 0.3333;
     float whichmask = fract(vTexCoord.x*GetWindowSize().x*steps);
     float mask = 1.0 + float(whichmask < steps) * (-GetOption(MASK_DARK));

    colour.rgb = mix(mask*colour, colour, dot(colour.rgb,vec3(maskFade)));

    if (GetOption(Curvature) == 1.0 && corn.y < corn.x || GetOption(Curvature) == 1.0 && corn.x < 0.00001 )
    colour = vec3(0.0); 

    SetOutput(vec4(colour.rgb, 1.0));
}
