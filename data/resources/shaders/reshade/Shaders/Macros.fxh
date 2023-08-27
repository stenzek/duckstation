////////////////////////////////////////////////////////////
// BASIC MACROS FOR RESHADE 4                             //
// AUTHOR: TREYM                                          //
////////////////////////////////////////////////////////////
// Modified by dddfault                                   //
//                                                        //
// Changelogs :                                           //
//   Added Sampler texture boundary resolver option       //
//   Added float2 parameters option                       //
////////////////////////////////////////////////////////////
// Macros Guide:                                          //
////////////////////////////////////////////////////////////

/*  ////////////////////////////////////////////////////  *
 *  ////////////////////////////////////////////////////  *

  Usage of these macros is very simple once you understand
 the syntax and variable names. Let's start with a Simple
 integer slider. To begin, type:

    UI_INT

  Next we need to add _S to indicate that this is a
 "slider" widget. Follow the syntax below:

    UI_INT_S(INT_NAME, "Label", "Tooltip", 0, 100, 50)

  Using just a single line of code, we have created a UI
 tweakable integer named INT_NAME with a minimum value of
 0, a maximum value of 100, and a default value of 50.

  Next, let's create that same widget, but within a UI
 category. This time, we'll type:

    CAT_INT_S(INT_NAME, "Category", "Label", "Tooltip", 0, 100, 50)

  As you can see, the syntax follows the same pattern but
 with a new input for "Category"

  Below you will find a useful list of examples to get you
 started. I hope you find these useful and they help your
 workflow. Happy coding!

 - TreyM

 *  ////////////////////////////////////////////////////  *
 *  ////////////////////////////////////////////////////  *

    Widget Types
        Input   = _I
        Slider  = _S
        Drag    = _D

 *  ////////////////////////////////////////////////////  *

    BOOLEAN Macro
        UI_BOOL(BOOL_NAME, "Label", "Tooltip", true)

    BOOLEAN Categorized Macro
        CAT_BOOL(BOOL_NAME, "Category", "Label", "Tooltip", true)

 *  ////////////////////////////////////////////////////  *

    INTEGER Combo Widget
        UI_COMBO(INT_NAME, "Label", "Tooltip", 0, 2, 0, "Item 1\0Item 2\0Item 3\0")

    INTEGER Drag Widget
        UI_INT_D(INT_NAME, "Label", "Tooltip", 0, 100, 50)

    INTEGER Input Widget
        UI_INT_I(INT_NAME, "Label", "Tooltip", 0, 100, 50)

    INTEGER Radio Widget
        UI_RADIO(INT_NAME, "Label", "Tooltip", 0, 2, 0, " Item 1 \0 Item 2 \0 Item 3\0")

    INTEGER Slider Widget
        UI_INT_S(INT_NAME, "Label", "Tooltip", 0, 100, 50)

    INTEGER Categorized Combo Widget
        CAT_COMBO(INT_NAME, "Category", "Label", "Tooltip", 0, 2, 0, " Item 1 \0 Item 2 \0 Item 3\0")

    INTEGER Categorized Drag Widget
        CAT_INT_D(INT_NAME, "Category", "Label", "Tooltip", 0, 100, 50)

    INTEGER Categorized Input Widget
        CAT_INT_I(INT_NAME, "Category", "Label", "Tooltip", 0, 100, 50)

    INTEGER Categorized Radio Widget
        CAT_RADIO(INT_NAME, "Category", "Label", "Tooltip", 0, 2, 0, " Item 1 \0 Item 2 \0 Item 3\0")

    INTEGER Categorized Slider Widget
        CAT_INT_S(INT_NAME, "Category", "Label", "Tooltip", 0, 100, 50)

 *  ////////////////////////////////////////////////////  *

    FLOAT Drag Widget
        UI_FLOAT_D(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT Input Widget
        UI_FLOAT_I(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT Slider Widget
        UI_FLOAT_S(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT Categorized Drag Widget
        CAT_FLOAT_D(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT Categorized Input Widget
        CAT_FLOAT_I(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT Categorized Slider Widget
        CAT_FLOAT_S(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5)

    FLOAT macro with full control (value after "Tooltip" is ui_step)
        UI_FLOAT_FULL(FLOAT_NAME, "ui_type", "Label", "Tooltip", 0.1, 0.0, 1.0, 0.5)

    FLOAT Categorized macro with full control (value after "Tooltip" is ui_step)
        CAT_FLOAT_FULL(FLOAT_NAME, "ui_type", "Category", "Label", "Tooltip", 0.1, 0.0, 1.0, 0.5)

  *  ////////////////////////////////////////////////////  *

     FLOAT2 Drag Widget
         UI_FLOAT2_D(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Input Widget
         UI_FLOAT2_I(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Slider Widget
         UI_FLOAT2_S(FLOAT_NAME, "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Categorized Drag Widget
         CAT_FLOAT2_D(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Categorized Input Widget
         CAT_FLOAT2_I(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Categorized Slider Widget
         CAT_FLOAT2_S(FLOAT_NAME, "Category", "Label", "Tooltip", 0.0, 1.0, 0.5, 0.5)

     FLOAT2 macro with full control (value after "Tooltip" is ui_step)
         UI_FLOAT2_FULL(FLOAT_NAME, "ui_type", "Label", "Tooltip", 0.1, 0.0, 1.0, 0.5, 0.5)

     FLOAT2 Categorized macro with full control (value after "Tooltip" is ui_step)
         CAT_FLOAT2_FULL(FLOAT_NAME, "ui_type", "Category", "Label", "Tooltip", 0.1, 0.0, 1.0, 0.5, 0.5)

 *  ////////////////////////////////////////////////////  *

    FLOAT3 Drag Widget
        UI_FLOAT3_D(FLOAT_NAME, "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Input Widget
        UI_FLOAT3_I(FLOAT_NAME, "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Slider Widget
        UI_FLOAT3_S(FLOAT_NAME, "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Categorized Drag Widget
        CAT_FLOAT3_D(FLOAT_NAME, "Category", "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Categorized Input Widget
        CAT_FLOAT3_I(FLOAT_NAME, "Category", "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Categorized Slider Widget
        CAT_FLOAT3_S(FLOAT_NAME, "Category", "Label", "Tooltip", 0.5, 0.5, 0.5)

 *  ////////////////////////////////////////////////////  *

    FLOAT3 Color Widget
        UI_COLOR(FLOAT_NAME, "Label", "Tooltip", 0.5, 0.5, 0.5)

    FLOAT3 Categorized Color Widget
        CAT_COLOR(FLOAT_NAME, "Category", "Label", "Tooltip", 0.5, 0.5, 0.5)

 *  ////////////////////////////////////////////////////  *

    SAMPLER Macro
        SAMPLER(SamplerName, TextureName)

    SAMPLER Macro with texture boundary resolver option
        SAMPLER_UV(SamplerName, TextureName, ResolverType)

    TEXTURE Macro
        TEXTURE(TextureName, "TexturePath")

    TEXTURE Full Macro
        TEXTURE_FULL(TextureName, "TexturePath", Width, Height, Format)

 *  ////////////////////////////////////////////////////  *

    TECHNIQUE Macro
        TECHNIQUE(TechniqueName, PassMacro)

    PASS Macro
        PASS(PassID, VertexShader, PixelShader)

    PASS Macro with RenderTarget
        PASS_RT(PassID, VertexShader, PixelShader, RenderTarget)

    ////////////////////////////////////////////////////
 *  ////////////////////////////////////////////////////  */

// INTEGER MACROS ////////////////////////////////
    #define UI_COMBO(var, label, tooltip, minval, maxval, defval, items) \
        uniform int var \
        < \
            ui_type     = "combo"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_items    = items; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_COMBO(var, category, label, tooltip, minval, maxval, defval, items) \
        uniform int var \
        < \
            ui_type     = "combo"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_items    = items; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_INT_I(var, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "input"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_INT_I(var, category, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "input"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_INT_S(var, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "slider"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_INT_S(var, category, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "slider"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_INT_D(var, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "drag"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_INT_D(var, category, label, tooltip, minval, maxval, defval) \
        uniform int var \
        < \
            ui_type     = "drag"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

        #define UI_RADIO(var, label, tooltip, minval, maxval, defval, items) \
            uniform int var \
            < \
                ui_type     = "radio"; \
                ui_label    = label; \
                ui_tooltip  = tooltip; \
                ui_items    = items; \
                ui_min      = minval; \
                ui_max      = maxval; \
            >               = defval;

        #define CAT_RADIO(var, category, label, tooltip, minval, maxval, defval, items) \
            uniform int var \
            < \
                ui_type     = "radio"; \
                ui_category = category; \
                ui_label    = label; \
                ui_tooltip  = tooltip; \
                ui_items    = items; \
                ui_min      = minval; \
                ui_max      = maxval; \
            >               = defval;

// BOOL MACROS ///////////////////////////////////
    #define UI_BOOL(var, label, tooltip, def) \
        uniform bool var \
        < \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = def;

    #define CAT_BOOL(var, category, label, tooltip, def) \
        uniform bool var \
        < \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = def;

// FLOAT MACROS //////////////////////////////////
    #define UI_FLOAT_D(var, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "drag"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_FLOAT_D(var, category, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "drag"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_FLOAT_FULL(var, uitype, label, tooltip, uistep, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = uitype; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_step     = uistep; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_FLOAT_FULL(var, uitype, category, label, tooltip, uistep, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = uitype; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_step     = uistep; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_FLOAT_I(var, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "input"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_FLOAT_I(var, category, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "input"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_FLOAT_S(var, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "slider"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define CAT_FLOAT_S(var, category, label, tooltip, minval, maxval, defval) \
        uniform float var \
        < \
            ui_type     = "slider"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = defval;

    #define UI_FLOAT2_D(var, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "drag"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define CAT_FLOAT2_D(var, category, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "drag"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define UI_FLOAT2_FULL(var, uitype, label, tooltip, uistep, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = uitype; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_step     = uistep; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define CAT_FLOAT2_FULL(var, uitype, category, label, tooltip, uistep, minval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = uitype; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_step     = uistep; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define UI_FLOAT2_I(var, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "input"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define CAT_FLOAT2_I(var, category, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "input"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define UI_FLOAT2_S(var, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "slider"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define CAT_FLOAT2_S(var, category, label, tooltip, minval, maxval, defval1, defval2) \
        uniform float2 var \
        < \
            ui_type     = "slider"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
            ui_min      = minval; \
            ui_max      = maxval; \
        >               = float2(defval1, defval2);

    #define UI_FLOAT3_D(var, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "drag"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define CAT_FLOAT3_D(var, category, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "drag"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define UI_FLOAT3_I(var, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "input"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define CAT_FLOAT3_I(var, category, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "input"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define UI_FLOAT3_S(var, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "slider"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define CAT_FLOAT3_S(var, category, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "slider"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);


// COLOR WIDGET MACROS ///////////////////////////
    #define UI_COLOR(var, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "color"; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);

    #define CAT_COLOR(var, category, label, tooltip, defval1, defval2, defval3) \
        uniform float3 var \
        < \
            ui_type     = "color"; \
            ui_category = category; \
            ui_label    = label; \
            ui_tooltip  = tooltip; \
        >               = float3(defval1, defval2, defval3);


// SAMPLER MACRO /////////////////////////////////
    #define SAMPLER(sname, tname) \
        sampler	sname \
        { \
            Texture     = tname; \
        };

    #define SAMPLER_UV(sname, tname, addUVW) \
        sampler	sname \
        { \
            Texture  = tname; \
            AddressU = addUVW; \
            AddressV = addUVW; \
            AddressW = addUVW; \
        };


// TEXTURE MACROs ////////////////////////////////
    #define TEXTURE(tname, src) \
        texture tname <source=src;> \
        { \
            Width       = BUFFER_WIDTH; \
            Height      = BUFFER_HEIGHT; \
            Format      = RGBA8; \
        };

    #define TEXTURE_FULL(tname, src, width, height, fomat) \
        texture tname <source=src;> \
        { \
            Width       = width; \
            Height      = height; \
            Format      = fomat; \
        };


// TECHNIQUE MACROS //////////////////////////////
    #define TECHNIQUE(tname, pass) \
        technique tname \
        { \
            pass \
        }

    #define PASS(ID, vs, ps) pass \
        { \
            VertexShader  = vs; \
            PixelShader   = ps; \
        }

    #define PASS_RT(ID, vs, ps, rt) pass \
        { \
            VertexShader  = vs; \
            PixelShader   = ps; \
            RenderTarget  = rt; \
        }
