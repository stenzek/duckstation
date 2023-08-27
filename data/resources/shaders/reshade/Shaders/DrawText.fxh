#ifndef _DRAWTEXT_H_
#define _DRAWTEXT_H_

#define _DRAWTEXT_GRID_X 14.0
#define _DRAWTEXT_GRID_Y 7.0


///////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                                   //
//  DrawText.fxh by kingreic1992   ( update: Sep.28.2019 )                                           //
//                                                                                                   //
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//                                                                                                   //
//  Available functions:                                                                             //
//      DrawText_String( offset, text size, xy ratio, input coord, string array, array size, output) //
//          float2 offset       = top left corner of string, screen hight pixel unit.                //
//          float  text size    = text size, screen hight pixel unit.                                //
//          float  xy ratio     = xy ratio of text.                                                  //
//          float2 input coord  = current texture coord.                                             //
//          int    string array = string data in float2 array format, ex: "Demo Text"                //
//              int String0[9] = { __D, __e, __m, __o, __Space, __T, __e, __x, __t};                 //
//          int    string size  = size of the string array.                                          //
//          float  output       = output.                                                            //
//                                                                                                   //
//      DrawText_Digit( offset, text size, xy ratio, input coord, precision after dot, data, output) //
//          float2 offset       = same as DrawText_String.                                           //
//          float  text size    = same as DrawText_String.                                           //
//          float  xy ratio     = same as DrawText_String.                                           //
//          float2 input coord  = same as DrawText_String.                                           //
//          int    precision    = digits after dot.                                                  //
//          float  data         = input float.                                                       //
//          float  output       = output.                                                            //
//                                                                                                   //
//      float2 DrawText_Shift(offset, shift, text size, xy ratio)                                    //
//          float2 offset       = same as DrawText_String.                                           //
//          float2 shift        = shift line(y) and column.                                          //
//          float text size     = same as DrawText_String.                                           //
//          float xy ratio      = same as DrawText_String.                                           //
//                                                                                                   //
///////////////////////////////////////////////////////////////////////////////////////////////////////


//Sample Usage

/*

#include "DrawText.fxh"

float4 main_fragment( float4 position : POSITION,
                      float2 txcoord  : TEXCOORD) : COLOR {
    float res = 0.0;

    int line0[9]  = { __D, __e, __m, __o, __Space, __T, __e, __x, __t };   //Demo Text
    int line1[15] = { __b, __y, __Space, __k, __i, __n, __g, __e, __r, __i, __c, __1, __9, __9, __2 }; //by kingeric1992
    int line2[6]  = { __S, __i, __z, __e, __Colon, __Space }; // Size: %d.

    DrawText_String(float2(100.0 , 100.0), 32, 1, txcoord,  line0, 9, res);
    DrawText_String(float2(100.0 , 134.0), textSize, 1, txcoord,  line1, 15, res);
    DrawText_String(DrawText_Shift(float2(100.0 , 134.0), int2(0, 1), textSize, 1), 18, 1, txcoord,  line2, 6, res);
    DrawText_Digit(DrawText_Shift(DrawText_Shift(float2(100.0 , 134.0), int2(0, 1), textSize, 1), int2(8, 0), 18, 1),
                    18, 1, txcoord,  0, textSize, res);
    return res;
}
*/

//Text display
//Character indexing
#define __Space       0 //  (space)
#define __Exclam      1 //  !
#define __Quote       2 //  "
#define __Pound       3 //  #
#define __Dollar      4 //  $
#define __Percent     5 //  %
#define __And         6 //  &
#define __sQuote      7 //  '
#define __rBrac_O     8 //  (
#define __rBrac_C     9 //  )
#define __Asterisk   10 //  *
#define __Plus       11 //  +
#define __Comma      12 //  ,
#define __Minus      13 //  -

#define __Dot        14 //  .
#define __Slash      15 //  /
#define __0          16 //  0
#define __1          17 //  1
#define __2          18 //  2
#define __3          19 //  3
#define __4          20 //  4
#define __5          21 //  5
#define __6          22 //  6
#define __7          23 //  7
#define __8          24 //  8
#define __9          25 //  9
#define __Colon      26 //  :
#define __sColon     27 //  ;

#define __Less       28 //  <
#define __Equals     29 //  =
#define __Greater    30 //  >
#define __Question   31 //  ?
#define __at         32 //  @
#define __A          33 //  A
#define __B          34 //  B
#define __C          35 //  C
#define __D          36 //  D
#define __E          37 //  E
#define __F          38 //  F
#define __G          39 //  G
#define __H          40 //  H
#define __I          41 //  I

#define __J          42 //  J
#define __K          43 //  K
#define __L          44 //  L
#define __M          45 //  M
#define __N          46 //  N
#define __O          47 //  O
#define __P          48 //  P
#define __Q          49 //  Q
#define __R          50 //  R
#define __S          51 //  S
#define __T          52 //  T
#define __U          53 //  U
#define __V          54 //  V
#define __W          55 //  W

#define __X          56 //  X
#define __Y          57 //  Y
#define __Z          58 //  Z
#define __sBrac_O    59 //  [
#define __Backslash  60 //  \..
#define __sBrac_C    61 //  ]
#define __Caret      62 //  ^
#define __Underscore 63 //  _
#define __Punc       64 //  `
#define __a          65 //  a
#define __b          66 //  b
#define __c          67 //  c
#define __d          68 //  d
#define __e          69 //  e

#define __f          70 //  f
#define __g          71 //  g
#define __h          72 //  h
#define __i          73 //  i
#define __j          74 //  j
#define __k          75 //  k
#define __l          76 //  l
#define __m          77 //  m
#define __n          78 //  n
#define __o          79 //  o
#define __p          80 //  p
#define __q          81 //  q
#define __r          82 //  r
#define __s          83 //  s

#define __t          84 //  t
#define __u          85 //  u
#define __v          86 //  v
#define __w          87 //  w
#define __x          88 //  x
#define __y          89 //  y
#define __z          90 //  z
#define __cBrac_O    91 //  {
#define __vBar       92 //  |
#define __cBrac_C    93 //  }
#define __Tilde      94 //  ~
#define __tridot     95 // (...)
#define __empty0     96 // (null)
#define __empty1     97 // (null)
//Character indexing ends

texture Texttex < source = "FontAtlas.png"; > {
    Width  = 512;
    Height = 512;
};

sampler samplerText {
    Texture = Texttex;
};

//accomodate for undef array size.
#define DrawText_String(  pos, size, ratio, tex, array, arrSize, output ) \
    {   float  text = 0.0; \
        float2 uv = (tex * float2(BUFFER_WIDTH, BUFFER_HEIGHT) - pos) / size; \
        uv.y      = saturate(uv.y); \
        uv.x     *= ratio * 2.0; \
        float  id = array[int(trunc(uv.x))]; \
        if(uv.x  <= arrSize && uv.x >= 0.0) \
            text  = tex2D(samplerText, (frac(uv) + float2( id % 14.0, trunc(id / 14.0))) \
            / float2( _DRAWTEXT_GRID_X, _DRAWTEXT_GRID_Y) ).x; \
        output += text;  }

float2 DrawText_Shift( float2 pos, int2 shift, float size, float ratio ) {
    return pos + size * shift * float2(0.5, 1.0) / ratio;
}

void DrawText_Digit( float2 pos, float size, float ratio, float2 tex, int digit, float data, inout float res) {
    int digits[13] = {
        __0, __1, __2, __3, __4, __5, __6, __7, __8, __9, __Minus, __Space, __Dot
    };

    float2 uv = (tex * float2(BUFFER_WIDTH, BUFFER_HEIGHT) - pos) / size;
    uv.y      = saturate(uv.y);
    uv.x     *= ratio * 2.0;

    float  t  = abs(data);
    int radix = floor(t)? ceil(log2(t)/3.32192809):0;

    //early exit:
    if(uv.x > digit+1 || -uv.x > radix+1) return;

    float index = t;
    if(floor(uv.x) > 0)
        for(int i = ceil(-uv.x); i<0; i++) index *= 10.;
    else
        for(int i = ceil(uv.x); i<0; i++) index /= 10.;

    index = (uv.x >= -radix-!radix)? index%10 : (10+step(0, data)); //adding sign
    index = (uv.x > 0 && uv.x < 1)? 12:index; //adding dot
    index = digits[(uint)index];

    res  += tex2D(samplerText, (frac(uv) + float2( index % 14.0, trunc(index / 14.0))) /
                float2( _DRAWTEXT_GRID_X, _DRAWTEXT_GRID_Y)).x;
}

#endif