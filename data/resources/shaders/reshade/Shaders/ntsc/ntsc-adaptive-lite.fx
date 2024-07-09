#include "ReShade.fxh"

// NTSC-Adaptive-Lite  -  Faster for 2-Phase games (only 15 taps!)
// based on Themaister's NTSC shader


uniform int quality <
    ui_type = "combo";
    ui_items = "Custom\0Svideo\0Composite\0RF\0";
    ui_label = "NTSC Preset";
> = 2;

uniform bool ntsc_fields <
    ui_type = "radio";
    ui_label = "NTSC Merge Fields";
> = false;

uniform int ntsc_phase <
    ui_type = "combo";
    ui_items = "Auto\0(2-Phase)\0(3-Phase)\0";
    ui_label = "NTSC Phase";
> = 0;

uniform float ntsc_scale <
    ui_type = "drag";
    ui_min = 0.20;
    ui_max = 3.0;
    ui_step = 0.05;
    ui_label = "NTSC Resolution Scaling";
> = 1.0;

uniform float ntsc_sat <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 2.0;
    ui_step = 0.01;
    ui_label = "NTSC Color Saturation";
> = 1.0;

uniform float ntsc_bright <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.5;
    ui_step = 0.01;
    ui_label = "NTSC Brightness";
> = 1.0;

uniform float cust_fringing <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 5.0;
    ui_step = 0.1;
    ui_label = "NTSC Custom Fringing Value";
> = 0.0;

uniform float cust_artifacting <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 5.0;
    ui_step = 0.1;
    ui_label = "NTSC Custom Artifacting Value";
> = 0.0;

uniform float chroma_scale <
    ui_type = "drag";
    ui_min = 0.2;
    ui_max = 4.0;
    ui_step = 0.1;
    ui_label = "NTSC Chroma Scaling";
> = 1.0;

uniform float ntsc_artifacting_rainbow <
    ui_type = "drag";
    ui_min = -1.0;
    ui_max = 1.0;
    ui_step = 0.1;
    ui_label = "NTSC Artifacting Rainbow Effect";
> = 0.0;

uniform bool linearize <
    ui_type = "radio";
    ui_label = "NTSC Linearize Output Gamma";
> = false;


uniform float  FrameCount < source = "framecount"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  BufferWidth < source = "bufferwidth"; >;
uniform float  BufferHeight < source = "bufferheight"; >;


// RGB16f is the same as float_framebuffer.
texture2D tNTSC_P0 < pooled = false; > {Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D sNTSC_P0{Texture=tNTSC_P0;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=LINEAR;MinFilter=LINEAR;};

#define PI 3.14159265
#define OutputSize float2(BufferWidth,BufferHeight)

struct ST_VertexOut
{
    float2 pix_no          : TEXCOORD1;
    float  phase           : TEXCOORD2;
    float  BRIGHTNESS      : TEXCOORD3;
    float  SATURATION      : TEXCOORD4;
    float  FRINGING        : TEXCOORD5;
    float  ARTIFACTING     : TEXCOORD6;
    float  CHROMA_MOD_FREQ : TEXCOORD7;
    float  MERGE           : TEXCOORD8;
};


// Vertex shader generating a triangle covering the entire screen
void VS_NTSC_ADAPTIVE_P0(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 TexCoord : TEXCOORD, out ST_VertexOut vVARS)
{
    TexCoord.x = (id == 2) ? 2.0 : 0.0;
    TexCoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float res = ntsc_scale;
    float OriginalSize = 1.0/NormalizedNativePixelSize.x;
    float2 SourceSize  = 1.0/NormalizedNativePixelSize;

    if (res < 1.0) vVARS.pix_no = TexCoord * SourceSize.xy * (res * OutputSize.xy / SourceSize.xy); else
                   vVARS.pix_no = TexCoord * SourceSize.xy * (      OutputSize.xy / SourceSize.xy);
    vVARS.phase = (ntsc_phase < 1) ? ((OriginalSize > 300.0) ? 2.0 : 3.0) : ((ntsc_phase > 2) ? 3.0 : 2.0);
    
    float Quality = float(quality-1);

    res = max(res, 1.0);    
    vVARS.CHROMA_MOD_FREQ = (vVARS.phase < 2.5) ? (4.0 * PI / 15.0) : (PI / 3.0);
    vVARS.ARTIFACTING = (Quality > -0.5) ? Quality * 0.5*(res+1.0) : cust_artifacting;
    vVARS.FRINGING = (Quality > -0.5) ? Quality : cust_fringing;
    vVARS.SATURATION = ntsc_sat;
    vVARS.BRIGHTNESS = ntsc_bright;    
    vVARS.pix_no.x = vVARS.pix_no.x * res;

    vVARS.MERGE = (Quality == 2.0 || vVARS.phase < 2.5) ? 0.0 : 1.0;
    vVARS.MERGE = (Quality == -1.0) ? float(ntsc_fields == true) : vVARS.MERGE;
}

#define mix_mat float3x3(vVARS.BRIGHTNESS, vVARS.FRINGING, vVARS.FRINGING, vVARS.ARTIFACTING, 2.0 * vVARS.SATURATION, 0.0, vVARS.ARTIFACTING, 0.0, 2.0 * vVARS.SATURATION)

static const float3x3 yiq2rgb_mat = float3x3(
   1.0, 0.956, 0.6210,
   1.0, -0.2720, -0.6474,
   1.0, -1.1060, 1.7046);

float3 yiq2rgb(float3 yiq)
{
   return mul(yiq2rgb_mat, yiq);
}

static const float3x3 yiq_mat = float3x3(
      0.2989, 0.5870, 0.1140,
      0.5959, -0.2744, -0.3216,
      0.2115, -0.5229, 0.3114
);

float3 rgb2yiq(float3 col)
{
   return mul(yiq_mat, col);
}

static const float3 Y = float3( 0.299,  0.587,  0.114);

float df3(float3 a, float3 b, float3 c)
{
    return dot(smoothstep(0.0, 0.56, 3.0*(b - a) * (b - c)), Y);
}


float4 PS_NTSC_ADAPTIVE_P0(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in ST_VertexOut vVARS) : SV_Target
{
   float3 col = tex2D(ReShade::BackBuffer, vTexCoord).rgb;
   float3 yiq = rgb2yiq(col);
   float3 yiq2 = yiq;

    float4 SourceSize  = float4(1.0/NormalizedNativePixelSize, NormalizedNativePixelSize);

   float mod1 = 2.0;
   float mod2 = 3.0;

   float2 dx = float2(1.0, 0.0)*SourceSize.zw;
   float2 dy = float2(0.0, 1.0)*SourceSize.zw;

   float3 C = tex2D(ReShade::BackBuffer, vTexCoord    ).xyz;
   float3 L = tex2D(ReShade::BackBuffer, vTexCoord -dx).xyz;
   float3 R = tex2D(ReShade::BackBuffer, vTexCoord +dx).xyz;
   float3 U = tex2D(ReShade::BackBuffer, vTexCoord -dy).xyz;
   float3 D = tex2D(ReShade::BackBuffer, vTexCoord +dy).xyz;
   float3 UL = tex2D(ReShade::BackBuffer, vTexCoord -dx -dy).xyz;
   float3 UR = tex2D(ReShade::BackBuffer, vTexCoord +dx -dy).xyz;
   float3 DL = tex2D(ReShade::BackBuffer, vTexCoord -dx +dy).xyz;
   float3 DR = tex2D(ReShade::BackBuffer, vTexCoord +dx +dy).xyz;

   float hori = step(0.01,(df3(L, C, R) * df3(UL, U, UR) * df3(DL, D, DR)));
   float vert = 1.0 - step(0.01,(df3(U, C, D) * df3(UL, L, DL) * df3(UR, R, DR)));

   float blend = hori * vert * ntsc_artifacting_rainbow;

if (vVARS.MERGE > 0.5)
{
   float chroma_phase2 = (vVARS.phase < 2.5) ? PI * ((vVARS.pix_no.y % mod1) + ((FrameCount+1.) % 2.)) : 0.6667 * PI * ((vVARS.pix_no.y % mod2) + ((FrameCount+1.) % 2.));
   float mod_phase2 = (blend + 1.0) * chroma_phase2 + vVARS.pix_no.x * vVARS.CHROMA_MOD_FREQ;
   float i_mod2 = cos(mod_phase2);
   float q_mod2 = sin(mod_phase2);
   yiq2.yz *= float2(i_mod2, q_mod2); // Modulate.
   yiq2 = mul(mix_mat, yiq2); // Cross-talk.
   yiq2.yz *= float2(i_mod2, q_mod2); // Demodulate.   
}
  
   float chroma_phase = (vVARS.phase < 2.5) ? PI * ((vVARS.pix_no.y % mod1) + ((FrameCount+1.) % 2.)) : 0.6667 * PI * ((vVARS.pix_no.y % mod2) + ((FrameCount+1.) % 2.));
   float mod_phase = (blend + 1.0) * chroma_phase + vVARS.pix_no.x * vVARS.CHROMA_MOD_FREQ;


   float i_mod = cos(mod_phase);
   float q_mod = sin(mod_phase);

   yiq.yz *= float2(i_mod, q_mod); // Modulate.
   yiq = mul(mix_mat, yiq); // Cross-talk.
   yiq.yz *= float2(i_mod, q_mod); // Demodulate.
      
   yiq = (vVARS.MERGE < 0.5) ? yiq : 0.5*(yiq+yiq2);
   
   return float4(yiq, 1.0);
}


// Vertex shader generating a triangle covering the entire screen
void VS_NTSC_ADAPTIVE_P1(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 TexCoord : TEXCOORD)
{
    TexCoord.x = (id == 2) ? 2.0 : 0.0;
    TexCoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}


float3 fetch_offset(sampler2D Source, float2 tex, float offset, float2 one_x)
{
   /* Insert chroma scaling. Thanks to guest.r ideas. */

   float3 yiq;

   yiq.x  = tex2D(Source, tex + float2((offset) * (one_x.x), 0.0)).x;
   yiq.yz = tex2D(Source, tex + float2((offset) * (one_x.y), 0.0)).yz;

   return yiq;

/*  Old code
   return texture(Source, vTexCoord + float2((offset) * (one_x), 0.0)).xyz;
*/
}

/* These are accurate and normalized coeffs. */
static const int TAPS_3_phase = 24;
static const float luma_filter_3_phase[25] = {
-0.0000120203033684164,
-0.0000221465589348544,
-0.0000131553320142694,
-0.0000120203033684164,
-0.0000499802614018372,
-0.000113942875690297,
-0.000122153082899506,
-5.61214E-06,
0.000170520303591422,
0.000237204986579451,
0.000169644281482376,
0.000285695210375719,
0.000984598849305758,
0.0020187339488074,
0.00200232553469184,
-0.000909904964181485,
-0.00704925890919635,
-0.0132231937269633,
-0.0126072491817548,
0.00246092210875218,
0.0358691302651096,
0.0840185734607569,
0.135566921437963,
0.175265691355518,
0.190181351796957};

/* These are accurate and normalized coeffs. */
static const float chroma_filter_3_phase[25] = {
-0.000135741056915795,
-0.000568115749081878,
-0.00130605691082327,
-0.00231369942971182,
-0.00350569685928248,
-0.00474731062446688,
-0.00585980203774502,
-0.00663114046295865,
-0.00683148404964774,
-0.00623234997205773,
-0.00462792764511295,
-0.00185665431957684,
0.00217899013894782,
0.00749647783836479,
0.0140227874371299,
0.021590863169257,
0.0299437436530477,
0.0387464461271303,
0.0476049759842373,
0.0560911497485196,
0.0637713405314321,
0.0702368383153846,
0.0751333078160781,
0.0781868487834974,
0.0792244191487085};


/* These are accurate and normalized coeffs. Though they don't produce ideal smooth vertical lines transparency. */
static const int TAPS_2_phase = 15;
static const float luma_filter_2_phase[16] = {
0.00134372867555492,
0.00294231678339247,
0.00399617683765551,
0.00303632635732925,
-0.00110556727614119,
-0.00839970341605087,
-0.0169515379999301,
-0.0229874881474188,
-0.0217113019865528,
-0.00889151239892142,
0.0173269874254282,
0.0550969075027442,
0.098655909675851,
0.139487291941771,
0.168591277052964,
0.17914037794465};


/* These are accurate and normalized coeffs. */
static const float chroma_filter_2_phase[16] = {
0.00406084767413046,
0.00578573638571078,
0.00804447474387669,
0.0109152541019797,
0.0144533032717188,
0.0186765858322351,
0.0235518468184291,
0.0289834149989225,
0.034807373222651,
0.0407934139180355,
0.0466558344725586,
0.0520737649339226,
0.0567190701585739,
0.0602887575746322,
0.0625375226221969,
0.0633055985408521};



float4 PS_NTSC_ADAPTIVE_P1(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float4 SourceSize  = float4(BufferWidth, 1.0/NormalizedNativePixelSize.y, 1.0/BufferWidth, NormalizedNativePixelSize.y);

   float res = ntsc_scale;
   float OriginalSize = 1.0/NormalizedNativePixelSize.x;
   float3 signal = float3(0.0, 0.0, 0.0);
   float phase = (ntsc_phase < 1) ? ((OriginalSize > 300.0) ? 2.0 : 3.0) : ((ntsc_phase > 1) ? 3.0 : 2.0);

   float chroma_scale = phase > 2.5 ? min(chroma_scale, 2.2) : chroma_scale/2.0;
   float2 one_x = (SourceSize.z / res) * float2(1.0, 1.0 / chroma_scale);

   float2 tex = vTexCoord;

   if(phase < 2.5)
   {
      float3 sums = fetch_offset(sNTSC_P0, tex, 0.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 0.0, one_x);
      signal += sums * float3(luma_filter_2_phase[0], chroma_filter_2_phase[0], chroma_filter_2_phase[0]);
      sums = fetch_offset(sNTSC_P0, tex, 1.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 1.0, one_x);
      signal += sums * float3(luma_filter_2_phase[1], chroma_filter_2_phase[1], chroma_filter_2_phase[1]);
      sums = fetch_offset(sNTSC_P0, tex, 2.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 2.0, one_x);
      signal += sums * float3(luma_filter_2_phase[2], chroma_filter_2_phase[2], chroma_filter_2_phase[2]);
      sums = fetch_offset(sNTSC_P0, tex, 3.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 3.0, one_x);
      signal += sums * float3(luma_filter_2_phase[3], chroma_filter_2_phase[3], chroma_filter_2_phase[3]);
      sums = fetch_offset(sNTSC_P0, tex, 4.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 4.0, one_x);
      signal += sums * float3(luma_filter_2_phase[4], chroma_filter_2_phase[4], chroma_filter_2_phase[4]);
      sums = fetch_offset(sNTSC_P0, tex, 5.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 5.0, one_x);
      signal += sums * float3(luma_filter_2_phase[5], chroma_filter_2_phase[5], chroma_filter_2_phase[5]);
      sums = fetch_offset(sNTSC_P0, tex, 6.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 6.0, one_x);
      signal += sums * float3(luma_filter_2_phase[6], chroma_filter_2_phase[6], chroma_filter_2_phase[6]);
      sums = fetch_offset(sNTSC_P0, tex, 7.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 7.0, one_x);
      signal += sums * float3(luma_filter_2_phase[7], chroma_filter_2_phase[7], chroma_filter_2_phase[7]);
      sums = fetch_offset(sNTSC_P0, tex, 8.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 8.0, one_x);
      signal += sums * float3(luma_filter_2_phase[8], chroma_filter_2_phase[8], chroma_filter_2_phase[8]);
      sums = fetch_offset(sNTSC_P0, tex, 9.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 9.0, one_x);
      signal += sums * float3(luma_filter_2_phase[9], chroma_filter_2_phase[9], chroma_filter_2_phase[9]);
      sums = fetch_offset(sNTSC_P0, tex, 10.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 10.0, one_x);
      signal += sums * float3(luma_filter_2_phase[10], chroma_filter_2_phase[10], chroma_filter_2_phase[10]);
      sums = fetch_offset(sNTSC_P0, tex, 11.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 11.0, one_x);
      signal += sums * float3(luma_filter_2_phase[11], chroma_filter_2_phase[11], chroma_filter_2_phase[11]);
      sums = fetch_offset(sNTSC_P0, tex, 12.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 12.0, one_x);
      signal += sums * float3(luma_filter_2_phase[12], chroma_filter_2_phase[12], chroma_filter_2_phase[12]);
      sums = fetch_offset(sNTSC_P0, tex, 13.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 13.0, one_x);
      signal += sums * float3(luma_filter_2_phase[13], chroma_filter_2_phase[13], chroma_filter_2_phase[13]);
      sums = fetch_offset(sNTSC_P0, tex, 14.0 - 15.0, one_x) + fetch_offset(sNTSC_P0, tex, 15.0 - 14.0, one_x);
      signal += sums * float3(luma_filter_2_phase[14], chroma_filter_2_phase[14], chroma_filter_2_phase[14]);
      
      signal += tex2D(sNTSC_P0, vTexCoord).xyz *
         float3(luma_filter_2_phase[TAPS_2_phase], chroma_filter_2_phase[TAPS_2_phase], chroma_filter_2_phase[TAPS_2_phase]);
   }
   else if(phase > 2.5)
   {
      for (int i = 0; i < TAPS_3_phase; i++)
      {
         float offset = float(i);

         float3 sums = fetch_offset(sNTSC_P0, tex, offset - float(TAPS_3_phase), one_x) +
            fetch_offset(sNTSC_P0, tex, float(TAPS_3_phase) - offset, one_x);
         signal += sums * float3(luma_filter_3_phase[i], chroma_filter_3_phase[i], chroma_filter_3_phase[i]);
      }
      signal += tex2D(sNTSC_P0, vTexCoord).xyz *
         float3(luma_filter_3_phase[TAPS_3_phase], chroma_filter_3_phase[TAPS_3_phase], chroma_filter_3_phase[TAPS_3_phase]);
   }

   float3 rgb = yiq2rgb(signal);

   if(linearize == false) return float4(rgb, 1.0);
   else return pow(float4(rgb, 1.0), float4(2.2, 2.2, 2.2, 2.2));
}

technique NTSC_ADAPTIVE
{
    pass
    {
        VertexShader = VS_NTSC_ADAPTIVE_P0;
        PixelShader  = PS_NTSC_ADAPTIVE_P0;
        RenderTarget = tNTSC_P0;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_NTSC_ADAPTIVE_P1;
    }
}
