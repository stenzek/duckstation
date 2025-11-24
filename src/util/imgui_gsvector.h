// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/gsvector.h"

#include "imgui.h"
#include "imgui_internal.h"

inline ImVec2 GSVectorToImVec2(const GSVector2& vec)
{
  alignas(VECTOR_ALIGNMENT) ImVec2 ret;
  GSVector2::store<true>(&ret.x, vec);
  return ret;
}

inline GSVector2 ImVec2ToGSVector(const ImVec2& vec)
{
  return GSVector2::load<false>(&vec.x);
}

inline ImRect GSVectorToImRect(const GSVector4& vec)
{
  // GSVector4 maps directly to ImRect's memory layout.
  static_assert(sizeof(ImRect) == sizeof(GSVector4));
  alignas(VECTOR_ALIGNMENT) ImRect ret;
  GSVector4::store<true>(&ret.Min.x, vec);
  return ret;
}

inline GSVector4 ImRectToGSVector(const ImRect& rect)
{
  return GSVector4::load<false>(&rect.Min.x);
}
