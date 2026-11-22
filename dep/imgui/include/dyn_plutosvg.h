// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <plutosvg.h>

class Error;

#define DYN_PLUTOSVG_FUNCTIONS(X)                                                                                      \
  X(plutosvg_ft_svg_hooks)                                                                                             \
  X(plutosvg_document_load_from_data)                                                                                  \
  X(plutosvg_document_destroy)                                                                                         \
  X(plutosvg_document_get_width)                                                                                       \
  X(plutosvg_document_get_height)                                                                                      \
  X(plutosvg_document_render_to_surface)                                                                               \
  X(plutosvg_document_render)                                                                                          \
  X(plutovg_surface_create)                                                                                            \
  X(plutovg_surface_destroy)                                                                                           \
  X(plutovg_surface_get_width)                                                                                         \
  X(plutovg_surface_get_height)                                                                                        \
  X(plutovg_surface_get_data)                                                                                          \
  X(plutovg_surface_get_stride)                                                                                        \
  X(plutovg_canvas_create)                                                                                             \
  X(plutovg_canvas_destroy)                                                                                            \
  X(plutovg_canvas_scale)

struct DynPlutoSvg
{
#define ADD_FUNC(F) decltype(&::F) F;
  DYN_PLUTOSVG_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

  bool Open(Error* const error = nullptr);
};

extern DynPlutoSvg g_dyn_plutosvg;
