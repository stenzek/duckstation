//    Hyllian's DDT Shader

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
GUIName = Bilinear Fallback Threshold
OptionName = BIL_FALLBACK
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.6

[/configuration]
*/

const vec3 Y = vec3(0.2126729, 0.7151522, 0.0721750);

float luma(vec3 color)
{
    return dot(color, Y);
}

vec3 bilinear(float p, float q, vec3 A, vec3 B, vec3 C, vec3 D)
{
    return ((1-p)*(1-q)*A + p*(1-q)*B + (1-p)*q*C + p*q*D);
}

void main()
{
    vec2 texCoord = GetCoordinates();
    vec2 nativeSize = 1.0 / GetInvNativePixelSize();

    vec2 loc = texCoord*nativeSize;
    vec2 pos = fract(loc) - vec2(0.5, 0.5); // pos = pixel position
    vec2 dir = sign(pos); // dir = pixel direction

    vec2 dx = vec2(1.0/nativeSize.x, 0.0);
    vec2 dy = vec2(0.0, 1.0/nativeSize.y);

    vec2 g1 = dir*dx;
    vec2 g2 = dir*dy;

    vec2 tc = (floor(loc)+vec2(0.5,0.5))/nativeSize;

    vec3 A = SampleLocation(tc       ).rgb;
    vec3 B = SampleLocation(tc +g1   ).rgb;
    vec3 C = SampleLocation(tc    +g2).rgb;
    vec3 D = SampleLocation(tc +g1+g2).rgb;

    float a = luma(A);
    float b = luma(B);
    float c = luma(C);
    float d = luma(D);

    float p = abs(pos.x);
    float q = abs(pos.y);

    float k = distance(pos,g1);
    float l = distance(pos,g2);

    float wd1 = abs(a-d);
    float wd2 = abs(b-c);

    vec3 color = bilinear(p, q, A, B, C, D);

    if ( wd1 < wd2 )
    {
        if (k < l)
        {
            C = A + D - B;
        }
        else
        {
            B = A + D - C;
        }
    }
    else if (wd1 > wd2)
    {
        D = B + C - A;
    }


    vec3 ddt = bilinear(p, q, A, B, C, D);

    color = mix(color, ddt, smoothstep(0.0, BIL_FALLBACK, abs(wd2-wd1)));

    SetOutput(vec4(color, 1.0));
}
