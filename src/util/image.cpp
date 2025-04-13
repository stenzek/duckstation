// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "image.h"
#include "texture_decompress.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include <jpeglib.h>
#include <plutosvg.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

// clang-format off
#ifdef _MSC_VER
#pragma warning(disable : 4611) // warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable
#endif
// clang-format on

LOG_CHANNEL(Image);

static bool PNGBufferLoader(Image* image, std::span<const u8> data, Error* error);
static bool PNGBufferSaver(const Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool PNGFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool PNGFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

static bool JPEGBufferLoader(Image* image, std::span<const u8> data, Error* error);
static bool JPEGBufferSaver(const Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool JPEGFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool JPEGFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

static bool WebPBufferLoader(Image* image, std::span<const u8> data, Error* error);
static bool WebPBufferSaver(const Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool WebPFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool WebPFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

static bool DDSBufferLoader(Image* image, std::span<const u8> data, Error* error);
static bool DDSFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error);

namespace {
struct FormatHandler
{
  const char* extension;
  bool (*buffer_loader)(Image*, std::span<const u8>, Error*);
  bool (*buffer_saver)(const Image&, DynamicHeapArray<u8>*, u8, Error*);
  bool (*file_loader)(Image*, std::string_view, std::FILE*, Error*);
  bool (*file_saver)(const Image&, std::string_view, std::FILE*, u8, Error*);
};
} // namespace

static constexpr FormatHandler s_format_handlers[] = {
  {"png", PNGBufferLoader, PNGBufferSaver, PNGFileLoader, PNGFileSaver},
  {"jpg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
  {"jpeg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
  {"webp", WebPBufferLoader, WebPBufferSaver, WebPFileLoader, WebPFileSaver},
  {"dds", DDSBufferLoader, nullptr, DDSFileLoader, nullptr},
};

static const FormatHandler* GetFormatHandler(std::string_view extension)
{
  for (const FormatHandler& handler : s_format_handlers)
  {
    if (StringUtil::Strncasecmp(extension.data(), handler.extension, extension.size()) == 0)
      return &handler;
  }

  return nullptr;
}

static void SwapBGRAToRGBA(void* RESTRICT pixels_out, u32 pixels_out_pitch, const void* RESTRICT pixels_in,
                           u32 pixels_in_pitch, u32 width, u32 height);

Image::Image() = default;

Image::Image(const Image& copy)
{
  SetPixels(copy.m_width, copy.m_height, copy.m_format, copy.m_pixels.get(), copy.m_pitch);
}

Image::Image(u32 width, u32 height, ImageFormat format, const void* pixels, u32 pitch)
{
  SetPixels(width, height, format, pixels, pitch);
}

Image::Image(u32 width, u32 height, ImageFormat format, PixelStorage pixels, u32 pitch)
  : m_width(width), m_height(height), m_pitch(pitch), m_format(format), m_pixels(std::move(pixels))
{
}

Image::Image(u32 width, u32 height, ImageFormat format)
{
  Resize(width, height, format, false);
}

Image::Image(Image&& move)
{
  m_width = std::exchange(move.m_width, 0);
  m_height = std::exchange(move.m_height, 0);
  m_pitch = std::exchange(move.m_pitch, 0);
  m_format = std::exchange(move.m_format, ImageFormat::None);
  m_pixels = std::move(move.m_pixels);
}

void Image::Resize(u32 new_width, u32 new_height, bool preserve)
{
  Resize(new_width, new_height, m_format, preserve);
}

void Image::Resize(u32 new_width, u32 new_height, ImageFormat format, bool preserve)
{
  if (m_width == new_width && m_height == new_height && m_format == format)
    return;

  if (!preserve)
    m_pixels.reset();

  const u32 old_blocks_y = GetBlocksHigh();
  const u32 old_pitch = m_pitch;
  PixelStorage old_pixels =
    std::exchange(m_pixels, Common::make_unique_aligned_for_overwrite<u8[]>(
                              VECTOR_ALIGNMENT, CalculateStorageSize(new_width, new_height, format)));

  m_width = new_width;
  m_height = new_height;
  m_format = format;
  m_pitch = CalculatePitch(new_width, new_height, format);
  if (preserve && old_pixels)
  {
    StringUtil::StrideMemCpy(m_pixels.get(), m_pitch, old_pixels.get(), old_pitch, std::min(old_pitch, m_pitch),
                             std::min(old_blocks_y, GetBlocksHigh()));
  }
}

Image& Image::operator=(const Image& copy)
{
  SetPixels(copy.m_width, copy.m_height, copy.m_format, copy.m_pixels.get(), copy.m_pitch);
  return *this;
}

Image& Image::operator=(Image&& move)
{
  m_width = std::exchange(move.m_width, 0);
  m_height = std::exchange(move.m_height, 0);
  m_pitch = std::exchange(move.m_pitch, 0);
  m_format = std::exchange(move.m_format, ImageFormat::None);
  m_pixels = std::move(move.m_pixels);
  return *this;
}

const char* Image::GetFormatName(ImageFormat format)
{
  static constexpr std::array names = {
    "None",   // None
    "RGBA8",  // RGBA8
    "BGRA8",  // BGRA8
    "RGB565", // RGB565
    "RGB5A1", // RGB5A1
    "A1BGR5", // A1BGR5
    "BGR8",   // BGR8
    "BC1",    // BC1
    "BC2",    // BC2
    "BC3",    // BC3
    "BC7",    // BC7
  };
  static_assert(names.size() == static_cast<size_t>(ImageFormat::MaxCount));

  return names[static_cast<size_t>(format)];
}

u32 Image::GetPixelSize(ImageFormat format)
{
  static constexpr std::array<u8, static_cast<size_t>(ImageFormat::MaxCount)> sizes = {{
    0,  // Unknown
    4,  // RGBA8
    4,  // BGRA8
    2,  // RGB565
    2,  // RGB5A1
    2,  // A1BGR5
    3,  // BGR8
    8,  // BC1 - 16 pixels in 64 bits
    16, // BC2 - 16 pixels in 128 bits
    16, // BC3 - 16 pixels in 128 bits
    16, // BC4 - 16 pixels in 128 bits
  }};

  return sizes[static_cast<size_t>(format)];
}

bool Image::IsCompressedFormat(ImageFormat format)
{
  return (format >= ImageFormat::BC1);
}

u32 Image::CalculatePitch(u32 width, u32 height, ImageFormat format)
{
  const u32 pixel_size = GetPixelSize(format);
  if (!IsCompressedFormat(format))
    return Common::AlignUpPow2(width * pixel_size, 4);

  // All compressed formats use a block size of 4.
  const u32 blocks_wide = Common::AlignUpPow2(width, 4) / 4;
  return blocks_wide * pixel_size;
}

u32 Image::CalculateStorageSize(u32 width, u32 height, ImageFormat format)
{
  const u32 pixel_size = GetPixelSize(format);
  if (!IsCompressedFormat(format))
    return Common::AlignUpPow2(width * pixel_size, 4) * height;

  const u32 blocks_wide = Common::AlignUpPow2(width, 4) / 4;
  const u32 blocks_high = Common::AlignUpPow2(height, 4) / 4;
  return (blocks_wide * pixel_size) * blocks_high;
}

u32 Image::CalculateStorageSize(u32 width, u32 height, u32 pitch, ImageFormat format)
{
  height = IsCompressedFormat(format) ? (Common::AlignUpPow2(height, 4) / 4) : height;
  return pitch * height;
}

u32 Image::GetBlocksWide() const
{
  return IsCompressedFormat(m_format) ? (Common::AlignUpPow2(m_width, 4) / 4) : m_width;
}

u32 Image::GetBlocksHigh() const
{
  return IsCompressedFormat(m_format) ? (Common::AlignUpPow2(m_height, 4) / 4) : m_height;
}

u32 Image::GetStorageSize() const
{
  return GetBlocksHigh() * m_pitch;
}

std::span<const u8> Image::GetPixelsSpan() const
{
  return std::span<const u8>(m_pixels.get(), GetStorageSize());
}

std::span<u8> Image::GetPixelsSpan()
{
  return std::span<u8>(m_pixels.get(), GetStorageSize());
}

void Image::Clear()
{
  std::memset(m_pixels.get(), 0, CalculateStorageSize(m_width, m_height, m_pitch, m_format));
}

void Image::Invalidate()
{
  m_width = 0;
  m_height = 0;
  m_pitch = 0;
  m_format = ImageFormat::None;
  m_pixels.reset();
}

void Image::SetPixels(u32 width, u32 height, ImageFormat format, const void* pixels, u32 pitch)
{
  Resize(width, height, format, false);
  if (m_pixels)
    StringUtil::StrideMemCpy(m_pixels.get(), m_pitch, pixels, pitch, m_pitch, GetBlocksHigh());
}

void Image::SetPixels(u32 width, u32 height, ImageFormat format, PixelStorage pixels, u32 pitch)
{
  m_width = width;
  m_height = height;
  m_format = format;
  m_pitch = pitch;
  m_pixels = std::move(pixels);
}

bool Image::SetAllPixelsOpaque()
{
  if (m_format == ImageFormat::RGBA8 || m_format == ImageFormat::BGRA8)
  {
    for (u32 y = 0; y < m_height; y++)
    {
      u8* row = GetRowPixels(y);
      for (u32 x = 0; x < m_width; x++, row += sizeof(u32))
        row[3] = 0xFF;
    }

    return true;
  }
  else if (m_format == ImageFormat::RGB5A1)
  {
    for (u32 y = 0; y < m_height; y++)
    {
      u8* row = GetRowPixels(y);
      for (u32 x = 0; x < m_width; x++, row += sizeof(u32))
        row[1] |= 0x80;
    }

    return true;
  }
  else if (m_format == ImageFormat::RGB565)
  {
    // Already opaque
    return true;
  }
  else
  {
    // Unhandled format
    return false;
  }
}

Image::PixelStorage Image::TakePixels()
{
  m_width = 0;
  m_height = 0;
  m_format = ImageFormat::None;
  m_pitch = 0;
  return std::move(m_pixels);
}

bool Image::LoadFromFile(const char* filename, Error* error /* = nullptr */)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb", error);
  if (!fp)
    return false;

  return LoadFromFile(filename, fp.get(), error);
}

bool Image::SaveToFile(const char* filename, u8 quality /* = DEFAULT_SAVE_QUALITY */,
                       Error* error /* = nullptr */) const
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb", error);
  if (!fp)
    return false;

  if (SaveToFile(filename, fp.get(), quality, error))
    return true;

  // save failed
  fp.reset();
  FileSystem::DeleteFile(filename);
  return false;
}

bool Image::LoadFromFile(std::string_view filename, std::FILE* fp, Error* error /* = nullptr */)
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_loader)
  {
    Error::SetStringFmt(error, "Unknown extension '{}'", extension);
    return false;
  }

  return handler->file_loader(this, filename, fp, error);
}

bool Image::LoadFromBuffer(std::string_view filename, std::span<const u8> data, Error* error /* = nullptr */)
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->buffer_loader)
  {
    Error::SetStringFmt(error, "Unknown extension '{}'", extension);
    return false;
  }

  return handler->buffer_loader(this, data, error);
}

bool Image::RasterizeSVG(const std::span<const u8> data, u32 width, u32 height, Error* error)
{
  if (width == 0 || height == 0)
  {
    Error::SetStringFmt(error, "Invalid dimensions: {}x{}", width, height);
    return false;
  }

  std::unique_ptr<plutosvg_document, void (*)(plutosvg_document*)> doc(
    plutosvg_document_load_from_data(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size_bytes()),
                                     static_cast<float>(width), static_cast<float>(height), nullptr, nullptr),
    plutosvg_document_destroy);
  if (!doc)
  {
    Error::SetStringView(error, "plutosvg_document_load_from_data() failed");
    return false;
  }

  const plutovg_color_t current_color = {.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};

  std::unique_ptr<plutovg_surface, void (*)(plutovg_surface*)> bitmap(
    plutosvg_document_render_to_surface(doc.get(), nullptr, static_cast<int>(width), static_cast<int>(height),
                                        &current_color, nullptr, nullptr),
    plutovg_surface_destroy);
  if (!bitmap)
  {
    Error::SetStringView(error, "plutosvg_document_render_to_surface() failed");
    return false;
  }

  // lunasvg works in BGRA, swap to RGBA
  Resize(width, height, ImageFormat::RGBA8, false);
  SwapBGRAToRGBA(m_pixels.get(), m_pitch, plutovg_surface_get_data(bitmap.get()),
                 plutovg_surface_get_stride(bitmap.get()), width, height);
  return true;
}

bool Image::SaveToFile(std::string_view filename, std::FILE* fp, u8 quality /* = DEFAULT_SAVE_QUALITY */,
                       Error* error /* = nullptr */) const
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_saver)
  {
    Error::SetStringFmt(error, "Unknown extension '{}'", extension);
    return false;
  }

  if (!handler->file_saver(*this, filename, fp, quality, error))
    return false;

  if (std::fflush(fp) != 0)
  {
    Error::SetErrno(error, "fflush() failed: ", errno);
    return false;
  }

  return true;
}

std::optional<DynamicHeapArray<u8>> Image::SaveToBuffer(std::string_view filename,
                                                        u8 quality /* = DEFAULT_SAVE_QUALITY */,
                                                        Error* error /* = nullptr */) const
{
  std::optional<DynamicHeapArray<u8>> ret;

  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_saver)
  {
    Error::SetStringFmt(error, "Unknown extension '{}'", extension);
    return ret;
  }

  ret = DynamicHeapArray<u8>();
  if (!handler->buffer_saver(*this, &ret.value(), quality, error))
    ret.reset();

  return ret;
}

void SwapBGRAToRGBA(void* pixels_out, u32 pixels_out_pitch, const void* pixels_in, u32 pixels_in_pitch, u32 width,
                    u32 height)
{
#ifdef GSVECTOR_HAS_FAST_INT_SHUFFLE8
  constexpr u32 pixels_per_vec = sizeof(GSVector4i) / 4;
  const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);
#endif

  const u8* pixels_in_ptr = static_cast<const u8*>(pixels_in);
  u8* pixels_out_ptr = static_cast<u8*>(pixels_out);
  for (u32 y = 0; y < height; y++)
  {
    const u8* row_pixels_in_ptr = pixels_in_ptr;
    u8* row_pixels_out_ptr = pixels_out_ptr;
    u32 x = 0;

#ifdef GSVECTOR_HAS_FAST_INT_SHUFFLE8
    for (; x < aligned_width; x += pixels_per_vec)
    {
      static constexpr GSVector4i mask = GSVector4i::cxpr8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
      GSVector4i::store<false>(row_pixels_out_ptr, GSVector4i::load<false>(row_pixels_in_ptr).shuffle8(mask));
      row_pixels_in_ptr += sizeof(GSVector4i);
      row_pixels_out_ptr += sizeof(GSVector4i);
    }
#endif

    for (; x < width; x++)
    {
      u32 pixel;
      std::memcpy(&pixel, row_pixels_in_ptr, sizeof(pixel));
      pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
      std::memcpy(row_pixels_out_ptr, &pixel, sizeof(pixel));
      row_pixels_in_ptr += sizeof(pixel);
      row_pixels_out_ptr += sizeof(pixel);
    }

    pixels_in_ptr += pixels_in_pitch;
    pixels_out_ptr += pixels_out_pitch;
  }
}

template<ImageFormat format>
static void DecompressBC(void* RESTRICT pixels_out, u32 pixels_out_pitch, const void* RESTRICT pixels_in,
                         u32 pixels_in_pitch, u32 width, u32 height)
{
  constexpr u32 BC_BLOCK_SIZE = 4;
  constexpr u32 BC_BLOCK_BYTES = 16;

  const u32 blocks_wide = Common::AlignUpPow2(width, 4) / 4;
  const u32 blocks_high = Common::AlignUpPow2(height, 4) / 4;
  for (u32 y = 0; y < blocks_high; y++)
  {
    const u8* block_in = static_cast<const u8*>(pixels_in) + (y * pixels_in_pitch);
    for (u32 x = 0; x < blocks_wide; x++, block_in += BC_BLOCK_BYTES)
    {
      // decompress block
      switch (format)
      {
        case ImageFormat::BC1:
        {
          DecompressBlockBC1(x * BC_BLOCK_SIZE, y * BC_BLOCK_SIZE, pixels_out_pitch, block_in,
                             static_cast<unsigned char*>(pixels_out));
        }
        break;
        case ImageFormat::BC2:
        {
          DecompressBlockBC2(x * BC_BLOCK_SIZE, y * BC_BLOCK_SIZE, pixels_out_pitch, block_in,
                             static_cast<unsigned char*>(pixels_out));
        }
        break;
        case ImageFormat::BC3:
        {
          DecompressBlockBC3(x * BC_BLOCK_SIZE, y * BC_BLOCK_SIZE, pixels_out_pitch, block_in,
                             static_cast<unsigned char*>(pixels_out));
        }
        break;

        case ImageFormat::BC7:
        {
          u32 block_pixels_out[BC_BLOCK_SIZE * BC_BLOCK_SIZE];
          bc7decomp::unpack_bc7(block_in, reinterpret_cast<bc7decomp::color_rgba*>(block_pixels_out));

          // and write it to the new image
          const u32* RESTRICT copy_in_ptr = block_pixels_out;
          u8* RESTRICT copy_out_ptr =
            static_cast<u8*>(pixels_out) + (y * BC_BLOCK_SIZE * pixels_out_pitch) + (x * BC_BLOCK_SIZE * sizeof(u32));
          for (u32 sy = 0; sy < 4; sy++)
          {
            std::memcpy(copy_out_ptr, copy_in_ptr, sizeof(u32) * BC_BLOCK_SIZE);
            copy_in_ptr += BC_BLOCK_SIZE;
            copy_out_ptr += pixels_out_pitch;
          }
        }
        break;
      }
    }
  }
}

std::optional<Image> Image::ConvertToRGBA8(Error* error) const
{
  std::optional<Image> ret;

  if (!IsValid())
  {
    Error::SetStringView(error, "Image is not valid.");
    return ret;
  }

  ret = Image(m_width, m_height, ImageFormat::RGBA8);
  if (!ConvertToRGBA8(ret->GetPixels(), ret->GetPitch(), m_pixels.get(), m_pitch, m_width, m_height, m_format, error))
    ret.reset();

  return ret;
}

bool Image::ConvertToRGBA8(void* RESTRICT pixels_out, u32 pixels_out_pitch, const void* RESTRICT pixels_in,
                           u32 pixels_in_pitch, u32 width, u32 height, ImageFormat format, Error* error)
{
  switch (format)
  {
    case ImageFormat::BGRA8:
    {
      SwapBGRAToRGBA(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, width, height);
      return true;
    }

    case ImageFormat::RGBA8:
    {
      StringUtil::StrideMemCpy(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, sizeof(u32) * width, height);
      return true;
    }

    case ImageFormat::RGB565:
    {
      constexpr u32 pixels_per_vec = 8;
      [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

      for (u32 y = 0; y < height; y++)
      {
        const u8* RESTRICT row_pixels_in = static_cast<const u8*>(pixels_in) + (y * pixels_in_pitch);
        u8* RESTRICT row_pixels_out = static_cast<u8*>(pixels_out) + (y * pixels_out_pitch);
        u32 x = 0;

#ifdef CPU_ARCH_SIMD
        for (; x < aligned_width; x += pixels_per_vec)
        {
          GSVector4i rgb565 = GSVector4i::load<false>(row_pixels_in);
          row_pixels_in += sizeof(u16) * pixels_per_vec;

          GSVector4i r = rgb565.srl16<11>();
          r = r.sll16<3>() | r.sll16<13>().srl16<13>();

          GSVector4i g = rgb565.sll16<5>().srl16<10>();
          g = g.sll16<2>() | g.sll16<14>().srl16<14>();

          GSVector4i b = rgb565.sll16<11>().srl16<11>();
          b = b.sll16<3>() | b.sll16<13>().srl16<13>();

          const GSVector4i low =
            r.u16to32() | g.u16to32().sll32<8>() | b.u16to32().sll32<16>() | GSVector4i::cxpr(0xFF000000);
          const GSVector4i high = r.uph64().u16to32() | g.uph64().u16to32().sll32<8>() |
                                  b.uph64().u16to32().sll32<16>() | GSVector4i::cxpr(0xFF000000);

          GSVector4i::store<false>(row_pixels_out, low);
          row_pixels_out += sizeof(GSVector4i);

          GSVector4i::store<false>(row_pixels_out, high);
          row_pixels_out += sizeof(GSVector4i);
        }
#endif

        DONT_VECTORIZE_THIS_LOOP
        for (; x < width; x++)
        {
          // RGB565 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, row_pixels_in, sizeof(u16));
          row_pixels_in += sizeof(u16);
          const u8 r5 = Truncate8(pixel_in >> 11);
          const u8 g6 = Truncate8((pixel_in >> 5) & 0x3F);
          const u8 b5 = Truncate8(pixel_in & 0x1F);
          const u32 rgba8 = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 2) | (g6 & 3)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (0xFF000000u);
          std::memcpy(row_pixels_out, &rgba8, sizeof(u32));
          row_pixels_out += sizeof(u32);
        }
      }

      return true;
    }

    case ImageFormat::RGB5A1:
    {
      constexpr u32 pixels_per_vec = 8;
      [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

      for (u32 y = 0; y < height; y++)
      {
        const u8* RESTRICT row_pixels_in = static_cast<const u8*>(pixels_in) + (y * pixels_in_pitch);
        u8* RESTRICT row_pixels_out = static_cast<u8*>(pixels_out) + (y * pixels_out_pitch);
        u32 x = 0;

#ifdef CPU_ARCH_SIMD
        for (; x < aligned_width; x += pixels_per_vec)
        {
          GSVector4i rgb5a1 = GSVector4i::load<false>(row_pixels_in);
          row_pixels_in += sizeof(u16) * pixels_per_vec;

          GSVector4i r = rgb5a1.sll16<1>().srl16<11>();
          r = r.sll16<3>() | r.sll16<13>().srl16<13>();

          GSVector4i g = rgb5a1.sll16<6>().srl16<11>();
          g = g.sll16<3>() | g.sll16<13>().srl16<13>();

          GSVector4i b = rgb5a1.sll16<11>().srl16<11>();
          b = b.sll16<3>() | b.sll16<13>().srl16<13>();

          GSVector4i a = rgb5a1.sra16<7>().srl16<8>();

          const GSVector4i low =
            r.u16to32() | g.u16to32().sll32<8>() | b.u16to32().sll32<16>() | a.u16to32().sll32<24>();
          const GSVector4i high = r.uph64().u16to32() | g.uph64().u16to32().sll32<8>() |
                                  b.uph64().u16to32().sll32<16>() | a.uph64().u16to32().sll32<24>();

          GSVector4i::store<false>(row_pixels_out, low);
          row_pixels_out += sizeof(GSVector4i);

          GSVector4i::store<false>(row_pixels_out, high);
          row_pixels_out += sizeof(GSVector4i);
        }
#endif

        DONT_VECTORIZE_THIS_LOOP
        for (; x < width; x++)
        {
          // RGB5A1 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, row_pixels_in, sizeof(u16));
          row_pixels_in += sizeof(u16);
          const u8 a1 = Truncate8(pixel_in >> 15);
          const u8 r5 = Truncate8((pixel_in >> 10) & 0x1F);
          const u8 g6 = Truncate8((pixel_in >> 5) & 0x1F);
          const u8 b5 = Truncate8(pixel_in & 0x1F);
          const u32 rgba8 = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 3) | (g6 & 7)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (a1 ? 0xFF000000u : 0u);
          std::memcpy(row_pixels_out, &rgba8, sizeof(u32));
          row_pixels_out += sizeof(u32);
        }
      }

      return true;
    }

    case ImageFormat::A1BGR5:
    {
      constexpr u32 pixels_per_vec = 8;
      [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

      for (u32 y = 0; y < height; y++)
      {
        const u8* RESTRICT row_pixels_in = static_cast<const u8*>(pixels_in) + (y * pixels_in_pitch);
        u8* RESTRICT row_pixels_out = static_cast<u8*>(pixels_out) + (y * pixels_out_pitch);
        u32 x = 0;

#ifdef CPU_ARCH_SIMD
        for (; x < aligned_width; x += pixels_per_vec)
        {
          GSVector4i a1bgr5 = GSVector4i::load<false>(row_pixels_in);
          row_pixels_in += sizeof(u16) * pixels_per_vec;

          GSVector4i r = a1bgr5.srl16<11>();
          r = r.sll16<3>() | r.sll16<13>().srl16<13>();

          GSVector4i g = a1bgr5.sll16<5>().srl16<11>();
          g = g.sll16<3>() | g.sll16<13>().srl16<13>();

          GSVector4i b = a1bgr5.sll16<10>().srl16<11>();
          b = b.sll16<3>() | b.sll16<13>().srl16<13>();

          GSVector4i a = a1bgr5.sll16<15>().sra16<7>().srl16<8>();

          const GSVector4i low =
            r.u16to32() | g.u16to32().sll32<8>() | b.u16to32().sll32<16>() | a.u16to32().sll32<24>();
          const GSVector4i high = r.uph64().u16to32() | g.uph64().u16to32().sll32<8>() |
                                  b.uph64().u16to32().sll32<16>() | a.uph64().u16to32().sll32<24>();

          GSVector4i::store<false>(row_pixels_out, low);
          row_pixels_out += sizeof(GSVector4i);

          GSVector4i::store<false>(row_pixels_out, high);
          row_pixels_out += sizeof(GSVector4i);
        }
#endif

        DONT_VECTORIZE_THIS_LOOP
        for (; x < width; x++)
        {
          // RGB5A1 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, row_pixels_in, sizeof(u16));
          row_pixels_in += sizeof(u16);
          const u8 a1 = Truncate8(pixel_in & 0x01);
          const u8 r5 = Truncate8((pixel_in >> 11) & 0x1F);
          const u8 g6 = Truncate8((pixel_in >> 6) & 0x1F);
          const u8 b5 = Truncate8((pixel_in >> 1) & 0x1F);
          const u32 rgba8 = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 3) | (g6 & 7)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (a1 ? 0xFF000000u : 0u);
          std::memcpy(row_pixels_out, &rgba8, sizeof(u32));
          row_pixels_out += sizeof(u32);
        }
      }

      return true;
    }

    case ImageFormat::BGR8:
    {
      for (u32 y = 0; y < height; y++)
      {
        const u8* RESTRICT row_pixels_in = static_cast<const u8*>(pixels_in) + (y * pixels_in_pitch);
        u8* RESTRICT row_pixels_out = static_cast<u8*>(pixels_out) + (y * pixels_out_pitch);

        for (u32 x = 0; x < width; x++)
        {
          // Set alpha channel to full intensity.
          const u32 rgba = (ZeroExtend32(row_pixels_in[0]) | (ZeroExtend32(row_pixels_in[2]) << 8) |
                            (ZeroExtend32(row_pixels_in[2]) << 16) | 0xFF000000u);
          std::memcpy(row_pixels_out, &rgba, sizeof(rgba));
          row_pixels_in += 3;
          row_pixels_out += sizeof(rgba);
        }
      }

      return true;
    }
    break;

    case ImageFormat::BC1:
    {
      DecompressBC<ImageFormat::BC1>(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, width, height);
      return true;
    }
    break;

    case ImageFormat::BC2:
    {
      DecompressBC<ImageFormat::BC2>(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, width, height);
      return true;
    }
    break;

    case ImageFormat::BC3:
    {
      DecompressBC<ImageFormat::BC3>(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, width, height);
      return true;
    }
    break;

    case ImageFormat::BC7:
    {
      DecompressBC<ImageFormat::BC7>(pixels_out, pixels_out_pitch, pixels_in, pixels_in_pitch, width, height);
      return true;
    }

    default:
    {
      Error::SetStringFmt(error, "Unhandled format {}", GetFormatName(format));
      return false;
    }
  }
}

void Image::FlipY()
{
  if (!IsValid())
    return;

  PixelStorage temp = Common::make_unique_aligned_for_overwrite<u8[]>(VECTOR_ALIGNMENT, m_pitch);
  const u32 half_height = m_height / 2;
  for (u32 flip_row = 0; flip_row < half_height; flip_row++)
  {
    u8* top_ptr = &m_pixels[flip_row * m_pitch];
    u8* bottom_ptr = &m_pixels[((m_height - 1) - flip_row) * m_pitch];
    std::memcpy(temp.get(), top_ptr, m_pitch);
    std::memcpy(top_ptr, bottom_ptr, m_pitch);
    std::memcpy(bottom_ptr, temp.get(), m_pitch);
  }
}

static void PNGSetErrorFunction(png_structp png_ptr, Error* error)
{
  png_set_error_fn(
    png_ptr, error,
    [](png_structp png_ptr, png_const_charp message) {
      Error::SetStringView(static_cast<Error*>(png_get_error_ptr(png_ptr)), message);
      png_longjmp(png_ptr, 1);
    },
    [](png_structp png_ptr, png_const_charp message) { WARNING_LOG("libpng warning: {}", message); });
}

static bool PNGCommonLoader(Image* image, png_structp png_ptr, png_infop info_ptr, std::vector<png_bytep>& row_pointers)
{
  png_read_info(png_ptr, info_ptr);

  const u32 width = png_get_image_width(png_ptr, info_ptr);
  const u32 height = png_get_image_height(png_ptr, info_ptr);
  const png_byte color_type = png_get_color_type(png_ptr, info_ptr);
  const png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

  // Read any color_type into 8bit depth, RGBA format.
  // See http://www.libpng.org/pub/png/libpng-manual.txt

  if (bit_depth == 16)
    png_set_strip_16(png_ptr);

  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png_ptr);

  // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png_ptr);

  if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png_ptr);

  // These color_type don't have an alpha channel then fill it with 0xff.
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);

  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png_ptr);

  png_read_update_info(png_ptr, info_ptr);

  image->Resize(width, height, ImageFormat::RGBA8, false);
  row_pointers.reserve(height);
  for (u32 y = 0; y < height; y++)
    row_pointers.push_back(reinterpret_cast<png_bytep>(image->GetRowPixels(y)));

  png_read_image(png_ptr, row_pointers.data());
  return true;
}

bool PNGFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error)
{
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr)
  {
    Error::SetStringView(error, "png_create_read_struct() failed.");
    return false;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    Error::SetStringView(error, "png_create_info_struct() failed.");
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() { png_destroy_read_struct(&png_ptr, &info_ptr, nullptr); });

  std::vector<png_bytep> row_pointers;

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
  {
    image->Invalidate();
    return false;
  }

  png_set_read_fn(png_ptr, fp, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
    std::FILE* fp = static_cast<std::FILE*>(png_get_io_ptr(png_ptr));
    if (std::fread(data_ptr, size, 1, fp) != 1)
      png_error(png_ptr, "fread() failed");
  });

  return PNGCommonLoader(image, png_ptr, info_ptr, row_pointers);
}

bool PNGBufferLoader(Image* image, std::span<const u8> data, Error* error)
{
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr)
  {
    Error::SetStringView(error, "png_create_read_struct() failed.");
    return false;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    Error::SetStringView(error, "png_create_info_struct() failed.");
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() { png_destroy_read_struct(&png_ptr, &info_ptr, nullptr); });

  std::vector<png_bytep> row_pointers;

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
  {
    image->Invalidate();
    return false;
  }

  struct IOData
  {
    std::span<const u8> buffer;
    size_t buffer_pos;
  };
  IOData iodata = {data, 0};

  png_set_read_fn(png_ptr, &iodata, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
    IOData* data = static_cast<IOData*>(png_get_io_ptr(png_ptr));
    const size_t read_size = std::min<size_t>(data->buffer.size() - data->buffer_pos, size);
    if (read_size > 0)
    {
      std::memcpy(data_ptr, &data->buffer[data->buffer_pos], read_size);
      data->buffer_pos += read_size;
    }
  });

  return PNGCommonLoader(image, png_ptr, info_ptr, row_pointers);
}

static void PNGSaveCommon(const Image& image, png_structp png_ptr, png_infop info_ptr, u8 quality)
{
  png_set_compression_level(png_ptr, std::clamp(quality / 10, 0, 9));
  png_set_IHDR(png_ptr, info_ptr, image.GetWidth(), image.GetHeight(), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png_ptr, info_ptr);

  for (u32 y = 0; y < image.GetHeight(); ++y)
    png_write_row(png_ptr, (png_bytep)image.GetRowPixels(y));

  png_write_end(png_ptr, nullptr);
}

bool PNGFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
{
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = nullptr;
  if (!png_ptr)
  {
    Error::SetStringView(error, "png_create_write_struct() failed.");
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() {
    if (png_ptr)
      png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
  });

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    Error::SetStringView(error, "png_create_info_struct() failed.");
    return false;
  }

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_set_write_fn(
    png_ptr, fp,
    [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
      if (std::fwrite(data_ptr, size, 1, static_cast<std::FILE*>(png_get_io_ptr(png_ptr))) != 1)
        png_error(png_ptr, "fwrite() failed");
    },
    [](png_structp png_ptr) {});

  PNGSaveCommon(image, png_ptr, info_ptr, quality);
  return true;
}

bool PNGBufferSaver(const Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error)
{
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = nullptr;
  if (!png_ptr)
  {
    Error::SetStringView(error, "png_create_write_struct() failed.");
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() {
    if (png_ptr)
      png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
  });

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    Error::SetStringView(error, "png_create_info_struct() failed.");
    return false;
  }

  struct IOData
  {
    DynamicHeapArray<u8>* buffer;
    size_t buffer_pos;
  };
  IOData iodata = {data, 0};

  data->resize(image.GetWidth() * image.GetHeight() * 2);

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_set_write_fn(
    png_ptr, data,
    [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
      IOData* iodata = static_cast<IOData*>(png_get_io_ptr(png_ptr));
      const size_t new_pos = iodata->buffer_pos + size;
      if (new_pos > iodata->buffer->size())
        iodata->buffer->resize(std::max(new_pos, iodata->buffer->size() * 2));
      std::memcpy(iodata->buffer->data() + iodata->buffer_pos, data_ptr, size);
      iodata->buffer_pos += size;
    },
    [](png_structp png_ptr) {});

  PNGSaveCommon(image, png_ptr, info_ptr, quality);
  iodata.buffer->resize(iodata.buffer_pos);
  return true;
}

namespace {
struct JPEGErrorHandler
{
  jpeg_error_mgr err;
  Error* errptr;
  fastjmp_buf jbuf;

  JPEGErrorHandler(Error* errptr_)
  {
    jpeg_std_error(&err);
    err.error_exit = &ErrorExit;
    errptr = errptr_;
  }

  static void ErrorExit(j_common_ptr cinfo)
  {
    JPEGErrorHandler* eh = (JPEGErrorHandler*)cinfo->err;
    char msg[JMSG_LENGTH_MAX];
    eh->err.format_message(cinfo, msg);
    Error::SetStringFmt(eh->errptr, "libjpeg fatal error: {}", msg);
    fastjmp_jmp(&eh->jbuf, 1);
  }
};
} // namespace

template<typename T>
static bool WrapJPEGDecompress(Image* image, Error* error, T setup_func)
{
  std::vector<u8> scanline;
  jpeg_decompress_struct info = {};

  // NOTE: Be **very** careful not to allocate memory after calling this function.
  // It won't get freed, because fastjmp does not unwind the stack.
  JPEGErrorHandler errhandler(error);
  if (fastjmp_set(&errhandler.jbuf) != 0)
  {
    jpeg_destroy_decompress(&info);
    return false;
  }

  info.err = &errhandler.err;
  jpeg_create_decompress(&info);
  setup_func(info);

  const int herr = jpeg_read_header(&info, TRUE);
  if (herr != JPEG_HEADER_OK)
  {
    Error::SetStringFmt(error, "jpeg_read_header() returned {}", herr);
    return false;
  }

  if (info.image_width == 0 || info.image_height == 0 || info.num_components < 3)
  {
    Error::SetStringFmt(error, "Invalid image dimensions: {}x{}x{}", info.image_width, info.image_height,
                        info.num_components);
    return false;
  }

  info.out_color_space = JCS_RGB;
  info.out_color_components = 3;

  if (!jpeg_start_decompress(&info))
  {
    Error::SetStringFmt(error, "jpeg_start_decompress() returned failure");
    return false;
  }

  image->Resize(info.image_width, info.image_height, ImageFormat::RGBA8, false);
  scanline.resize(info.image_width * 3);

  u8* scanline_buffer[1] = {scanline.data()};
  bool result = true;
  for (u32 y = 0; y < info.image_height; y++)
  {
    if (jpeg_read_scanlines(&info, scanline_buffer, 1) != 1)
    {
      Error::SetStringFmt(error, "jpeg_read_scanlines() failed at row {}", y);
      result = false;
      break;
    }

    // RGB -> RGBA
    const u8* src_ptr = scanline.data();
    u8* dst_ptr = image->GetRowPixels(y);
    for (u32 x = 0; x < info.image_width; x++)
    {
      const u32 pixel32 =
        (ZeroExtend32(src_ptr[0]) | (ZeroExtend32(src_ptr[1]) << 8) | (ZeroExtend32(src_ptr[2]) << 16) | 0xFF000000u);
      std::memcpy(dst_ptr, &pixel32, sizeof(pixel32));
      dst_ptr += sizeof(pixel32);
      src_ptr += 3;
    }
  }

  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  return result;
}

bool JPEGBufferLoader(Image* image, std::span<const u8> data, Error* error)
{
  return WrapJPEGDecompress(image, error, [data](jpeg_decompress_struct& info) {
    jpeg_mem_src(&info, static_cast<const unsigned char*>(data.data()), static_cast<unsigned long>(data.size()));
  });
}

bool JPEGFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error)
{
  static constexpr u32 BUFFER_SIZE = 16384;

  struct FileCallback
  {
    jpeg_source_mgr mgr;

    std::FILE* fp;
    std::unique_ptr<u8[]> buffer;
    Error* errptr;
    bool end_of_file;
  };

  FileCallback cb = {
    .mgr = {
      .next_input_byte = nullptr,
      .bytes_in_buffer = 0,
      .init_source = [](j_decompress_ptr cinfo) {},
      .fill_input_buffer = [](j_decompress_ptr cinfo) -> boolean {
        FileCallback* cb = BASE_FROM_RECORD_FIELD(cinfo->src, FileCallback, mgr);
        cb->mgr.next_input_byte = cb->buffer.get();
        if (cb->end_of_file)
        {
          cb->buffer[0] = 0xFF;
          cb->buffer[1] = JPEG_EOI;
          cb->mgr.bytes_in_buffer = 2;
          return TRUE;
        }

        const size_t r = std::fread(cb->buffer.get(), 1, BUFFER_SIZE, cb->fp);
        cb->end_of_file |= (std::feof(cb->fp) != 0);
        cb->mgr.bytes_in_buffer = r;
        return TRUE;
      },
      .skip_input_data =
        [](j_decompress_ptr cinfo, long num_bytes) {
          FileCallback* cb = BASE_FROM_RECORD_FIELD(cinfo->src, FileCallback, mgr);
          const size_t skip_in_buffer = std::min<size_t>(cb->mgr.bytes_in_buffer, static_cast<size_t>(num_bytes));
          cb->mgr.next_input_byte += skip_in_buffer;
          cb->mgr.bytes_in_buffer -= skip_in_buffer;

          const size_t seek_cur = static_cast<size_t>(num_bytes) - skip_in_buffer;
          if (seek_cur > 0)
          {
            if (!FileSystem::FSeek64(cb->fp, static_cast<size_t>(seek_cur), SEEK_CUR, cb->errptr))
            {
              cb->end_of_file = true;
              return;
            }
          }
        },
      .resync_to_restart = jpeg_resync_to_restart,
      .term_source = [](j_decompress_ptr cinfo) {},
    },
    .fp = fp,
    .buffer = std::make_unique<u8[]>(BUFFER_SIZE),
    .errptr = error,
    .end_of_file = false,
  };

  return WrapJPEGDecompress(image, error, [&cb](jpeg_decompress_struct& info) { info.src = &cb.mgr; });
}

template<typename T>
static bool WrapJPEGCompress(const Image& image, u8 quality, Error* error, T setup_func)
{
  std::vector<u8> scanline;
  jpeg_compress_struct info = {};

  // NOTE: Be **very** careful not to allocate memory after calling this function.
  // It won't get freed, because fastjmp does not unwind the stack.
  JPEGErrorHandler errhandler(error);
  if (fastjmp_set(&errhandler.jbuf) != 0)
  {
    jpeg_destroy_compress(&info);
    return false;
  }

  info.err = &errhandler.err;
  jpeg_create_compress(&info);
  setup_func(info);

  info.image_width = image.GetWidth();
  info.image_height = image.GetHeight();
  info.in_color_space = JCS_RGB;
  info.input_components = 3;

  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, quality, TRUE);
  jpeg_start_compress(&info, TRUE);

  scanline.resize(image.GetWidth() * 3);
  u8* scanline_buffer[1] = {scanline.data()};
  bool result = true;
  for (u32 y = 0; y < info.image_height; y++)
  {
    // RGBA -> RGB
    u8* dst_ptr = scanline.data();
    const u8* src_ptr = image.GetRowPixels(y);
    for (u32 x = 0; x < info.image_width; x++)
    {
      u32 rgba;
      std::memcpy(&rgba, src_ptr, sizeof(rgba));
      src_ptr += sizeof(rgba);
      *(dst_ptr++) = Truncate8(rgba);
      *(dst_ptr++) = Truncate8(rgba >> 8);
      *(dst_ptr++) = Truncate8(rgba >> 16);
    }

    if (jpeg_write_scanlines(&info, scanline_buffer, 1) != 1)
    {
      Error::SetStringFmt(error, "jpeg_write_scanlines() failed at row {}", y);
      result = false;
      break;
    }
  }

  jpeg_finish_compress(&info);
  jpeg_destroy_compress(&info);
  return result;
}

bool JPEGBufferSaver(const Image& image, DynamicHeapArray<u8>* buffer, u8 quality, Error* error)
{
  // give enough space to avoid reallocs
  buffer->resize(image.GetWidth() * image.GetHeight() * 2);

  struct MemCallback
  {
    jpeg_destination_mgr mgr;
    DynamicHeapArray<u8>* buffer;
    size_t buffer_used;
  };

  MemCallback cb;
  cb.buffer = buffer;
  cb.buffer_used = 0;
  cb.mgr.next_output_byte = buffer->data();
  cb.mgr.free_in_buffer = buffer->size();
  cb.mgr.init_destination = [](j_compress_ptr cinfo) {};
  cb.mgr.empty_output_buffer = [](j_compress_ptr cinfo) -> boolean {
    MemCallback* cb = (MemCallback*)cinfo->dest;

    // double size
    cb->buffer_used = cb->buffer->size();
    cb->buffer->resize(cb->buffer->size() * 2);
    cb->mgr.next_output_byte = cb->buffer->data() + cb->buffer_used;
    cb->mgr.free_in_buffer = cb->buffer->size() - cb->buffer_used;
    return TRUE;
  };
  cb.mgr.term_destination = [](j_compress_ptr cinfo) {
    MemCallback* cb = (MemCallback*)cinfo->dest;

    // get final size
    cb->buffer->resize(cb->buffer->size() - cb->mgr.free_in_buffer);
  };

  return WrapJPEGCompress(image, quality, error, [&cb](jpeg_compress_struct& info) { info.dest = &cb.mgr; });
}

bool JPEGFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
{
  static constexpr u32 BUFFER_SIZE = 16384;

  struct FileCallback
  {
    jpeg_destination_mgr mgr;

    std::FILE* fp;
    std::unique_ptr<u8[]> buffer;
    Error* errptr;
    bool write_error;
  };

  FileCallback cb = {
    .mgr = {
      .next_output_byte = nullptr,
      .free_in_buffer = 0,
      .init_destination =
        [](j_compress_ptr cinfo) {
          FileCallback* cb = BASE_FROM_RECORD_FIELD(cinfo->dest, FileCallback, mgr);
          cb->mgr.next_output_byte = cb->buffer.get();
          cb->mgr.free_in_buffer = BUFFER_SIZE;
        },
      .empty_output_buffer = [](j_compress_ptr cinfo) -> boolean {
        FileCallback* cb = BASE_FROM_RECORD_FIELD(cinfo->dest, FileCallback, mgr);
        if (!cb->write_error)
        {
          if ((cb->write_error = (std::fwrite(cb->buffer.get(), 1, BUFFER_SIZE, cb->fp) != BUFFER_SIZE)))
            Error::SetErrno(cb->errptr, "fwrite() failed: ", errno);
        }

        cb->mgr.next_output_byte = cb->buffer.get();
        cb->mgr.free_in_buffer = BUFFER_SIZE;
        return TRUE;
      },
      .term_destination =
        [](j_compress_ptr cinfo) {
          FileCallback* cb = BASE_FROM_RECORD_FIELD(cinfo->dest, FileCallback, mgr);
          const size_t left = BUFFER_SIZE - cb->mgr.free_in_buffer;
          if (left > 0 && !cb->write_error)
          {
            if ((cb->write_error = (std::fwrite(cb->buffer.get(), 1, left, cb->fp) != left)))
              Error::SetErrno(cb->errptr, "fwrite() failed: ", errno);
          }
        },
    },
    .fp = fp,
    .buffer = std::make_unique<u8[]>(BUFFER_SIZE),
    .errptr = error,
    .write_error = false,
  };

  return (WrapJPEGCompress(image, quality, error, [&cb](jpeg_compress_struct& info) { info.dest = &cb.mgr; }) &&
          !cb.write_error);
}

bool WebPBufferLoader(Image* image, std::span<const u8> data, Error* error)
{
  int width, height;
  if (!WebPGetInfo(data.data(), data.size(), &width, &height) || width <= 0 || height <= 0)
  {
    Error::SetStringView(error, "WebPGetInfo() failed");
    return false;
  }

  image->Resize(static_cast<u32>(width), static_cast<u32>(height), ImageFormat::RGBA8, false);
  if (!WebPDecodeRGBAInto(data.data(), data.size(), image->GetPixels(), image->GetStorageSize(), image->GetPitch()))
  {
    Error::SetStringView(error, "WebPDecodeRGBAInto() failed");
    image->Invalidate();
    return false;
  }

  return true;
}

bool WebPBufferSaver(const Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error)
{
  u8* encoded_data;
  const size_t encoded_size =
    WebPEncodeRGBA(reinterpret_cast<const u8*>(image.GetPixels()), image.GetWidth(), image.GetHeight(),
                   image.GetPitch(), static_cast<float>(quality), &encoded_data);
  if (encoded_size == 0)
  {
    Error::SetStringFmt(error, "WebPEncodeRGBA() for {}x{} failed.", image.GetWidth(), image.GetHeight());
    return false;
  }

  data->resize(encoded_size);
  std::memcpy(data->data(), encoded_data, encoded_size);
  WebPFree(encoded_data);
  return true;
}

bool WebPFileLoader(Image* image, std::string_view filename, std::FILE* fp, Error* error)
{
  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(fp, error);
  if (!data.has_value())
    return false;

  return WebPBufferLoader(image, data->cspan(), error);
}

bool WebPFileSaver(const Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
{
  DynamicHeapArray<u8> buffer;
  if (!WebPBufferSaver(image, &buffer, quality, error))
    return false;

  if (std::fwrite(buffer.data(), buffer.size(), 1, fp) != 1)
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DDS Handler
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// From https://raw.githubusercontent.com/Microsoft/DirectXTex/master/DirectXTex/DDS.h
//
// This header defines constants and structures that are useful when parsing
// DDS files.  DDS files were originally designed to use several structures
// and constants that are native to DirectDraw and are defined in ddraw.h,
// such as DDSURFACEDESC2 and DDSCAPS2.  This file defines similar
// (compatible) constants and structures so that one can use DDS files
// without needing to include ddraw.h.
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=248926

#pragma pack(push, 1)

static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT
{
  uint32_t dwSize;
  uint32_t dwFlags;
  uint32_t dwFourCC;
  uint32_t dwRGBBitCount;
  uint32_t dwRBitMask;
  uint32_t dwGBitMask;
  uint32_t dwBBitMask;
  uint32_t dwABitMask;
};

#define DDS_FOURCC 0x00000004     // DDPF_FOURCC
#define DDS_RGB 0x00000040        // DDPF_RGB
#define DDS_RGBA 0x00000041       // DDPF_RGB | DDPF_ALPHAPIXELS
#define DDS_LUMINANCE 0x00020000  // DDPF_LUMINANCE
#define DDS_LUMINANCEA 0x00020001 // DDPF_LUMINANCE | DDPF_ALPHAPIXELS
#define DDS_ALPHA 0x00000002      // DDPF_ALPHA
#define DDS_PAL8 0x00000020       // DDPF_PALETTEINDEXED8
#define DDS_PAL8A 0x00000021      // DDPF_PALETTEINDEXED8 | DDPF_ALPHAPIXELS
#define DDS_BUMPDUDV 0x00080000   // DDPF_BUMPDUDV

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                                                                                 \
  ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | ((uint32_t)(uint8_t)(ch2) << 16) |                     \
   ((uint32_t)(uint8_t)(ch3) << 24))
#endif /* defined(MAKEFOURCC) */

#define DDS_HEADER_FLAGS_TEXTURE 0x00001007    // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
#define DDS_HEADER_FLAGS_MIPMAP 0x00020000     // DDSD_MIPMAPCOUNT
#define DDS_HEADER_FLAGS_VOLUME 0x00800000     // DDSD_DEPTH
#define DDS_HEADER_FLAGS_PITCH 0x00000008      // DDSD_PITCH
#define DDS_HEADER_FLAGS_LINEARSIZE 0x00080000 // DDSD_LINEARSIZE
#define DDS_MAX_TEXTURE_SIZE 32768

// Subset here matches D3D10_RESOURCE_DIMENSION and D3D11_RESOURCE_DIMENSION
enum DDS_RESOURCE_DIMENSION
{
  DDS_DIMENSION_TEXTURE1D = 2,
  DDS_DIMENSION_TEXTURE2D = 3,
  DDS_DIMENSION_TEXTURE3D = 4,
};

struct DDS_HEADER
{
  uint32_t dwSize;
  uint32_t dwFlags;
  uint32_t dwHeight;
  uint32_t dwWidth;
  uint32_t dwPitchOrLinearSize;
  uint32_t dwDepth; // only if DDS_HEADER_FLAGS_VOLUME is set in dwFlags
  uint32_t dwMipMapCount;
  uint32_t dwReserved1[11];
  DDS_PIXELFORMAT ddspf;
  uint32_t dwCaps;
  uint32_t dwCaps2;
  uint32_t dwCaps3;
  uint32_t dwCaps4;
  uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10
{
  uint32_t dxgiFormat;
  uint32_t resourceDimension;
  uint32_t miscFlag; // see DDS_RESOURCE_MISC_FLAG
  uint32_t arraySize;
  uint32_t miscFlags2; // see DDS_MISC_FLAGS2
};

#pragma pack(pop)

static_assert(sizeof(DDS_HEADER) == 124, "DDS Header size mismatch");
static_assert(sizeof(DDS_HEADER_DXT10) == 20, "DDS DX10 Extended Header size mismatch");

constexpr DDS_PIXELFORMAT DDSPF_A8R8G8B8 = {
  sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8R8G8B8 = {
  sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_A8B8G8R8 = {
  sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8B8G8R8 = {
  sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_R8G8B8 = {
  sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};

// End of Microsoft code from DDS.h.

static bool DDSPixelFormatMatches(const DDS_PIXELFORMAT& pf1, const DDS_PIXELFORMAT& pf2)
{
  return std::tie(pf1.dwSize, pf1.dwFlags, pf1.dwFourCC, pf1.dwRGBBitCount, pf1.dwRBitMask, pf1.dwGBitMask,
                  pf1.dwGBitMask, pf1.dwBBitMask,
                  pf1.dwABitMask) == std::tie(pf2.dwSize, pf2.dwFlags, pf2.dwFourCC, pf2.dwRGBBitCount, pf2.dwRBitMask,
                                              pf2.dwGBitMask, pf2.dwGBitMask, pf2.dwBBitMask, pf2.dwABitMask);
}

struct DDSLoadInfo
{
  u32 block_size = 1;
  u32 bytes_per_block = 4;
  u32 width = 0;
  u32 height = 0;
  u32 mip_count = 0;
  ImageFormat format = ImageFormat::RGBA8;
  s64 base_image_offset = 0;
  u32 base_image_size = 0;
  u32 base_image_pitch = 0;
  bool clear_alpha = false;
};

template<typename ReadFunction>
static bool ParseDDSHeader(const ReadFunction& RF, DDSLoadInfo* info, Error* error)
{
  u32 magic;
  if (!RF(&magic, sizeof(magic), error) || magic != DDS_MAGIC)
  {
    Error::AddPrefix(error, "Failed to read magic: ");
    return false;
  }

  DDS_HEADER header;
  u32 header_size = sizeof(header);
  if (!RF(&header, header_size, error) || header.dwSize < header_size)
  {
    Error::AddPrefix(error, "Failed to read header: ");
    return false;
  }

  // We should check for DDS_HEADER_FLAGS_TEXTURE here, but some tools don't seem
  // to set it (e.g. compressonator). But we can still validate the size.
  if (header.dwWidth == 0 || header.dwWidth >= DDS_MAX_TEXTURE_SIZE || header.dwHeight == 0 ||
      header.dwHeight >= DDS_MAX_TEXTURE_SIZE)
  {
    Error::SetStringFmt(error, "Size is invalid: {}x{}", header.dwWidth, header.dwHeight);
    return false;
  }

  // Image should be 2D.
  if (header.dwFlags & DDS_HEADER_FLAGS_VOLUME)
  {
    Error::SetStringView(error, "Volume textures are not supported.");
    return false;
  }

  // Presence of width/height fields is already tested by DDS_HEADER_FLAGS_TEXTURE.
  info->width = header.dwWidth;
  info->height = header.dwHeight;

  // Check for mip levels.
  if (header.dwFlags & DDS_HEADER_FLAGS_MIPMAP)
  {
    info->mip_count = header.dwMipMapCount;
    if (header.dwMipMapCount != 0)
    {
      info->mip_count = header.dwMipMapCount;
    }
    else
    {
      const u32 max_dim = Common::PreviousPow2(std::max(header.dwWidth, header.dwHeight));
      info->mip_count = (std::countr_zero(max_dim) + 1);
    }
  }
  else
  {
    info->mip_count = 1;
  }

  // Handle fourcc formats vs uncompressed formats.
  const bool has_fourcc = (header.ddspf.dwFlags & DDS_FOURCC) != 0;
  if (has_fourcc)
  {
    // Handle DX10 extension header.
    u32 dxt10_format = 0;
    if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'))
    {
      DDS_HEADER_DXT10 dxt10_header;
      if (!RF(&dxt10_header, sizeof(dxt10_header), error))
      {
        Error::AddPrefix(error, "Failed to read DXT10 header: ");
        return false;
      }

      // Can't handle array textures here. Doesn't make sense to use them, anyway.
      if (dxt10_header.resourceDimension != DDS_DIMENSION_TEXTURE2D || dxt10_header.arraySize != 1)
      {
        Error::SetStringView(error, "Only 2D textures are supported.");
        return false;
      }

      header_size += sizeof(dxt10_header);
      dxt10_format = dxt10_header.dxgiFormat;
    }

    if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '1') || dxt10_format == 71)
    {
      info->format = ImageFormat::BC1;
      info->block_size = 4;
      info->bytes_per_block = 8;
    }
    else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '2') ||
             header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '3') || dxt10_format == 74)
    {
      info->format = ImageFormat::BC2;
      info->block_size = 4;
      info->bytes_per_block = 16;
    }
    else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '4') ||
             header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '5') || dxt10_format == 77)
    {
      info->format = ImageFormat::BC3;
      info->block_size = 4;
      info->bytes_per_block = 16;
    }
    else if (dxt10_format == 98)
    {
      info->format = ImageFormat::BC7;
      info->block_size = 4;
      info->bytes_per_block = 16;
    }
    else
    {
      Error::SetStringFmt(error, "Unknown format with FOURCC 0x{:08X} / DXT10 format {}", header.ddspf.dwFourCC,
                          dxt10_format);
      return false;
    }
  }
  else
  {
    if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8R8G8B8))
    {
      info->format = ImageFormat::BGRA8;
    }
    else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8R8G8B8))
    {
      info->format = ImageFormat::BGRA8;
      info->clear_alpha = true;
    }
    else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8B8G8R8))
    {
      info->format = ImageFormat::RGBA8;
      info->clear_alpha = true;
    }
    else if (DDSPixelFormatMatches(header.ddspf, DDSPF_R8G8B8))
    {
      info->format = ImageFormat::BGR8;
      info->clear_alpha = true;
    }
    else if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8B8G8R8))
    {
      info->format = ImageFormat::RGBA8;
    }
    else
    {
      Error::SetStringFmt(error, "Unhandled format with FOURCC 0x{:08X}", header.ddspf.dwFourCC);
      return false;
    }

    // All these formats are RGBA, just with byte swapping.
    info->block_size = 1;
    info->bytes_per_block = header.ddspf.dwRGBBitCount / 8;
  }

  // Mip levels smaller than the block size are padded to multiples of the block size.
  const u32 blocks_wide = Common::AlignUpPow2(info->width, info->block_size) / info->block_size;
  const u32 blocks_high = Common::AlignUpPow2(info->height, info->block_size) / info->block_size;

  // Pitch can be specified in the header, otherwise we can derive it from the dimensions. For
  // compressed formats, both DDS_HEADER_FLAGS_LINEARSIZE and DDS_HEADER_FLAGS_PITCH should be
  // set. See https://msdn.microsoft.com/en-us/library/windows/desktop/bb943982(v=vs.85).aspx
  if (header.dwFlags & DDS_HEADER_FLAGS_PITCH && header.dwFlags & DDS_HEADER_FLAGS_LINEARSIZE)
  {
    // Convert pitch (in bytes) to texels/row length.
    if (header.dwPitchOrLinearSize < info->bytes_per_block)
    {
      // Likely a corrupted or invalid file.
      Error::SetStringFmt(error, "Invalid pitch: {}", header.dwPitchOrLinearSize);
      return false;
    }

    info->base_image_pitch = header.dwPitchOrLinearSize;
    info->base_image_size = info->base_image_pitch * blocks_high;
  }
  else
  {
    // Assume no padding between rows of blocks.
    info->base_image_pitch = blocks_wide * info->bytes_per_block;
    info->base_image_size = info->base_image_pitch * blocks_high;
  }

  info->base_image_offset = sizeof(magic) + header_size;

#if 0
  // D3D11 cannot handle block compressed textures where the first mip level is not a multiple of the block size.
  if (mip_level == 0 && info.block_size > 1 && ((width % info.block_size) != 0 || (height % info.block_size) != 0))
  {
    Error::SetStringFmt(error,
                        "Invalid dimensions for DDS texture. For compressed textures of this format, "
                        "the width/height of the first mip level must be a multiple of {}.",
                        info.block_size);
    return false;
  }
#endif

  return true;
}

bool DDSFileLoader(Image* image, std::string_view path, std::FILE* fp, Error* error)
{
  const auto header_reader = [fp](void* buffer, size_t size, Error* error) {
    if (std::fread(buffer, size, 1, fp) == 1)
      return true;

    Error::SetErrno(error, "fread() failed: ", errno);
    return false;
  };

  DDSLoadInfo info;
  if (!ParseDDSHeader(header_reader, &info, error))
    return false;

  // always load the base image
  if (!FileSystem::FSeek64(fp, info.base_image_offset, SEEK_SET, error))
    return false;

  image->Resize(info.width, info.height, info.format, false);
  const u32 blocks = image->GetBlocksHigh();
  if (image->GetPitch() != info.base_image_pitch)
  {
    for (u32 y = 0; y < blocks; y++)
    {
      if (std::fread(image->GetRowPixels(y), info.base_image_pitch, 1, fp) != 1)
      {
        Error::SetErrno(error, "fread() failed: ", errno);
        return false;
      }
    }
  }
  else
  {
    if (std::fread(image->GetPixels(), info.base_image_pitch * blocks, 1, fp) != 1)
    {
      Error::SetErrno(error, "fread() failed: ", errno);
      return false;
    }
  }

  if (info.clear_alpha)
    image->SetAllPixelsOpaque();

  return true;
}

bool DDSBufferLoader(Image* image, std::span<const u8> data, Error* error)
{
  size_t data_pos = 0;
  const auto header_reader = [&data, &data_pos](void* buffer, size_t size, Error* error) {
    if ((data_pos + size) > data.size())
    {
      Error::SetStringView(error, "Buffer does not contain sufficient data.");
      return false;
    }

    std::memcpy(buffer, &data[data_pos], size);
    data_pos += size;
    return true;
  };

  DDSLoadInfo info;
  if (!ParseDDSHeader(header_reader, &info, error))
    return false;

  if ((static_cast<u64>(info.base_image_offset) + info.base_image_size) > data.size())
  {
    Error::SetStringFmt(error, "Buffer does not contain complete base image.");
    return false;
  }

  image->SetPixels(info.width, info.height, info.format, &data[static_cast<size_t>(info.base_image_offset)],
                   info.base_image_pitch);

  if (info.clear_alpha)
    image->SetAllPixelsOpaque();

  return true;
}
