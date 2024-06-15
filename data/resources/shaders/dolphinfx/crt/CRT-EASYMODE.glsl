//    CRT Shader by EasyMode
//    License: GPL

//    A flat CRT shader ideally for 1080p or higher displays.

//    Recommended Settings:

//    Video
//    - Aspect Ratio:  4:3
//    - Integer Scale: Off

//    Shader
//    - Filter: Nearest
//    - Scale:  Don't Care

//    Example RGB Mask Parameter Settings:

//    Aperture Grille (Default)
//    - Dot Width:  1
//    - Dot Height: 1
//    - Stagger:    0

//    Lottes' Shadow Mask
//    - Dot Width:  2
//    - Dot Height: 1
//    - Stagger:    3


/*
[configuration]

[OptionRangeFloat]
GUIName = Sharpness Horizontal
OptionName = SHARPNESS_H
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Sharpness Vertical
OptionName = SHARPNESS_V
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Mask Strength
OptionName = MASK_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.3

[OptionRangeFloat]
GUIName = Mask Dot Width
OptionName = MASK_DOT_WIDTH
MinValue = 1.0
MaxValue = 100.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Mask Dot Height
OptionName = MASK_DOT_HEIGHT
MinValue = 1.0
MaxValue = 100.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Mask Stagger
OptionName = MASK_STAGGER
MinValue = 0.0
MaxValue = 100.0
StepAmount = 1.0
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Mask Size
OptionName = MASK_SIZE
MinValue = 1.0
MaxValue = 100.0
StepAmount = 1.0
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Scanline Strength
OptionName = SCANLINE_STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Scanline Beam Width Min.
OptionName = SCANLINE_BEAM_WIDTH_MIN
MinValue = 0.5
MaxValue = 5.0
StepAmount = 0.5
DefaultValue = 1.5

[OptionRangeFloat]
GUIName = Scanline Beam Width Max.
OptionName = SCANLINE_BEAM_WIDTH_MAX
MinValue = 0.5
MaxValue = 5.0
StepAmount = 0.5
DefaultValue = 1.5

[OptionRangeFloat]
GUIName = Scanline Brightness Min.
OptionName = SCANLINE_BRIGHT_MIN
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.35

[OptionRangeFloat]
GUIName = Scanline Brightness Max.
OptionName = SCANLINE_BRIGHT_MAX
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.65

[OptionRangeFloat]
GUIName = Scanline Cutoff
OptionName = SCANLINE_CUTOFF
MinValue = 1.0
MaxValue = 1000.0
StepAmount = 1.0
DefaultValue = 400.0

[OptionRangeFloat]
GUIName = Gamma Input
OptionName = GAMMA_INPUT
MinValue = 0.1
MaxValue = 5.0
StepAmount = 0.1
DefaultValue = 2.0

[OptionRangeFloat]
GUIName = Gamma Output
OptionName = GAMMA_OUTPUT
MinValue = 0.1
MaxValue = 5.0
StepAmount = 0.1
DefaultValue = 1.8

[OptionRangeFloat]
GUIName = Brightness Boost
OptionName = BRIGHT_BOOST
MinValue = 1.0
MaxValue = 2.0
StepAmount = 0.01
DefaultValue = 1.2

[OptionRangeFloat]
GUIName = Dilation
OptionName = DILATION
MinValue = 0.0
MaxValue = 1.0
StepAmount = 1.0
DefaultValue = 1.0

[/configuration]
*/

#define FIX(c) max(abs(c), 1e-5)
#define PI 3.141592653589

#define TEX2D(c) dilate(SampleLocation(c))

// Set to 0 to use linear filter and gain speed
#define ENABLE_LANCZOS 1

vec4 dilate(vec4 col)
{
    vec4 x = mix(vec4(1.0), col, GetOption(DILATION));

    return col * x;
}

float curve_distance(float x, float sharp)
{

/*
    apply half-circle s-curve to distance for sharper (more pixelated) interpolation
    single line formula for Graph Toy:
    0.5 - sqrt(0.25 - (x - step(0.5, x)) * (x - step(0.5, x))) * sign(0.5 - x)
*/

    float x_step = step(0.5, x);
    float curve = 0.5 - sqrt(0.25 - (x - x_step) * (x - x_step)) * sign(0.5 - x);

    return mix(x, curve, sharp);
}

mat4x4 get_color_matrix(vec2 co, vec2 dx)
{
    return mat4x4(TEX2D(co - dx), TEX2D(co), TEX2D(co + dx), TEX2D(co + 2.0 * dx));
}

vec3 filter_lanczos(vec4 coeffs, mat4x4 color_matrix)
{
    vec4 col        = color_matrix * coeffs;
    vec4 sample_min = min(color_matrix[1], color_matrix[2]);
    vec4 sample_max = max(color_matrix[1], color_matrix[2]);

    col = clamp(col, sample_min, sample_max);

    return col.rgb;
}

void main()
{
    vec2 vTexCoord = GetCoordinates();
    vec2 nativeSize = 1.0 / GetInvNativePixelSize();
    vec4 SourceSize = vec4(nativeSize, 1.0/nativeSize);

    vec2 dx     = vec2(SourceSize.z, 0.0);
    vec2 dy     = vec2(0.0, SourceSize.w);
    vec2 pix_co = vTexCoord * SourceSize.xy - vec2(0.5, 0.5);
    vec2 tex_co = (floor(pix_co) + vec2(0.5, 0.5)) * SourceSize.zw;
    vec2 dist   = fract(pix_co);
    float curve_x;
    vec3 col, col2;

#if ENABLE_LANCZOS
    curve_x = curve_distance(dist.x, GetOption(SHARPNESS_H) * GetOption(SHARPNESS_H));

    vec4 coeffs = PI * vec4(1.0 + curve_x, curve_x, 1.0 - curve_x, 2.0 - curve_x);

    coeffs = FIX(coeffs);
    coeffs = 2.0 * sin(coeffs) * sin(coeffs * 0.5) / (coeffs * coeffs);
    coeffs /= dot(coeffs, vec4(1.0));

    col  = filter_lanczos(coeffs, get_color_matrix(tex_co, dx));
    col2 = filter_lanczos(coeffs, get_color_matrix(tex_co + dy, dx));
#else
    curve_x = curve_distance(dist.x, GetOption(SHARPNESS_H));

    col  = mix(TEX2D(tex_co).rgb,      TEX2D(tex_co + dx).rgb,      curve_x);
    col2 = mix(TEX2D(tex_co + dy).rgb, TEX2D(tex_co + dx + dy).rgb, curve_x);
#endif

    col = mix(col, col2, curve_distance(dist.y, GetOption(SHARPNESS_V)));
    col = pow(col, vec3(GetOption(GAMMA_INPUT) / (GetOption(DILATION) + 1.0)));

    float luma        = dot(vec3(0.2126, 0.7152, 0.0722), col);
    float bright      = (max(col.r, max(col.g, col.b)) + luma) * 0.5;
    float scan_bright = clamp(bright, GetOption(SCANLINE_BRIGHT_MIN), GetOption(SCANLINE_BRIGHT_MAX));
    float scan_beam   = clamp(bright * GetOption(SCANLINE_BEAM_WIDTH_MAX), GetOption(SCANLINE_BEAM_WIDTH_MIN), GetOption(SCANLINE_BEAM_WIDTH_MAX));
    float scan_weight = 1.0 - pow(cos(vTexCoord.y * 2.0 * PI * SourceSize.y) * 0.5 + 0.5, scan_beam) * GetOption(SCANLINE_STRENGTH);

    float mask   = 1.0 - GetOption(MASK_STRENGTH);    
    vec2 mod_fac = floor(vTexCoord * GetWindowSize().xy * SourceSize.xy / (SourceSize.xy * vec2(GetOption(MASK_SIZE), GetOption(MASK_DOT_HEIGHT) * GetOption(MASK_SIZE))));
    int dot_no   = int(mod((mod_fac.x + mod(mod_fac.y, 2.0) * GetOption(MASK_STAGGER)) / GetOption(MASK_DOT_WIDTH), 3.0));
    vec3 mask_weight;

    if      (dot_no == 0) mask_weight = vec3(1.0,  mask, mask);
    else if (dot_no == 1) mask_weight = vec3(mask, 1.0,  mask);
    else                  mask_weight = vec3(mask, mask, 1.0);

    if (SourceSize.y >= GetOption(SCANLINE_CUTOFF)) 
        scan_weight = 1.0;

    col2 = col.rgb;
    col *= vec3(scan_weight);
    col  = mix(col, col2, scan_bright);
    col *= mask_weight;
    col  = pow(col, vec3(1.0 / GetOption(GAMMA_OUTPUT)));

    SetOutput(vec4(col * GetOption(BRIGHT_BOOST), 1.0));
}
