bools = ["false", "true"]


print("static constexpr DrawRectangleFunction funcs[2][2][2] = {")
for texture in range(2):
  print("  {")
  for raw_texture in range(2):
    print("    {")
    for transparency in range(2):
      line = "&GPU_SW_Backend::DrawRectangle<%s, %s, %s>" % (bools[texture], bools[0 if texture == 0 else raw_texture], bools[transparency])
      print("      %s%s" % (line, "," if transparency == 0 else ""))
    print("    }%s" % ("," if raw_texture == 0 else ""))
  print("  }%s" % ("," if texture == 0 else ""))
print("};")


print("static constexpr DrawTriangleFunction funcs[2][2][2][2][2] = {")
for shading in range(2):
  print("  {")
  for texture in range(2):
    print("    {")
    for raw_texture in range(2):
      print("    {")
      for transparency in range(2):
        print("      {")
        for dither in range(2):
          line = "&GPU_SW_Backend::DrawTriangle<%s, %s, %s, %s, %s>" % (bools[shading], bools[texture], bools[0 if texture == 0 else raw_texture], bools[transparency], bools[0 if raw_texture != 0 else dither])
          print("          %s%s" % (line, "," if dither == 0 else ""))
        print("        }%s" % ("," if transparency == 0 else ""))
      print("      }%s" % ("," if raw_texture == 0 else ""))
    print("    }%s" % ("," if texture == 0 else ""))
  print("  }%s" % ("," if shading == 0 else ""))
print("};")


print("static constexpr DrawLineFunction funcs[2][2][2] = {")
for shading in range(2):
  print("  {")
  for transparency in range(2):
    print("      {")
    for dither in range(2):
      line = "&GPU_SW_Backend::DrawLine<%s, %s, %s>" % (bools[shading], bools[transparency], bools[dither])
      print("          %s%s" % (line, "," if dither == 0 else ""))
    print("      }%s" % ("," if transparency == 0 else ""))
  print("  }%s" % ("," if shading == 0 else ""))
print("};")
