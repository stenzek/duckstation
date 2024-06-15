//    Pixellate Shader
//    Copyright (c) 2011, 2012 Fes
//    Permission to use, copy, modify, and/or distribute this software for any
//    purpose with or without fee is hereby granted, provided that the above
//    copyright notice and this permission notice appear in all copies.
//    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
//    SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
//    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//    (Fes gave their permission to have this shader distributed under this
//    licence in this forum post:
//        http://board.byuu.org/viewtopic.php?p=57295#p57295


/*
[configuration]

[OptionRangeFloat]
GUIName = Linear Gamma Weight
OptionName = INTERPOLATE_IN_LINEAR_GAMMA
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[/configuration]
*/

void main()
{
   vec2 vTexCoord  = GetCoordinates();
   vec2 SourceSize = 1.0 / GetInvNativePixelSize();
   vec2 texelSize  = 1.0 / SourceSize.xy;
   vec2 OutputSize = GetWindowSize().xy;

   vec2 range = vec2(abs(SourceSize.x / (OutputSize.x * SourceSize.x)), abs(SourceSize.y / (OutputSize.y * SourceSize.y)));
   range = range / 2.0 * 0.999;

   float left   = vTexCoord.x - range.x;
   float top    = vTexCoord.y + range.y;
   float right  = vTexCoord.x + range.x;
   float bottom = vTexCoord.y - range.y;
   
   vec3 topLeftColor;
   vec3 bottomRightColor;
   vec3 bottomLeftColor;
   vec3 topRightColor;

   if (GetOption(INTERPOLATE_IN_LINEAR_GAMMA) > 0.5){
   topLeftColor     = pow(SampleLocation((floor(vec2(left, top)     / texelSize) + vec2(0.5)) * texelSize).rgb, vec3(2.2));
   bottomRightColor = pow(SampleLocation((floor(vec2(right, bottom) / texelSize) + vec2(0.5)) * texelSize).rgb, vec3(2.2));
   bottomLeftColor  = pow(SampleLocation((floor(vec2(left, bottom)  / texelSize) + vec2(0.5)) * texelSize).rgb, vec3(2.2));
   topRightColor    = pow(SampleLocation((floor(vec2(right, top)    / texelSize) + vec2(0.5)) * texelSize).rgb, vec3(2.2));
   }else{
   topLeftColor     = SampleLocation((floor(vec2(left, top)     / texelSize) + vec2(0.5)) * texelSize).rgb;
   bottomRightColor = SampleLocation((floor(vec2(right, bottom) / texelSize) + vec2(0.5)) * texelSize).rgb;
   bottomLeftColor  = SampleLocation((floor(vec2(left, bottom)  / texelSize) + vec2(0.5)) * texelSize).rgb;
   topRightColor    = SampleLocation((floor(vec2(right, top)    / texelSize) + vec2(0.5)) * texelSize).rgb;}

   vec2 border = clamp(round(vTexCoord / texelSize) * texelSize, vec2(left, bottom), vec2(right, top));

   float totalArea = 4.0 * range.x * range.y;

   vec3 averageColor;
   averageColor  = ((border.x - left)  * (top - border.y)    / totalArea) * topLeftColor;
   averageColor += ((right - border.x) * (border.y - bottom) / totalArea) * bottomRightColor;
   averageColor += ((border.x - left)  * (border.y - bottom) / totalArea) * bottomLeftColor;
   averageColor += ((right - border.x) * (top - border.y)    / totalArea) * topRightColor;

   vec4 color = (GetOption(INTERPOLATE_IN_LINEAR_GAMMA) > 0.5) ? vec4(pow(averageColor, vec3(1.0 / 2.2)), 1.0) : vec4(averageColor, 1.0);

   SetOutput(color);
}
