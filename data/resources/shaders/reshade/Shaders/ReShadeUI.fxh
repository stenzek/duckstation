#pragma once

#if !defined(__RESHADE__) || __RESHADE__ < 30000
#error "ReShade 3.0+ is required to use this header file"
#endif

#define RESHADE_VERSION(major,minor,build) (10000 * (major) + 100 * (minor) + (build))
#define SUPPORTED_VERSION(major,minor,build) (__RESHADE__ >= RESHADE_VERSION(major,minor,build))

// Since 3.0.0
// Commit current in-game user interface status
// https://github.com/crosire/reshade/commit/302bacc49ae394faedc2e29a296c1cebf6da6bb2#diff-82cf230afdb2a0d5174111e6f17548a5R1183
// Added various GUI related uniform variable annotations
// https://reshade.me/forum/releases/2341-3-0
#define __UNIFORM_INPUT_ANY    ui_type = "input";

#define __UNIFORM_INPUT_BOOL1  __UNIFORM_INPUT_ANY // It is unsupported on all version
#define __UNIFORM_INPUT_BOOL2  __UNIFORM_INPUT_ANY // It is unsupported on all version
#define __UNIFORM_INPUT_BOOL3  __UNIFORM_INPUT_ANY // It is unsupported on all version
#define __UNIFORM_INPUT_BOOL4  __UNIFORM_INPUT_ANY // It is unsupported on all version
#define __UNIFORM_INPUT_INT1   __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_INT2   __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_INT3   __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_INT4   __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_FLOAT1 __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_FLOAT2 __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_FLOAT3 __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_INPUT_FLOAT4 __UNIFORM_INPUT_ANY // If it was not supported in someday or now, please add information

// Since 4.0.1
// Change slider widget to be used with new "slider" instead of a "drag" type annotation
// https://github.com/crosire/reshade/commit/746229f31cd6f311a3e72a543e4f1f23faa23f11#diff-59405a313bd8cbfb0ca6dd633230e504R1701
// Changed slider widget to be used with < ui_type = "slider"; > instead of < ui_type = "drag"; >
// https://reshade.me/forum/releases/4772-4-0
#if SUPPORTED_VERSION(4,0,1)
#define __UNIFORM_DRAG_ANY    ui_type = "drag";

// Since 4.0.0
// Rework statistics tab and add drag widgets back
// https://github.com/crosire/reshade/commit/1b2c38795f00efd66c007da1f483f1441b230309
// Changed drag widget to a slider widget (old one is still available via < ui_type = "drag2"; >)
// https://reshade.me/forum/releases/4772-4-0
#elif SUPPORTED_VERSION(4,0,0)
#define __UNIFORM_DRAG_ANY    ui_type = "drag2";

// Since 3.0.0
// Commit current in-game user interface status
// https://github.com/crosire/reshade/commit/302bacc49ae394faedc2e29a296c1cebf6da6bb2#diff-82cf230afdb2a0d5174111e6f17548a5R1187
// Added various GUI related uniform variable annotations
// https://reshade.me/forum/releases/2341-3-0
#else
#define __UNIFORM_DRAG_ANY    ui_type = "drag";
#endif

#define __UNIFORM_DRAG_BOOL1  __UNIFORM_DRAG_ANY // It is unsupported on all version
#define __UNIFORM_DRAG_BOOL2  __UNIFORM_DRAG_ANY // It is unsupported on all version
#define __UNIFORM_DRAG_BOOL3  __UNIFORM_DRAG_ANY // It is unsupported on all version
#define __UNIFORM_DRAG_BOOL4  __UNIFORM_DRAG_ANY // It is unsupported on all version
#define __UNIFORM_DRAG_INT1   __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_INT2   __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_INT3   __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_INT4   __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_FLOAT1 __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_FLOAT2 __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_FLOAT3 __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_DRAG_FLOAT4 __UNIFORM_DRAG_ANY // If it was not supported in someday or now, please add information

// Since 4.0.1
// Change slider widget to be used with new "slider" instead of a "drag" type annotation
// https://github.com/crosire/reshade/commit/746229f31cd6f311a3e72a543e4f1f23faa23f11#diff-59405a313bd8cbfb0ca6dd633230e504R1699
// Changed slider widget to be used with < ui_type = "slider"; > instead of < ui_type = "drag"; >
// https://reshade.me/forum/releases/4772-4-0
#if SUPPORTED_VERSION(4,0,1)
#define __UNIFORM_SLIDER_ANY    ui_type = "slider";

// Since 4.0.0
// Rework statistics tab and add drag widgets back
// https://github.com/crosire/reshade/commit/1b2c38795f00efd66c007da1f483f1441b230309
// Changed drag widget to a slider widget (old one is still available via < ui_type = "drag2"; >)
// https://reshade.me/forum/releases/4772-4-0
#elif SUPPORTED_VERSION(4,0,0)
#define __UNIFORM_SLIDER_ANY    ui_type = "drag";
#else
#define __UNIFORM_SLIDER_ANY    __UNIFORM_DRAG_ANY
#endif

#define __UNIFORM_SLIDER_BOOL1  __UNIFORM_SLIDER_ANY // It is unsupported on all version
#define __UNIFORM_SLIDER_BOOL2  __UNIFORM_SLIDER_ANY // It is unsupported on all version
#define __UNIFORM_SLIDER_BOOL3  __UNIFORM_SLIDER_ANY // It is unsupported on all version
#define __UNIFORM_SLIDER_BOOL4  __UNIFORM_SLIDER_ANY // It is unsupported on all version
#define __UNIFORM_SLIDER_INT1   __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_INT2   __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_INT3   __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_INT4   __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_FLOAT1 __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_FLOAT2 __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_FLOAT3 __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_SLIDER_FLOAT4 __UNIFORM_SLIDER_ANY // If it was not supported in someday or now, please add information

// Since 3.0.0
// Add combo box display type for uniform variables and fix displaying of integer variable under Direct3D 9
// https://github.com/crosire/reshade/commit/b025bfae5f7343509ec0cacf6df0cff537c499f2#diff-82cf230afdb2a0d5174111e6f17548a5R1631
// Added various GUI related uniform variable annotations
// https://reshade.me/forum/releases/2341-3-0
#define __UNIFORM_COMBO_ANY    ui_type = "combo";

//      __UNIFORM_COMBO_BOOL1
#define __UNIFORM_COMBO_BOOL2  __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_BOOL3  __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_BOOL4  __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_INT1   __UNIFORM_COMBO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_COMBO_INT2   __UNIFORM_COMBO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_COMBO_INT3   __UNIFORM_COMBO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_COMBO_INT4   __UNIFORM_COMBO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_COMBO_FLOAT1 __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_FLOAT2 __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_FLOAT3 __UNIFORM_COMBO_ANY // It is unsupported on all version
#define __UNIFORM_COMBO_FLOAT4 __UNIFORM_COMBO_ANY // It is unsupported on all version

// Since 4.0.0 (but the ui_items force set "Off\0On\0"), and if less than it force converted to checkbox
// Add option to display boolean values as combo box instead of checkbox
// https://github.com/crosire/reshade/commit/aecb757c864c9679e77edd6f85a1521c49e489c1#diff-59405a313bd8cbfb0ca6dd633230e504R1147
// https://github.com/crosire/reshade/blob/v4.0.0/source/gui.cpp
// Added option to display boolean values as combo box instead of checkbox (via < ui_type = "combo"; >)
// https://reshade.me/forum/releases/4772-4-0
#define __UNIFORM_COMBO_BOOL1  __UNIFORM_COMBO_ANY

// Since 4.0.0
// Cleanup GUI code and rearrange some widgets
// https://github.com/crosire/reshade/commit/6751f7bd50ea7c0556cf0670f10a4b4ba912ee7d#diff-59405a313bd8cbfb0ca6dd633230e504R1711
// Added radio button widget (via < ui_type = "radio"; ui_items = "Button 1\0Button 2\0...\0"; >)
// https://reshade.me/forum/releases/4772-4-0
#if SUPPORTED_VERSION(4,0,0)
#define __UNIFORM_RADIO_ANY    ui_type = "radio";
#else
#define __UNIFORM_RADIO_ANY    __UNIFORM_COMBO_ANY
#endif

#define __UNIFORM_RADIO_BOOL1  __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_BOOL2  __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_BOOL3  __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_BOOL4  __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_INT1   __UNIFORM_RADIO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_RADIO_INT2   __UNIFORM_RADIO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_RADIO_INT3   __UNIFORM_RADIO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_RADIO_INT4   __UNIFORM_RADIO_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_RADIO_FLOAT1 __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_FLOAT2 __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_FLOAT3 __UNIFORM_RADIO_ANY // It is unsupported on all version
#define __UNIFORM_RADIO_FLOAT4 __UNIFORM_RADIO_ANY // It is unsupported on all version

// Since 4.1.0
// Fix floating point uniforms with unknown "ui_type" not showing up in UI
// https://github.com/crosire/reshade/commit/50e5bf44dfc84bc4220c2b9f19d5f50c7a0fda66#diff-59405a313bd8cbfb0ca6dd633230e504R1788
// Fixed floating point uniforms with unknown "ui_type" not showing up in UI
// https://reshade.me/forum/releases/5021-4-1
#define __UNIFORM_COLOR_ANY    ui_type = "color";

// Since 3.0.0
// Move technique list to preset configuration file
// https://github.com/crosire/reshade/blob/84bba3aa934c1ebe4c6419b69dfe1690d9ab9d34/source/runtime.cpp#L1328
// Added various GUI related uniform variable annotations
// https://reshade.me/forum/releases/2341-3-0

// If empty, these versions before 4.1.0 are decide that the type is color from the number of components

#define __UNIFORM_COLOR_BOOL1  __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_BOOL2  __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_BOOL3  __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_BOOL4  __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_INT1   __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_INT2   __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_INT3   __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_INT4   __UNIFORM_COLOR_ANY // It is unsupported on all version
//      __UNIFORM_COLOR_FLOAT1
#define __UNIFORM_COLOR_FLOAT2 __UNIFORM_COLOR_ANY // It is unsupported on all version
#define __UNIFORM_COLOR_FLOAT3 __UNIFORM_COLOR_ANY // If it was not supported in someday or now, please add information
#define __UNIFORM_COLOR_FLOAT4 __UNIFORM_COLOR_ANY // If it was not supported in someday or now, please add information

// Since 4.2.0
// Add alpha slider widget for single component uniform variables (#86)
// https://github.com/crosire/reshade/commit/87a740a8e3c4dcda1dd4eeec8d5cff7fa35fe829#diff-59405a313bd8cbfb0ca6dd633230e504R1820
// Added alpha slider widget for single component uniform variables
// https://reshade.me/forum/releases/5150-4-2
#if SUPPORTED_VERSION(4,2,0)
#define __UNIFORM_COLOR_FLOAT1 __UNIFORM_COLOR_ANY
#else
#define __UNIFORM_COLOR_FLOAT1 __UNIFORM_SLIDER_ANY
#endif

// Since 4.3.0
// Add new "list" GUI widget (#103)
// https://github.com/crosire/reshade/commit/515287d20ce615c19cf3d4c21b49f83896f04ddc#diff-59405a313bd8cbfb0ca6dd633230e504R1894
// Added new "list" GUI widget
// https://reshade.me/forum/releases/5417-4-3
#if SUPPORTED_VERSION(4,3,0)
#define __UNIFORM_LIST_ANY    ui_type = "list";
#else
#define __UNIFORM_LIST_ANY    __UNIFORM_COMBO_ANY
#endif

//      __UNIFORM_LIST_BOOL1
#define __UNIFORM_LIST_BOOL2  __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_BOOL3  __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_BOOL4  __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_INT1   __UNIFORM_LIST_ANY // Supported in 4.3.0
#define __UNIFORM_LIST_INT2   __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_INT3   __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_INT4   __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_FLOAT1 __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_FLOAT2 __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_FLOAT3 __UNIFORM_LIST_ANY // Not supported in all versions
#define __UNIFORM_LIST_FLOAT4 __UNIFORM_LIST_ANY // Not supported in all versions

// For compatible with ComboBox
#define __UNIFORM_LIST_BOOL1  __UNIFORM_COMBO_ANY
