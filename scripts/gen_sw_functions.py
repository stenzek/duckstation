texmode = [
  "GPUTextureMode::Palette4Bit",
  "GPUTextureMode::Palette8Bit",
  "GPUTextureMode::Direct16Bit",
  "GPUTextureMode::Direct16Bit",

  "GPUTextureMode::RawPalette4Bit",
  "GPUTextureMode::RawPalette8Bit",
  "GPUTextureMode::RawDirect16Bit",
  "GPUTextureMode::RawDirect16Bit",

  "GPUTextureMode::Disabled"
]

transparentmode = [
  "GPUTransparencyMode::HalfBackgroundPlusHalfForeground",
  "GPUTransparencyMode::BackgroundPlusForeground",
  "GPUTransparencyMode::BackgroundMinusForeground",
  "GPUTransparencyMode::BackgroundPlusQuarterForeground",
  "GPUTransparencyMode::Disabled"
]

bools = ["false", "true"]


"""
print("const GPU_SW_Backend::DrawRectangleFunction GPU_SW_Backend::s_rectangle_draw_functions[%d][%d][2] = {" % (len(texmode), len(transparentmode)))
for texture in range(len(texmode)):
  print("  { // %s" % texmode[texture])
  for transparency in range(len(transparentmode)):
    print("    { // %s" % transparentmode[transparency])
    for check_mask in range(2):
      line = "&GPU_SW_Backend::DrawRectangle<%s, %s, %s>" % (texmode[texture], transparentmode[transparency], bools[check_mask])
      print("      %s%s" % (line, "," if check_mask == 0 else ""))
    print("    }%s" % ("," if transparency < (len(transparentmode) - 1) else ""))
  print("  }%s" % ("," if texture < (len(texmode) - 1) else ""))
print("};")
"""

"""
print("const GPU_SW_Backend::DrawTriangleFunction GPU_SW_Backend::s_triangle_draw_functions[2][%d][%d][2][2] = {" % (len(texmode), len(transparentmode)))
for shading in range(2):
  print("  { // shading %s" % bools[shading])
  for texture in range(len(texmode)):
    print("    { // %s" % texmode[texture])
    for transparency in range(len(transparentmode)):
      print("      { // %s" % transparentmode[transparency])
      for dither in range(2):
        print("        { // dither %s" % bools[dither])
        for check_mask in range(2):
          line = "&GPU_SW_Backend::DrawTriangle<%s, %s, %s, %s, %s>" % (bools[shading], texmode[texture], transparentmode[transparency], bools[dither], bools[check_mask])
          print("          %s%s" % (line, "," if check_mask == 0 else ""))
        print("        }%s" % ("," if dither == 0 else ""))
      print("      }%s" % ("," if transparency < (len(transparentmode) - 1) else ""))
    print("    }%s" % ("," if texture < (len(texmode) - 1) else ""))
  print("  }%s" % ("," if shading == 0 else ""))
print("};")
"""

print("const GPU_SW_Backend::DrawLineFunction GPU_SW_Backend::s_line_draw_functions[2][%d][2][2] = {" % (len(transparentmode)))
for shading in range(2):
  print("  { // shading %s" % bools[shading])
  for transparency in range(len(transparentmode)):
    print("      { // %s" % transparentmode[transparency])
    for dither in range(2):
      print("        { // dither %s" % bools[dither])
      for check_mask in range(2):
        line = "&GPU_SW_Backend::DrawLine<%s, %s, %s, %s>" % (bools[shading], transparentmode[transparency], bools[dither], bools[check_mask])
        print("          %s%s" % (line, "," if check_mask == 0 else ""))
      print("        }%s" % ("," if dither == 0 else ""))
    print("      }%s" % ("," if transparency < (len(transparentmode) - 1) else ""))
  print("  }%s" % ("," if shading == 0 else ""))
print("};")
