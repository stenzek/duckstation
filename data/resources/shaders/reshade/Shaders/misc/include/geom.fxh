#ifndef GEOM_PARAMS_H
#define GEOM_PARAMS_H

/*
    Geom Shader - a modified CRT-Geom without CRT features made to be appended/integrated
    into any other shaders and provide curvature/warping/oversampling features.

    Adapted by Hyllian (2024).
*/


/*
    CRT-interlaced

    Copyright (C) 2010-2012 cgwg, Themaister and DOLLS

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    (cgwg gave their consent to have the original version of this shader
    distributed under the GPL in this message:

    http://board.byuu.org/viewtopic.php?p=26075#p26075

    "Feel free to distribute my shaders under the GPL. After all, the
    barrel distortion code was taken from the Curvature shader, which is
    under the GPL."
    )
    This shader variant is pre-configured with screen curvature
*/


uniform bool geom_curvature <
	ui_type = "radio";
	ui_category = "Geom Curvature";
	ui_label = "Geom Curvature Toggle";
> = 0.0;

uniform float geom_R <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 10.0;
	ui_step = 0.1;
	ui_category = "Geom Curvature";
	ui_label = "Geom Curvature Radius";
> = 2.0;

uniform float geom_d <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 3.0;
	ui_step = 0.1;
	ui_category = "Geom Curvature";
	ui_label = "Geom Distance";
> = 1.5;

uniform bool geom_invert_aspect <
	ui_type = "radio";
	ui_category = "Geom Curvature";
	ui_label = "Geom Curvature Aspect Inversion";
> = 0.0;

uniform float geom_cornersize <
	ui_type = "drag";
	ui_min = 0.001;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_category = "Geom Curvature";
	ui_label = "Geom Corner Size";
> = 0.03;

uniform float geom_cornersmooth <
	ui_type = "drag";
	ui_min = 80.0;
	ui_max = 2000.0;
	ui_step = 100.0;
	ui_category = "Geom Curvature";
	ui_label = "Geom Corner Smoothness";
> = 1000.0;

uniform float geom_x_tilt <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_category = "Geom Curvature";
	ui_label = "Geom Horizontal Tilt";
> = 0.0;

uniform float geom_y_tilt <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_category = "Geom Curvature";
	ui_label = "Geom Vertical Tilt";
> = 0.0;

uniform float geom_overscan_x <
	ui_type = "drag";
	ui_min = -125.0;
	ui_max = 125.0;
	ui_step = 0.5;
	ui_category = "Geom Curvature";
	ui_label = "Geom Horiz. Overscan %";
> = 100.0;

uniform float geom_overscan_y <
	ui_type = "drag";
	ui_min = -125.0;
	ui_max = 125.0;
	ui_step = 0.5;
	ui_category = "Geom Curvature";
	ui_label = "Geom Vert. Overscan %";
> = 100.0;

uniform float centerx <
	ui_type = "drag";
	ui_min = -100.0;
	ui_max = 100.0;
	ui_step = 0.1;
	ui_category = "Geom Curvature";
	ui_label = "Image Center X";
> = 0.00;

uniform float centery <
	ui_type = "drag";
	ui_min = -100.0;
	ui_max = 100.0;
	ui_step = 0.1;
	ui_category = "Geom Curvature";
	ui_label = "Image Center Y";
> = 0.00;



// Macros.
#define FIX(c) max(abs(c), 1e-5);

// aspect ratio
#define aspect     (geom_invert_aspect==true?float2(ViewportHeight/ViewportWidth,1.0):float2(1.0,ViewportHeight/ViewportWidth))


float intersect(float2 xy, float2 sinangle, float2 cosangle)
{
    float A = dot(xy,xy) + geom_d*geom_d;
    float B, C;

    B = 2.0*(geom_R*(dot(xy,sinangle) - geom_d*cosangle.x*cosangle.y) - geom_d*geom_d);
    C = geom_d*geom_d + 2.0*geom_R*geom_d*cosangle.x*cosangle.y;

    return (-B-sqrt(B*B - 4.0*A*C))/(2.0*A);
}

float2 bkwtrans(float2 xy, float2 sinangle, float2 cosangle)
{
    float  c      = intersect(xy, sinangle, cosangle);
    float2 point  = (c.xx*xy + geom_R.xx*sinangle) / geom_R.xx;
    float2 poc    = point/cosangle;
    float2 tang   = sinangle/cosangle;

    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;

    float a     = (-B + sqrt(B*B - 4.0*A*C)) / (2.0*A);
    float2 uv   = (point - a*sinangle) / cosangle;
    float r     = FIX(geom_R*acos(a));
    
    return uv*r/sin(r/geom_R);
}

float2 fwtrans(float2 uv, float2 sinangle, float2 cosangle)
{
    float r = FIX(sqrt(dot(uv, uv)));
    uv *= sin(r/geom_R)/r;
    float x = 1.0 - cos(r/geom_R);
    float D;
    
    D = geom_d/geom_R + x*cosangle.x*cosangle.y + dot(uv,sinangle);

    return geom_d*(uv*cosangle - x*sinangle)/D;
}

float3 maxscale(float2 sinangle, float2 cosangle)
{
    float2 c = bkwtrans(-geom_R * sinangle / (1.0 + geom_R/geom_d*cosangle.x*cosangle.y), sinangle, cosangle);
    float2 a = 0.5.xx*aspect;

    float2 lo = float2(fwtrans(float2(-a.x,  c.y), sinangle, cosangle).x,
                       fwtrans(float2( c.x, -a.y), sinangle, cosangle).y)/aspect;
    float2 hi = float2(fwtrans(float2(+a.x,  c.y), sinangle, cosangle).x,
                       fwtrans(float2( c.x, +a.y), sinangle, cosangle).y)/aspect;

    return float3((hi+lo)*aspect*0.5,max(hi.x-lo.x, hi.y-lo.y));
}

float2 transform(float2 coord, float2 sinangle, float2 cosangle, float3 stretch)
{
    coord = (coord - 0.5.xx)*aspect*stretch.z + stretch.xy;
    
    return (bkwtrans(coord, sinangle, cosangle) /
        float2(geom_overscan_x / 100.0, geom_overscan_y / 100.0)/aspect + 0.5.xx);
}


float corner(float2 coord)
{
           coord = min(coord, 1.0.xx - coord) * aspect;
    float2 cdist = geom_cornersize.xx;
           coord = (cdist - min(coord, cdist));
    float   dist = sqrt(dot(coord, coord));
    
    return clamp((cdist.x - dist)*geom_cornersmooth, 0.0, 1.0);
}

float fwidth(float value)
{
    return abs(ddx(value)) + abs(ddy(value));
}

#endif  //  GEOM_PARAMS_H
