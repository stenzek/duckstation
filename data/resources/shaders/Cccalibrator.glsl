/*==========================================*/
/*=======Calibrator by PavelDurov1488=======*/
/*==========It's not ready yet...===========*/
/*===I need to add blur, dynamic noise...===*/

/*
[configuration]


[OptionRangeFloat]
GUIName = Luminance(Y)
OptionName = LUMINANCE
MinValue = 0.000
MaxValue = 3.000
StepAmount = 0.050
DefaultValue = 1.200

[OptionRangeFloat]
GUIName = Orange-Cyan(I)
OptionName = ORANGECYAN
MinValue = 0.000
MaxValue = 3.000
StepAmount = 0.050
DefaultValue = 1.200

[OptionRangeFloat]
GUIName = Magenta-Green(Q)
OptionName = MAGENTAGREEN
MinValue = 0.000
MaxValue = 3.000
StepAmount = 0.050
DefaultValue = 1.200

[OptionRangeFloat]
GUIName = Black
OptionName = BLACK
MinValue = 0.000
MaxValue = 255.000
StepAmount = 1.000
DefaultValue = 10.000

[OptionRangeFloat]
GUIName = White
OptionName = WHITE
MinValue = 0.000
MaxValue = 255.000
StepAmount = 1.000
DefaultValue = 240.000

[OptionRangeFloat]
GUIName = Noise
OptionName = NOISE
MinValue = 0.000
MaxValue = 50.000
StepAmount = 1.000
DefaultValue = 10.000

[OptionRangeFloat]
GUIName = Saturation
OptionName = SATURATION
MinValue = 0.000
MaxValue = 100.000
StepAmount = 1.000
DefaultValue = 50.000

[/configuration]
*/

float pseudoNoise(vec2 co)
{
return fract(sin(dot(vec2(co.x+0.513,co.y+0.4124) ,vec2(12.9898,78.233))) * 43758.5453);// *fract(sin(dot(vec2(co.x+4.231,co.y+3.143) ,vec2(12.9898,78.233)*2.0)) * 43758.5453); //pseudo random number generator
}

CONSTANT vec3 RGBtoY = vec3(0.299, 0.587, 0.114);
CONSTANT vec3 RGBtoI = vec3(0.596,-0.275,-0.321);
CONSTANT vec3 RGBtoQ = vec3(0.212,-0.523, 0.311);
CONSTANT vec3 YIQtoR = vec3(1.0, 0.95568806036115671171, 0.61985809445637075388);
CONSTANT vec3 YIQtoG = vec3(1.0,-0.27158179694405859326,-0.64687381613840131330);
CONSTANT vec3 YIQtoB = vec3(1.0,-1.10817732668266195230, 1.70506455991918171490);  

void main()
{

  
  float2 texcoord = GetCoordinates();
  float2 time = float2(GetTime(), GetTime());
//  int FrameCount = GetFrameCount();
  float4 color = Sample();
//  float brightness_scale = GetOption(BRIGHTNESS_SCALE);

//vec3 czm_saturation(vec3 rgb, float adjustment)
//{
//    // Algorithm from Chapter 16 of OpenGL Shading Language
//    const vec3 W = vec3(0.2125, 0.7154, 0.0721);
//    vec3 intensity = vec3(dot(rgb, W));
//    return mix(intensity, rgb, adjustment);
//}

  // rgb->yiq
  float3 yuv;
  yuv.r = pow(dot(color.rgb, float3(0.299, 0.587, 0.114)),LUMINANCE);
  yuv.g = dot(color.rgb, float3(0.595716,-0.274453,-0.321263))*ORANGECYAN;
  yuv.b = dot(color.rgb, float3(0.211456,-0.522591, 0.311135))*MAGENTAGREEN;

  // apply brightness to y
//  yuv.r = saturate(yuv.r * brightness_scale);

  // yuv->rgb
  color.r = dot(yuv, float3(1.0, 0.95629572, 0.62102442));
  color.g = dot(yuv, float3(1.0,-0.27212210,-0.64738060));
  color.b = dot(yuv, float3(1.0,-1.10698902, 1.70461500));
  color.rgb = saturate(color.rgb*WHITE/255.0+BLACK/255.0-color.rgb*BLACK/255.0+(pseudoNoise(texcoord))*1.0/255.0);
  
  color.rgb = saturate(color.rgb+(pseudoNoise(vec2(color.r+color.b,color.g+color.b))-0.5)*NOISE/255.0);
  SetOutput(saturate(color));
}
