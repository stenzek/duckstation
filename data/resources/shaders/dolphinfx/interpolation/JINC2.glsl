
//   Hyllian's jinc windowed-jinc 2-lobe with anti-ringing Shader
   
//   Copyright (C) 2011-2024 Hyllian - sergiogdb@gmail.com

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
GUIName = Window Sinc Param
OptionName = JINC2_WINDOW_SINC
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.50

[OptionRangeFloat]
GUIName = Sinc Param
OptionName = JINC2_SINC
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.88

[OptionRangeFloat]
GUIName = Anti-ringing Strength
OptionName = JINC2_AR_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.1
DefaultValue = 0.5

[/configuration]
*/

#define halfpi  1.5707963267948966192313216916398
#define pi    3.1415926535897932384626433832795
#define wa    (JINC2_WINDOW_SINC*pi)
#define wb    (JINC2_SINC*pi)

// Calculates the distance between two points
float d(vec2 pt1, vec2 pt2)
{
  vec2 v = pt2 - pt1;
  return sqrt(dot(v,v));
}

vec3 min4(vec3 a, vec3 b, vec3 c, vec3 d)
{
    return min(a, min(b, min(c, d)));
}

vec3 max4(vec3 a, vec3 b, vec3 c, vec3 d)
{
    return max(a, max(b, max(c, d)));
}

vec4 resampler(vec4 x)
{
	vec4 res;
	res.x = (x.x==0.0) ?  wa*wb  :  sin(x.x*wa)*sin(x.x*wb)/(x.x*x.x);
	res.y = (x.y==0.0) ?  wa*wb  :  sin(x.y*wa)*sin(x.y*wb)/(x.y*x.y);
	res.z = (x.z==0.0) ?  wa*wb  :  sin(x.z*wa)*sin(x.z*wb)/(x.z*x.z);
	res.w = (x.w==0.0) ?  wa*wb  :  sin(x.w*wa)*sin(x.w*wb)/(x.w*x.w);
	return res;
}

void main()
{
    vec2 SourceSize = 1.0 / GetInvNativePixelSize();
    vec2 invSourceSize = 1.0 / SourceSize;
    vec2 vTexCoord = GetCoordinates();

    vec3 color;
    mat4x4 weights;

    vec2 dx = vec2(1.0, 0.0);
    vec2 dy = vec2(0.0, 1.0);

    vec2 pc = vTexCoord*SourceSize;

    vec2 tc = (floor(pc-vec2(0.5,0.5))+vec2(0.5,0.5));
     
    weights[0] = resampler(vec4(d(pc, tc    -dx    -dy), d(pc, tc           -dy), d(pc, tc    +dx    -dy), d(pc, tc+2.0*dx    -dy)));
    weights[1] = resampler(vec4(d(pc, tc    -dx       ), d(pc, tc              ), d(pc, tc    +dx       ), d(pc, tc+2.0*dx       )));
    weights[2] = resampler(vec4(d(pc, tc    -dx    +dy), d(pc, tc           +dy), d(pc, tc    +dx    +dy), d(pc, tc+2.0*dx    +dy)));
    weights[3] = resampler(vec4(d(pc, tc    -dx+2.0*dy), d(pc, tc       +2.0*dy), d(pc, tc    +dx+2.0*dy), d(pc, tc+2.0*dx+2.0*dy)));

    dx = dx * invSourceSize;
    dy = dy * invSourceSize;
    tc = tc * invSourceSize;
     
     // reading the texels
     
    vec3 c00 = SampleLocation(tc    -dx    -dy).xyz;
    vec3 c10 = SampleLocation(tc           -dy).xyz;
    vec3 c20 = SampleLocation(tc    +dx    -dy).xyz;
    vec3 c30 = SampleLocation(tc+2.0*dx    -dy).xyz;
    vec3 c01 = SampleLocation(tc    -dx       ).xyz;
    vec3 c11 = SampleLocation(tc              ).xyz;
    vec3 c21 = SampleLocation(tc    +dx       ).xyz;
    vec3 c31 = SampleLocation(tc+2.0*dx       ).xyz;
    vec3 c02 = SampleLocation(tc    -dx    +dy).xyz;
    vec3 c12 = SampleLocation(tc           +dy).xyz;
    vec3 c22 = SampleLocation(tc    +dx    +dy).xyz;
    vec3 c32 = SampleLocation(tc+2.0*dx    +dy).xyz;
    vec3 c03 = SampleLocation(tc    -dx+2.0*dy).xyz;
    vec3 c13 = SampleLocation(tc       +2.0*dy).xyz;
    vec3 c23 = SampleLocation(tc    +dx+2.0*dy).xyz;
    vec3 c33 = SampleLocation(tc+2.0*dx+2.0*dy).xyz;

    //  Get min/max samples
    vec3 min_sample = min4(c11, c21, c12, c22);
    vec3 max_sample = max4(c11, c21, c12, c22);

    color = mat4x3(c00, c10, c20, c30) * weights[0];
    color+= mat4x3(c01, c11, c21, c31) * weights[1];
    color+= mat4x3(c02, c12, c22, c32) * weights[2];
    color+= mat4x3(c03, c13, c23, c33) * weights[3];
    color = color/(dot(weights * vec4(1.0), vec4(1.0)));

    // Anti-ringing
    vec3 aux = color;
    color = clamp(color, min_sample, max_sample);

    color = mix(aux, color, JINC2_AR_STRENGTH);
 
    // final sum and weight normalization
    SetOutput(vec4(color, 1.0));
}
