// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <ft2build.h>

#include FT_FREETYPE_H         // <freetype/freetype.h>
#include FT_GLYPH_H            // <freetype/ftglyph.h>
#include FT_MULTIPLE_MASTERS_H // <freetype/ftmm.h>
#include FT_MODULE_H           // <freetype/ftmodapi.h>
#include FT_SIZES_H            // <freetype/ftsizes.h>
#include FT_SYNTHESIS_H        // <freetype/ftsynth.h>

class Error;

#define DYN_FREETYPE_FUNCTIONS(X)                                                                                      \
  X(FT_New_Memory_Face)                                                                                                \
  X(FT_Select_Charmap)                                                                                                 \
  X(FT_Get_MM_Var)                                                                                                     \
  X(FT_Done_MM_Var)                                                                                                    \
  X(FT_Done_Face)                                                                                                      \
  X(FT_Get_Char_Index)                                                                                                 \
  X(FT_Load_Glyph)                                                                                                     \
  X(FT_GlyphSlot_Embolden)                                                                                             \
  X(FT_GlyphSlot_Oblique)                                                                                              \
  X(FT_New_Library)                                                                                                    \
  X(FT_Add_Default_Modules)                                                                                            \
  X(FT_Property_Set)                                                                                                   \
  X(FT_Done_Library)                                                                                                   \
  X(FT_New_Size)                                                                                                       \
  X(FT_Activate_Size)                                                                                                  \
  X(FT_Set_Var_Design_Coordinates)                                                                                     \
  X(FT_Request_Size)                                                                                                   \
  X(FT_Done_Size)                                                                                                      \
  X(FT_Render_Glyph)                                                                                                   \
  X(FT_Library_Version)

struct DynFreeType
{
#define ADD_FUNC(F) decltype(&::F) F;
  DYN_FREETYPE_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

  bool Open(Error* const error);
};

extern DynFreeType g_dyn_freetype;
