// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Compute DuckStation texture replacement filenames for a TIM image.
// Standalone C++ version of tim_texture_hash.py with identical output, see that script for details and caveats.
//
// Build from the repository root:
//   clang++ -std=c++20 -O2 -Idep/xxhash/include -o tim_texture_hash scripts/tim_texture_hash.cpp

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

static constexpr uint32_t VRAM_WIDTH = 1024;
static constexpr uint32_t VRAM_HEIGHT = 512;
static constexpr const char* MODE_NAMES[] = {"P4", "P8", "C16"};
static constexpr uint32_t MODE_SHIFTS[] = {2, 1, 0};
static constexpr uint32_t PALETTE_WIDTHS[] = {16, 256, 0};

struct Section
{
  uint32_t x, y, w, h;
  const uint8_t* data;
};

static std::string s_error;

static void Warn(const char* fmt, ...)
{
  std::va_list ap;
  va_start(ap, fmt);
  std::fputs("warning: ", stderr);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}

static bool Fail(const char* fmt, ...)
{
  char buf[256];
  std::va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s_error = buf;
  return false;
}

static uint16_t ReadU16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t ReadU32(const uint8_t* p)
{
  return static_cast<uint32_t>(ReadU16(p)) | (static_cast<uint32_t>(ReadU16(p + 2)) << 16);
}

static bool ReadSection(const uint8_t* data, size_t len, size_t pos, const char* what, Section* sec)
{
  if (pos + 12 > len)
    return Fail("%s section header truncated at offset 0x%zX", what, pos);

  const uint32_t size = ReadU32(data + pos);
  sec->x = ReadU16(data + pos + 4);
  sec->y = ReadU16(data + pos + 6);
  sec->w = ReadU16(data + pos + 8);
  sec->h = ReadU16(data + pos + 10);
  sec->data = data + pos + 12;

  const size_t expected_size = 12 + static_cast<size_t>(sec->w) * sec->h * 2;
  if (pos + expected_size > len)
  {
    return Fail("%s data truncated (%ux%u needs %zu bytes, %zu available)", what, sec->w, sec->h, expected_size - 12,
                len - pos - 12);
  }
  if (size != expected_size)
    Warn("%s section size is %u, expected %zu", what, size, expected_size);

  return true;
}

static bool GetNames(const uint8_t* data, size_t len, std::optional<uint32_t> mode_override, bool verbose,
                     std::vector<std::string>& names)
{
  char buf[256];

  if (len < 8)
    return Fail("file too small for a TIM header");

  const uint8_t tim_id = data[0];
  const uint8_t version = data[1];
  const uint16_t reserved = ReadU16(data + 2);
  const uint32_t flags = ReadU32(data + 4);
  if (tim_id != 0x10 || version != 0)
    return Fail("not a TIM (ID %02Xh, version %02Xh)", tim_id, version);
  if (reserved != 0)
    return Fail("compressed TIM (reserved field %u) is not supported", reserved);
  if ((flags & ~0xFu) != 0)
    Warn("reserved flag bits are set (%08Xh)", flags);

  const uint32_t tim_type = flags & 7;
  const bool has_clut = (flags & 8) != 0;
  size_t pos = 8;

  Section clut = {};
  if (has_clut)
  {
    if (!ReadSection(data, len, pos, "CLUT", &clut))
      return false;
    pos += 12 + static_cast<size_t>(clut.w) * clut.h * 2;
  }

  Section pix;
  if (!ReadSection(data, len, pos, "pixel", &pix))
    return false;

  if (verbose)
  {
    std::fprintf(stderr, "type %u, pixels %ux%u halfwords at (%u,%u)\n", tim_type, pix.w, pix.h, pix.x, pix.y);
    if (has_clut)
      std::fprintf(stderr, "CLUT %ux%u at (%u,%u)\n", clut.w, clut.h, clut.x, clut.y);
  }

  if (pix.w == 0 || pix.h == 0)
  {
    Warn("TIM has no pixel data");
    return true;
  }

  if ((pix.x % VRAM_WIDTH) + pix.w > VRAM_WIDTH || (pix.y % VRAM_HEIGHT) + pix.h > VRAM_HEIGHT)
    Warn("pixel rect (%u,%u %ux%u) wraps around VRAM, names will not match", pix.x, pix.y, pix.w, pix.h);

  const size_t pix_size = static_cast<size_t>(pix.w) * pix.h * 2;
  const XXH128_hash_t h128 = XXH3_128bits(pix.data, pix_size);
  std::snprintf(buf, sizeof(buf), "vram-write-%016" PRIX64 "%016" PRIX64, static_cast<uint64_t>(h128.high64),
                static_cast<uint64_t>(h128.low64));
  names.emplace_back(buf);

  uint32_t mode;
  if (mode_override.has_value())
  {
    mode = mode_override.value();
  }
  else if (tim_type < 3)
  {
    mode = tim_type;
  }
  else
  {
    Warn("TIM type %u has no texture mode, use --mode to generate texupload names", tim_type);
    return true;
  }

  const uint64_t src_hash = XXH3_64bits(pix.data, pix_size);
  char prefix[64], suffix[64];
  std::snprintf(prefix, sizeof(prefix), "texupload-%s-%016" PRIX64, MODE_NAMES[mode], static_cast<uint64_t>(src_hash));
  std::snprintf(suffix, sizeof(suffix), "%ux%u-0-0-%ux%u", pix.w, pix.h, pix.w << MODE_SHIFTS[mode], pix.h);

  if (mode == 2)
  {
    std::snprintf(buf, sizeof(buf), "%s-%s", prefix, suffix);
    names.emplace_back(buf);
    return true;
  }

  if (!has_clut)
  {
    Warn("TIM has no CLUT, cannot generate texupload names for a palette texture");
    return true;
  }

  if (clut.x % 16 != 0)
    Warn("CLUT X %u is not a multiple of 16, the GPU cannot use palettes at this position", clut.x);

  const uint32_t pal_width = PALETTE_WIDTHS[mode];
  for (uint32_t row = 0; row < clut.h; row++)
  {
    for (uint32_t start = 0; start < clut.w; start += pal_width)
    {
      const uint32_t count = std::min(pal_width, clut.w - start);
      const uint32_t vram_x = (clut.x + start) % VRAM_WIDTH;
      uint32_t hash_count;
      if (count == pal_width)
      {
        // HashPalette() only hashes up to the right edge of VRAM.
        hash_count = std::min(count, VRAM_WIDTH - vram_x);
      }
      else if (vram_x + count > VRAM_WIDTH)
      {
        Warn("partial palette at (%u,%u) crosses the edge of VRAM, skipping", vram_x, clut.y + row);
        continue;
      }
      else
      {
        hash_count = count;
      }

      const size_t offset = (static_cast<size_t>(row) * clut.w + start) * 2;
      const uint64_t pal_hash = XXH3_64bits(clut.data + offset, hash_count * 2);
      std::snprintf(buf, sizeof(buf), "%s-%016" PRIX64 "-%s-P0-%u", prefix, static_cast<uint64_t>(pal_hash), suffix,
                    count - 1);
      if (std::find(names.begin(), names.end(), buf) == names.end())
        names.emplace_back(buf);
    }
  }

  return true;
}

static void Usage(const char* argv0)
{
  std::fprintf(stderr,
               "usage: %s [-h] [--offset OFFSET] [--mode {P4,P8,C16}] [-v] files [files ...]\n\n"
               "Compute DuckStation texture replacement filenames for TIM images.\n"
               "See scripts/tim_texture_hash.py for details and caveats.\n",
               argv0);
}

} // namespace

int main(int argc, char* argv[])
{
  std::vector<const char*> files;
  size_t offset = 0;
  std::optional<uint32_t> mode;
  bool verbose = false;

  for (int i = 1; i < argc; i++)
  {
    const char* arg = argv[i];
    const char* value = nullptr;
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0)
    {
      Usage(argv[0]);
      return 0;
    }
    else if (std::strcmp(arg, "-v") == 0 || std::strcmp(arg, "--verbose") == 0)
    {
      verbose = true;
    }
    else if (std::strcmp(arg, "--offset") == 0 || std::strncmp(arg, "--offset=", 9) == 0)
    {
      value = (arg[8] == '=') ? (arg + 9) : ((i + 1) < argc ? argv[++i] : nullptr);
      char* end;
      offset = value ? std::strtoull(value, &end, 0) : 0;
      if (!value || *value == '\0' || *end != '\0')
      {
        Usage(argv[0]);
        return 2;
      }
    }
    else if (std::strcmp(arg, "--mode") == 0 || std::strncmp(arg, "--mode=", 7) == 0)
    {
      value = (arg[6] == '=') ? (arg + 7) : ((i + 1) < argc ? argv[++i] : nullptr);
      mode.reset();
      for (uint32_t m = 0; value && m < std::size(MODE_NAMES); m++)
      {
        if (std::strcmp(value, MODE_NAMES[m]) == 0)
          mode = m;
      }
      if (!mode.has_value())
      {
        Usage(argv[0]);
        return 2;
      }
    }
    else
    {
      files.push_back(arg);
    }
  }

  if (files.empty())
  {
    Usage(argv[0]);
    return 2;
  }

  int result = 0;
  for (const char* path : files)
  {
    if (files.size() > 1)
      std::printf("# %s\n", path);
    std::fflush(stdout);

    std::FILE* fp = std::fopen(path, "rb");
    if (!fp)
    {
      std::fprintf(stderr, "error: %s: %s\n", path, std::strerror(errno));
      result = 1;
      continue;
    }

    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), fp)) > 0)
      data.insert(data.end(), chunk, chunk + n);
    std::fclose(fp);

    std::vector<std::string> names;
    const size_t start = std::min(offset, data.size());
    if (GetNames(data.data() + start, data.size() - start, mode, verbose, names))
    {
      for (const std::string& name : names)
        std::printf("%s\n", name.c_str());
    }
    else
    {
      std::fprintf(stderr, "error: %s: %s\n", path, s_error.c_str());
      result = 1;
    }
  }

  return result;
}
