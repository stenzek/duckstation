#include "ReShade.fxh"

//
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
//

#ifndef CRTS_DEBUG
	#define CRTS_DEBUG	0
#endif

#ifndef CRTS_2_TAP
	#define CRTS_2_TAP	0
#endif

uniform bool CRTS_WARP <
	ui_type = "boolean";
	ui_label = "Enable Warping [CRT Lottes 2.0]";
> = true;

uniform float CRTS_WARP_X <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 512.0;
	ui_label = "CRT Warping X [CRT Lottes 2.0]";
> = 64.0;

uniform float CRTS_WARP_Y <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 512.0;
	ui_label = "CRT Warping Y [CRT Lottes 2.0]";
> = 48.0;

uniform bool CRTS_TONE <
	ui_type = "boolean";
	ui_label = "Enable CRT Tonemapping [CRT Lottes 2.0]";
> = true;

uniform bool CRTS_CONTRAST <
	ui_type = "boolean";
	ui_label = "Enable CRT Contrast [CRT Lottes 2.0]";
> = false;

uniform bool CRTS_SATURATION <
	ui_type = "boolean";
	ui_label = "Enable CRT Saturation [CRT Lottes 2.0]";
> = false;

uniform int CRTS_MASK_TYPE <
	ui_type = "combo";
	ui_items = "None\0Aperture Grille\0Aperture Grille (Lite)\0Shadow Mask\0";
	ui_label = "Mask Type [CRT Lottes 2.0]";
> = 2;

//--------------------------------------------------------------
// Scanline thinness
//  0.50 = fused scanlines
//  0.70 = recommended default
//  1.00 = thinner scanlines (too thin)

uniform float INPUT_THIN <
	ui_type = "drag";
	ui_min = 0.5;
	ui_max = 1.0;
	ui_label = "Scanlines Thinnes [CRT Lottes 2.0]";
> = 0.70;

//--------------------------------------------------------------
// Horizonal scan blur
//  -3.0 = pixely
//  -2.5 = default
//  -2.0 = smooth
//  -1.0 = too blurry

uniform float INPUT_BLUR <
	ui_type = "drag";
	ui_min = -3.0;
	ui_max = 0.0;
	ui_label = "Horizontal Scan Blur [CRT Lottes 2.0]";
> = -2.5;

//--------------------------------------------------------------
// Shadow mask effect, ranges from,
//  0.25 = large amount of mask (not recommended, too dark)
//  0.50 = recommended default
//  1.00 = no shadow mask

uniform float INPUT_MASK <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_label = "Shadow Mask Intensity [CRT Lottes 2.0]";
> = 0.5;

//--------------------------------------------------------------

uniform int INPUT_X <
	ui_type = "drag";
	ui_min = 1;
	ui_max = BUFFER_WIDTH;
	ui_label = "Resolution Width [CRT Lottes 2.0]";
> = 640;

uniform int INPUT_Y <
	ui_type = "drag";
	ui_min = 1;
	ui_max = BUFFER_HEIGHT;
	ui_label = "Resolution Height [CRT Lottes 2.0]";
> = 480;

//--------------------------------------------------------------
// Setup the function which returns input image color

void ToLinear(inout float3 color)
{
	float3 c1 = color.rgb / 12.92;
	float3 c2 = pow((color.rgb + 0.055)/1.055, 2.4);

	color.r = (color.r <= 0.04045) ? c1.r : c2.r;
	color.g = (color.g <= 0.04045) ? c1.g : c2.g;
	color.b = (color.b <= 0.04045) ? c1.b : c2.b;
}

void ToSRGB(inout float3 color)
{
	float3 c1 = color.rgb * 12.92;
	float3 c2 = 1.055 * pow(color.rgb, 0.4166) - 0.055;

	color.r = (color.r < 0.0031308) ? c1.r : c2.r;
	color.g = (color.g < 0.0031308) ? c1.g : c2.g;
	color.b = (color.b < 0.0031308) ? c1.b : c2.b;
}

float3 CrtsFetch(float2 uv)
{
	float3 color = tex2D(ReShade::BackBuffer, uv).rgb;
	ToLinear(color);
	return color;
}

float4 CrtsTone(
float contrast,
float saturation,
float thin,
float mask)
{
	
	mask = INPUT_MASK;

  	if (CRTS_MASK_TYPE <= 0){
		mask=1.0;
	}


  	if(CRTS_MASK_TYPE == 2){
		mask=0.5+INPUT_MASK*0.5;	
	}

  	float4 ret;
  	float midOut=0.18/((1.5-thin)*(0.5*mask+0.5));
  	float pMidIn=pow(0.18,contrast);
  	ret.x=contrast;
  	ret.y=((-pMidIn)+midOut)/((1.0-pMidIn)*midOut);
  	ret.z=((-pMidIn)*midOut+pMidIn)/(midOut*(-pMidIn)+midOut);
  	ret.w=contrast+saturation;
  	return ret;
}

float3 CrtsMask(float2 pos,float dark)
{
	
	if (CRTS_MASK_TYPE == 1){   
		float3 m=dark;
		float x=frac(pos.x*(1.0/3.0));
		if(x<(1.0/3.0))m.r=1.0;
		else if(x<(2.0/3.0))m.g=1.0;
		else m.b=1.0;
		return m;
	} else if (CRTS_MASK_TYPE == 2){
		float3 m=1.0;
		float x=frac(pos.x*(1.0/3.0));
		if(x<(1.0/3.0))m.r=dark;
		else if(x<(2.0/3.0))m.g=dark;
		else m.b=dark;
		return m;
	} else if(CRTS_MASK_TYPE <= 0){
	   return 1.0;	
	} else if(CRTS_MASK_TYPE >= 3){
		pos.x+=pos.y*3.0;
		float3 m=dark;
		float x=frac(pos.x*(1.0/6.0));
		if(x<(1.0/3.0))m.r=1.0;
		else if(x<(2.0/3.0))m.g=1.0;
		else m.b=1.0;
		return m;
	} else {
		return 0.0;
	}
 }

 float3 CrtsFilter(
//--------------------------------------------------------------
  // SV_POSITION, fragCoord.xy
  float2 ipos,
//--------------------------------------------------------------
  // inputSize / outputSize (in pixels)
  float2 inputSizeDivOutputSize,     
//--------------------------------------------------------------
  // 0.5 * inputSize (in pixels)
  float2 halfInputSize,
//--------------------------------------------------------------
  // 1.0 / inputSize (in pixels)
  float2 rcpInputSize,
//--------------------------------------------------------------
  // 1.0 / outputSize (in pixels)
  float2 rcpOutputSize,
//--------------------------------------------------------------
  // 2.0 / outputSize (in pixels)
  float2 twoDivOutputSize,   
//--------------------------------------------------------------
  // inputSize.y
  float inputHeight,
//--------------------------------------------------------------
  // Warp scanlines but not phosphor mask
  //  0.0 = no warp
  //  1.0/64.0 = light warping
  //  1.0/32.0 = more warping
  // Want x and y warping to be different (based on aspect)
  float2 warp,
//--------------------------------------------------------------
  // Scanline thinness
  //  0.50 = fused scanlines
  //  0.70 = recommended default
  //  1.00 = thinner scanlines (too thin)
  // Shared with CrtsTone() function
  float thin,
//--------------------------------------------------------------
  // Horizonal scan blur
  //  -3.0 = pixely
  //  -2.5 = default
  //  -2.0 = smooth
  //  -1.0 = too blurry
  float blur,
//--------------------------------------------------------------
  // Shadow mask effect, ranges from,
  //  0.25 = large amount of mask (not recommended, too dark)
  //  0.50 = recommended default
  //  1.00 = no shadow mask
  // Shared with CrtsTone() function
  float mask,
//--------------------------------------------------------------
  // Tonal curve parameters generated by CrtsTone()
  float4 tone
//--------------------------------------------------------------
 ){
//--------------------------------------------------------------
	#if (CRTS_DEBUG == 1)
		float2 uv=ipos*rcpOutputSize;
		// Show second half processed, and first half un-processed
		if(uv.x<0.5)
		{
				// Force nearest to get squares
				uv*=1.0/rcpInputSize;
				uv=floor(uv)+float2(0.5,0.5);
				uv*=rcpInputSize;
				float3 color=CrtsFetch(uv);
				return color;
		}
	#endif

  	float2 pos;
	float vin;

	if (CRTS_WARP){
		// Convert to {-1 to 1} range
		pos=ipos*twoDivOutputSize-float2(1.0,1.0);
		// Distort pushes image outside {-1 to 1} range
		pos*=float2(1.0+(pos.y*pos.y)*warp.x,1.0+(pos.x*pos.x)*warp.y);
		// TODO: Vignette needs optimization
		vin=1.0-((1.0-saturate(pos.x*pos.x))*(1.0-saturate(pos.y*pos.y)));
		vin=saturate((-vin)*inputHeight+inputHeight);
		// Leave in {0 to inputSize}
		pos=pos*halfInputSize+halfInputSize;     
	} else {
		pos=ipos*inputSizeDivOutputSize;
	}
	
  	// Snap to center of first scanline
  	float y0=floor(pos.y-0.5)+0.5;

	#if (CRTS_2_TAP == 1)
		// Using Inigo's "Improved Texture Interpolation"
		// http://iquilezles.org/www/articles/texture/texture.htm
		pos.x+=0.5;
		float xi=floor(pos.x);
		float xf=pos.x-xi;
		xf=xf*xf*xf*(xf*(xf*6.0-15.0)+10.0);  
		float x0=xi+xf-0.5;
		float2 p=float2(x0*rcpInputSize.x,y0*rcpInputSize.y);     
		// Coordinate adjusted bilinear fetch from 2 nearest scanlines
		float3 colA=CrtsFetch(p);
		p.y+=rcpInputSize.y;
		float3 colB=CrtsFetch(p);
	#else
		// Snap to center of one of four pixels
		float x0=floor(pos.x-1.5)+0.5;
		// Inital UV position
		float2 p=float2(x0*rcpInputSize.x,y0*rcpInputSize.y);     
		// Fetch 4 nearest texels from 2 nearest scanlines
		float3 colA0=CrtsFetch(p);
		p.x+=rcpInputSize.x;
		float3 colA1=CrtsFetch(p);
		p.x+=rcpInputSize.x;
		float3 colA2=CrtsFetch(p);
		p.x+=rcpInputSize.x;
		float3 colA3=CrtsFetch(p);
		p.y+=rcpInputSize.y;
		float3 colB3=CrtsFetch(p);
		p.x-=rcpInputSize.x;
		float3 colB2=CrtsFetch(p);
		p.x-=rcpInputSize.x;
		float3 colB1=CrtsFetch(p);
		p.x-=rcpInputSize.x;
		float3 colB0=CrtsFetch(p);
	#endif

  	// Vertical filter
  	// Scanline intensity is using sine wave
  	// Easy filter window and integral used later in exposure
  	float off=pos.y-y0;
  	float pi2=6.28318530717958;
  	float hlf=0.5;
  	float scanA=cos(min(0.5,  off *thin     )*pi2)*hlf+hlf;
  	float scanB=cos(min(0.5,(-off)*thin+thin)*pi2)*hlf+hlf;

	#if (CRTS_2_TAP == 1)
	if (CRTS_WARP){
			// Get rid of wrong pixels on edge
			scanA*=vin;
			scanB*=vin;
	}
		// Apply vertical filter
		float3 color=(colA*scanA)+(colB*scanB);
	#else
		 // Horizontal kernel is simple gaussian filter
		float off0=pos.x-x0;
		float off1=off0-1.0;
		float off2=off0-2.0;
		float off3=off0-3.0;
		float pix0=exp2(blur*off0*off0);
		float pix1=exp2(blur*off1*off1);
		float pix2=exp2(blur*off2*off2);
		float pix3=exp2(blur*off3*off3);
		float pixT=rcp(pix0+pix1+pix2+pix3);

	if (CRTS_WARP){
		// Get rid of wrong pixels on edge
		pixT*=vin;
		}
		scanA*=pixT;
		scanB*=pixT;
		// Apply horizontal and vertical filters
		float3 color=
			(colA0*pix0+colA1*pix1+colA2*pix2+colA3*pix3)*scanA +
			(colB0*pix0+colB1*pix1+colB2*pix2+colB3*pix3)*scanB;
	#endif
	
		// Apply phosphor mask          
		color*=CrtsMask(ipos,mask);
		// Optional color processing
	if (CRTS_TONE){
		// Tonal control, start by protecting from /0
		float peak=max(1.0/(256.0*65536.0),max(color.r,max(color.g,color.b)));
		// Compute the ratios of {R,G,B}
		float3 ratio=color*rcp(peak);
		// Apply tonal curve to peak value
	if (CRTS_CONTRAST){
			peak=pow(peak,tone.x);
	}
		peak=peak*rcp(peak*tone.y+tone.z);
		// Apply saturation
	if (CRTS_SATURATION){
			ratio=pow(ratio,float3(tone.w,tone.w,tone.w));
	}
		// Reconstruct color
		return ratio*peak;
	} else {
		return color;
	}
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void PS_CRTLottes2018(float4 vpos : SV_Position, float2 texcoord : TEXCOORD, out float4 color : SV_Target0)
{
	color = tex2D(ReShade::BackBuffer, texcoord.xy);

 	color.rgb=CrtsFilter(
  	vpos.xy,
  	float2(INPUT_X,INPUT_Y)/ReShade::ScreenSize.xy,
  	float2(INPUT_X,INPUT_Y)*0.5,
  	1.0/float2(INPUT_X,INPUT_Y),
  	1.0/ReShade::ScreenSize.xy,
  	2.0/ReShade::ScreenSize.xy,
  	INPUT_Y,
  	float2(1.0/CRTS_WARP_X,1.0/CRTS_WARP_Y),
  	INPUT_THIN,
  	INPUT_BLUR,
  	INPUT_MASK,
  	CrtsTone(1.0,0.0,INPUT_THIN,INPUT_MASK));
 	
	 ToSRGB(color.rgb);
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++


technique CRTLottes2018
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_CRTLottes2018;
	}
}