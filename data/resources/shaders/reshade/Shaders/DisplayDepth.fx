/*
  DisplayDepth by CeeJay.dk (with many updates and additions by the Reshade community)

  Visualizes the depth buffer. The distance of pixels determine their brightness.
  Close objects are dark. Far away objects are bright.
  Use this to configure the depth input preprocessor definitions (RESHADE_DEPTH_INPUT_*).
*/

#include "ReShade.fxh"

// -- Basic options --
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
#define TEXT_UPSIDE_DOWN "1"
#define TEXT_UPSIDE_DOWN_ALTER "0"
#else
#define TEXT_UPSIDE_DOWN "0"
#define TEXT_UPSIDE_DOWN_ALTER "1"
#endif
#if RESHADE_DEPTH_INPUT_IS_REVERSED
#define TEXT_REVERSED "1"
#define TEXT_REVERSED_ALTER "0"
#else
#define TEXT_REVERSED "0"
#define TEXT_REVERSED_ALTER "1"
#endif
#if RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
#define TEXT_LOGARITHMIC "1"
#define TEXT_LOGARITHMIC_ALTER "0"
#else
#define TEXT_LOGARITHMIC "0"
#define TEXT_LOGARITHMIC_ALTER "1"
#endif

// "ui_text" was introduced in ReShade 4.5, so cannot show instructions in older versions

uniform int iUIPresentType <
    ui_label = "Present type";
    ui_label_ja_jp = "画面効果";
    ui_type = "combo";
    ui_items = "Depth map\0Normal map\0Show both (Vertical 50/50)\0";
    ui_items_ja_jp = "深度マップ\0法線マップ\0両方を表示 (左右分割)\0";
#if __RESHADE__ < 40500
    ui_tooltip =
#else
    ui_text =
#endif
        "The right settings need to be set in the dialog that opens after clicking the \"Edit global preprocessor definitions\" button above.\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN is currently set to " TEXT_UPSIDE_DOWN ".\n"
        "If the Depth map is shown upside down set it to " TEXT_UPSIDE_DOWN_ALTER ".\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_REVERSED is currently set to " TEXT_REVERSED ".\n"
        "If close objects in the Depth map are bright and far ones are dark set it to " TEXT_REVERSED_ALTER ".\n"
        "Also try this if you can see the normals, but the depth view is all black.\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC is currently set to " TEXT_LOGARITHMIC ".\n"
        "If the Normal map has banding artifacts (extra stripes) set it to " TEXT_LOGARITHMIC_ALTER ".";
    ui_text_ja_jp =
#if ADDON_ADJUST_DEPTH
        "Adjust Depthアドオンのインストールを検出しました。\n"
        "'設定に保存して反映する'ボタンをクリックすると、このエフェクトで調節した全ての変数が共通設定に反映されます。\n"
        "または、上の'プリプロセッサの定義を編集'ボタンをクリックした後に開くダイアログで直接編集する事もできます。";
#else
        "調節が終わったら、上の'プリプロセッサの定義を編集'ボタンをクリックした後に開くダイアログに入力する必要があります。\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWNは現在" TEXT_UPSIDE_DOWN "に設定されています。\n"
        "深度マップが上下逆さまに表示されている場合は" TEXT_UPSIDE_DOWN_ALTER "に変更して下さい。\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_REVERSEDは現在" TEXT_REVERSED "に設定されています。\n"
        "画面効果が深度マップのとき、近くの形状がより白く、遠くの形状がより黒い場合は" TEXT_REVERSED_ALTER "に変更して下さい。\n"
        "また、法線マップで形が判別出来るが、深度マップが真っ暗に見えるという場合も、この設定の変更を試して下さい。\n"
        "\n"
        "RESHADE_DEPTH_INPUT_IS_LOGARITHMICは現在" TEXT_LOGARITHMIC "に設定されています。\n"
        "画面効果に実際のレンダリングと合致しない縞模様がある場合は" TEXT_LOGARITHMIC_ALTER "に変更して下さい。";
#endif
    ui_tooltip_ja_jp =
        "'深度マップ'は、形状の遠近を白黒で表現します。正しい見え方では、近くの形状ほど黒く、遠くの形状ほど白くなります。\n"
        "'法線マップ'は、形状を滑らかに表現します。正しい見え方では、全体的に青緑風で、地平線を見たときに地面が緑掛かった色合いになります。\n"
        "'両方を表示 (左右分割)'が選択された場合は、左に法線マップ、右に深度マップを表示します。";
> = 2;

uniform bool bUIShowOffset <
    ui_label = "Blend Depth map into the image (to help with finding the right offset)";
    ui_label_ja_jp = "透かし比較";
    ui_tooltip_ja_jp = "補正作業を支援するために、画面効果を半透過で適用します。";
> = false;

uniform bool bUIUseLivePreview <
    ui_category = "Preview settings";
    ui_category_ja_jp = "基本的な補正";
#if __RESHADE__ <= 50902
    ui_category_closed = true;
#elif !ADDON_ADJUST_DEPTH
    ui_category_toggle = true;
#endif
    ui_label = "Show live preview and ignore preprocessor definitions";
    ui_label_ja_jp = "プリプロセッサの定義を無視 (補正プレビューをオン)";
    ui_tooltip = "Enable this to preview with the current preset settings instead of the global preprocessor settings.";
    ui_tooltip_ja_jp =
        "共通設定に保存されたプリプロセッサの定義ではなく、これより下のプレビュー設定を使用するには、これを有効にします。\n"
#if ADDON_ADJUST_DEPTH
        "設定の準備が出来たら、'設定に保存して反映する'ボタンをクリックしてから、このチェックボックスをオフにして下さい。"
#else
        "設定の準備が出来たら、上の'プリプロセッサの定義を編集'ボタンをクリックした後に開くダイアログに入力して下さい。"
#endif
        "\n\n"
        "プレビューをオンにした場合と比較して画面効果がまったく同じになれば、正しく設定が反映されています。";
> = false;

#if __RESHADE__ <= 50902
uniform int iUIUpsideDown <
#else
uniform bool iUIUpsideDown <
#endif
    ui_category = "Preview settings";
    ui_label = "Upside Down";
    ui_label_ja_jp = "深度バッファの上下反転を修正";
#if __RESHADE__ <= 50902
    ui_type = "combo";
    ui_items = "Off\0On\0";
#endif
    ui_text_ja_jp =
        "\n"
#if ADDON_ADJUST_DEPTH
        "項目にカーソルを合わせると、設定が必要な状況の説明が表示されます。"
#else
        "項目にカーソルを合わせると、設定が必要な状況の説明と、プリプロセッサの定義が表示されます。"
#endif
    ;
    ui_tooltip_ja_jp =
        "深度マップが上下逆さまに表示されている場合は変更して下さい。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=値\n"
        "定義値は次の通りです。オンの場合は1、オフの場合は0を指定して下さい。\n"
        "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1\n"
        "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0"
#endif
        ;
> = RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN;

#if __RESHADE__ <= 50902
uniform int iUIReversed <
#else
uniform bool iUIReversed <
#endif
    ui_category = "Preview settings";
    ui_label = "Reversed";
    ui_label_ja_jp = "深度バッファの奥行反転を修正";
#if __RESHADE__ <= 50902
    ui_type = "combo";
    ui_items = "Off\0On\0";
#endif
    ui_tooltip_ja_jp =
        "画面効果が深度マップのとき、近くの形状が明るく、遠くの形状が暗い場合は変更して下さい。\n"
        "また、法線マップで形が判別出来るが、深度マップが真っ暗に見えるという場合も、この設定の変更を試して下さい。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_INPUT_IS_REVERSED=値\n"
        "定義値は次の通りです。オンの場合は1、オフの場合は0を指定して下さい。\n"
        "RESHADE_DEPTH_INPUT_IS_REVERSED=1\n"
        "RESHADE_DEPTH_INPUT_IS_REVERSED=0"
#endif
        ;
> = RESHADE_DEPTH_INPUT_IS_REVERSED;

#if __RESHADE__ <= 50902
uniform int iUILogarithmic <
#else
uniform bool iUILogarithmic <
#endif
    ui_category = "Preview settings";
    ui_label = "Logarithmic";
    ui_label_ja_jp = "深度バッファを対数分布として扱うように修正";
#if __RESHADE__ <= 50902
    ui_type = "combo";
    ui_items = "Off\0On\0";
#endif
    ui_tooltip = "Change this setting if the displayed surface normals have stripes in them.";
    ui_tooltip_ja_jp =
        "画面効果に実際のゲーム画面と合致しない縞模様がある場合は変更して下さい。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=値\n"
        "定義値は次の通りです。オンの場合は1、オフの場合は0を指定して下さい。\n"
        "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=1\n"
        "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0"
#endif
        ;
> = RESHADE_DEPTH_INPUT_IS_LOGARITHMIC;

// -- Advanced options --

uniform float2 fUIScale <
    ui_category = "Preview settings";
    ui_label = "Scale";
    ui_label_ja_jp = "拡大率";
    ui_type = "drag";
    ui_text =
        "\n"
        " * Advanced options\n"
        "\n"
        "The following settings also need to be set using \"Edit global preprocessor definitions\" above in order to take effect.\n"
        "You can preview how they will affect the Depth map using the controls below.\n"
        "\n"
        "It is rarely necessary to change these though, as their defaults fit almost all games.\n\n";
    ui_text_ja_jp =
        "\n"
        " * その他の補正 (不定形またはその他)\n"
        "\n"
        "これより下は、深度バッファが不定形など、特別なケース向けの設定です。\n"
        "通常はこれより上の'基本的な補正'のみでほとんどのゲームに適合します。\n"
        "また、これらの設定は画質の向上にはまったく役に立ちません。\n\n";
    ui_tooltip =
        "Best use 'Present type'->'Depth map' and enable 'Offset' in the options below to set the scale.\n"
        "Use these values for:\nRESHADE_DEPTH_INPUT_X_SCALE=<left value>\nRESHADE_DEPTH_INPUT_Y_SCALE=<right value>\n"
        "\n"
        "If you know the right resolution of the games depth buffer then this scale value is simply the ratio\n"
        "between the correct resolution and the resolution Reshade thinks it is.\n"
        "For example:\n"
        "If it thinks the resolution is 1920 x 1080, but it's really 1280 x 720 then the right scale is (1.5 , 1.5)\n"
        "because 1920 / 1280 is 1.5 and 1080 / 720 is also 1.5, so 1.5 is the right scale for both the x and the y";
    ui_tooltip_ja_jp =
        "深度バッファの解像度がクライアント解像度と異なる場合に変更して下さい。\n"
        "このスケール値は、深度バッファの解像度とクライアント解像度との単純な比率になります。\n"
        "深度バッファの解像度が1280×720でクライアント解像度が1920×1080の場合、横の比率が1920÷1280、縦の比率が1080÷720となります。\n"
        "計算した結果を設定すると、値はそれぞれX_SCALE=1.5、Y_SCALE=1.5となります。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_INPUT_X_SCALE=横の値\n"
        "RESHADE_DEPTH_INPUT_Y_SCALE=縦の値\n"
        "定義値は次の通りです。横の値はX_SCALE、縦の値はY_SCALEに指定して下さい。\n"
        "RESHADE_DEPTH_INPUT_X_SCALE=1.0\n"
        "RESHADE_DEPTH_INPUT_Y_SCALE=1.0"
#endif
        ;
    ui_min = 0.0; ui_max = 2.0;
    ui_step = 0.001;
> = float2(RESHADE_DEPTH_INPUT_X_SCALE, RESHADE_DEPTH_INPUT_Y_SCALE);

uniform int2 iUIOffset <
    ui_category = "Preview settings";
    ui_label = "Offset";
    ui_label_ja_jp = "位置オフセット";
    ui_type = "slider";
    ui_tooltip =
        "Best use 'Present type'->'Depth map' and enable 'Offset' in the options below to set the offset in pixels.\n"
        "Use these values for:\nRESHADE_DEPTH_INPUT_X_PIXEL_OFFSET=<left value>\nRESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET=<right value>";
    ui_tooltip_ja_jp =
        "深度バッファにレンダリングされた物体の形状が画面効果と重なり合っていない場合に変更して下さい。\n"
        "この値は、ピクセル単位で指定します。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET=横の値\n"
        "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET=縦の値\n"
        "定義値は次の通りです。横の値はX_PIXEL_OFFSET、縦の値はY_PIXEL_OFFSETに指定して下さい。\n"
        "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET=0.0\n"
        "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET=0.0"
#endif
        ;
    ui_min = -BUFFER_SCREEN_SIZE;
    ui_max = BUFFER_SCREEN_SIZE;
    ui_step = 1;
> = int2(RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET, RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET);

uniform float fUIFarPlane <
    ui_category = "Preview settings";
    ui_label = "Far Plane";
    ui_label_ja_jp = "遠点距離";
    ui_type = "drag";
    ui_tooltip = 
        "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=<value>\n"
        "Changing this value is not necessary in most cases.";
    ui_tooltip_ja_jp =
        "深度マップの色合いが距離感と合致しない、法線マップの表面が平面に見える、などの場合に変更して下さい。\n"
        "遠点距離を1000に設定すると、ゲームの描画距離が1000メートルであると見なします。\n\n"
        "このプレビュー画面はあくまでプレビューであり、ほとんどの場合、深度バッファは深度マップの色数より遥かに高い精度で表現されています。\n"
        "例えば、10m前後の距離の形状が純粋な黒に見えるからという理由で値を変更しないで下さい。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=値\n"
        "定義値は次の通りです。\n"
        "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0"
#endif
        ;
    ui_min = 0.0; ui_max = 1000.0;
    ui_step = 0.1;
> = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;

uniform float fUIDepthMultiplier <
    ui_category = "Preview settings";
    ui_label = "Multiplier";
    ui_label_ja_jp = "深度乗数";
    ui_type = "drag";
    ui_tooltip = "RESHADE_DEPTH_MULTIPLIER=<value>";
    ui_tooltip_ja_jp =
        "特定のエミュレータソフトウェアにおける深度バッファを修正するため、特別に追加された変数です。\n"
        "この値は僅かな変更でも計算式を破壊するため、設定すべき値を知らない場合は変更しないで下さい。"
#if !ADDON_ADJUST_DEPTH
        "\n\n"
        "定義名は次の通りです。文字は完全に一致する必要があり、半角大文字の英字とアンダーバーを用いなければなりません。\n"
        "RESHADE_DEPTH_MULTIPLIER=値\n"
        "定義値は次の通りです。\n"
        "RESHADE_DEPTH_MULTIPLIER=1.0"
#endif
        ;
    ui_min = 0.0; ui_max = 1000.0;
    ui_step = 0.001;
> = RESHADE_DEPTH_MULTIPLIER;

float GetLinearizedDepth(float2 texcoord)
{
    if (!bUIUseLivePreview)
    {
        return ReShade::GetLinearizedDepth(texcoord);
    }
    else
    {
        if (iUIUpsideDown) // RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
            texcoord.y = 1.0 - texcoord.y;

        texcoord.x /= fUIScale.x; // RESHADE_DEPTH_INPUT_X_SCALE
        texcoord.y /= fUIScale.y; // RESHADE_DEPTH_INPUT_Y_SCALE
        texcoord.x -= iUIOffset.x * BUFFER_RCP_WIDTH; // RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
        texcoord.y += iUIOffset.y * BUFFER_RCP_HEIGHT; // RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET

        float depth = tex2Dlod(ReShade::DepthBuffer, float4(texcoord, 0, 0)).x * fUIDepthMultiplier;

        const float C = 0.01;
        if (iUILogarithmic) // RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
            depth = (exp(depth * log(C + 1.0)) - 1.0) / C;

        if (iUIReversed) // RESHADE_DEPTH_INPUT_IS_REVERSED
            depth = 1.0 - depth;

        const float N = 1.0;
        depth /= fUIFarPlane - depth * (fUIFarPlane - N);

        return depth;
    }
}

float3 GetScreenSpaceNormal(float2 texcoord)
{
    float3 offset = float3(BUFFER_PIXEL_SIZE, 0.0);
    float2 posCenter = texcoord.xy;
    float2 posNorth  = posCenter - offset.zy;
    float2 posEast   = posCenter + offset.xz;

    float3 vertCenter = float3(posCenter - 0.5, 1) * GetLinearizedDepth(posCenter);
    float3 vertNorth  = float3(posNorth - 0.5,  1) * GetLinearizedDepth(posNorth);
    float3 vertEast   = float3(posEast - 0.5,   1) * GetLinearizedDepth(posEast);

    return normalize(cross(vertCenter - vertNorth, vertCenter - vertEast)) * 0.5 + 0.5;
}

void PS_DisplayDepth(in float4 position : SV_Position, in float2 texcoord : TEXCOORD, out float3 color : SV_Target)
{
    float3 depth = GetLinearizedDepth(texcoord).xxx;
    float3 normal = GetScreenSpaceNormal(texcoord);

    // Ordered dithering
#if 1
    const float dither_bit = 8.0; // Number of bits per channel. Should be 8 for most monitors.
    // Calculate grid position
    float grid_position = frac(dot(texcoord, (BUFFER_SCREEN_SIZE * float2(1.0 / 16.0, 10.0 / 36.0)) + 0.25));
    // Calculate how big the shift should be
    float dither_shift = 0.25 * (1.0 / (pow(2, dither_bit) - 1.0));
    // Shift the individual colors differently, thus making it even harder to see the dithering pattern
    float3 dither_shift_RGB = float3(dither_shift, -dither_shift, dither_shift); // Subpixel dithering
    // Modify shift acording to grid position.
    dither_shift_RGB = lerp(2.0 * dither_shift_RGB, -2.0 * dither_shift_RGB, grid_position);
    depth += dither_shift_RGB;
#endif

    color = depth;
    if (iUIPresentType == 1)
        color = normal;
    if (iUIPresentType == 2)
        color = lerp(normal, depth, step(BUFFER_WIDTH * 0.5, position.x));

    if (bUIShowOffset)
    {
        float3 color_orig = tex2D(ReShade::BackBuffer, texcoord).rgb;

        // Blend depth and back buffer color with 'overlay' so the offset is more noticeable
        color = lerp(2 * color * color_orig, 1.0 - 2.0 * (1.0 - color) * (1.0 - color_orig), max(color.r, max(color.g, color.b)) < 0.5 ? 0.0 : 1.0);
    }
}

technique DisplayDepth <
    ui_tooltip =
        "This shader helps you set the right preprocessor settings for depth input.\n"
        "To set the settings click on 'Edit global preprocessor definitions' and set them there - not in this shader.\n"
        "The settings will then take effect for all shaders, including this one.\n"  
        "\n"
        "By default calculated normals and depth are shown side by side.\n"
        "Normals (on the left) should look smooth and the ground should be greenish when looking at the horizon.\n"
        "Depth (on the right) should show close objects as dark and use gradually brighter shades the further away objects are.\n";
    ui_tooltip_ja_jp =
        "これは、深度バッファの入力をReShade側の計算式に合わせる調節をするための、設定作業の支援に特化した特殊な扱いのエフェクトです。\n"
        "初期状態では「両方を表示」が選択されており、左に法線マップ、右に深度マップが表示されます。\n"
        "\n"
        "法線マップ(左側)は、形状を滑らかに表現します。正しい設定では、全体的に青緑風で、地平線を見たときに地面が緑を帯びた色になります。\n"
        "深度マップ(右側)は、形状の遠近を白黒で表現します。正しい設定では、近くの形状ほど黒く、遠くの形状ほど白くなります。\n"
        "\n"
#if ADDON_ADJUST_DEPTH
        "設定を完了するには、DisplayDepth.fxエフェクトの変数の一覧にある'設定に保存して反映する'ボタンをクリックして下さい。\n"
#else
        "設定を完了するには、エフェクト変数の編集画面にある'プリプロセッサの定義を編集'ボタンをクリックした後に開くダイアログに入力して下さい。\n"
#endif
        "すると、インストール先のゲームに対して共通の設定として保存され、他のプリセットでも正しく表示されるようになります。";
>

{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_DisplayDepth;
    }
}
