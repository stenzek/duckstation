// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "animated_image.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include <png.h>

// clang-format off
#ifdef _MSC_VER
#pragma warning(disable : 4611) // warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable
#endif
// clang-format on

LOG_CHANNEL(Image);

static bool PNGBufferLoader(AnimatedImage* image, std::span<const u8> data, Error* error);
static bool PNGBufferSaver(const AnimatedImage& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool PNGFileLoader(AnimatedImage* image, std::string_view filename, std::FILE* fp, Error* error);
static bool PNGFileSaver(const AnimatedImage& image, std::string_view filename, std::FILE* fp, u8 quality,
                         Error* error);

namespace {
struct FormatHandler
{
  const char* extension;
  bool (*buffer_loader)(AnimatedImage*, std::span<const u8>, Error*);
  bool (*buffer_saver)(const AnimatedImage&, DynamicHeapArray<u8>*, u8, Error*);
  bool (*file_loader)(AnimatedImage*, std::string_view, std::FILE*, Error*);
  bool (*file_saver)(const AnimatedImage&, std::string_view, std::FILE*, u8, Error*);
};
} // namespace

static constexpr FormatHandler s_format_handlers[] = {
  {"png", PNGBufferLoader, PNGBufferSaver, PNGFileLoader, PNGFileSaver},
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

AnimatedImage::AnimatedImage() = default;

AnimatedImage::AnimatedImage(const AnimatedImage& copy)
  : m_width(copy.m_width), m_height(copy.m_height), m_frame_size(copy.m_frame_size), m_frames(copy.m_frames),
    m_pixels(copy.m_pixels), m_frame_delay(copy.m_frame_delay)
{
}

AnimatedImage::AnimatedImage(AnimatedImage&& move)
{
  m_width = std::exchange(move.m_width, 0);
  m_height = std::exchange(move.m_height, 0);
  m_frame_size = std::exchange(move.m_frame_size, 0);
  m_frames = std::exchange(move.m_frames, 0);
  m_pixels = std::move(move.m_pixels);
  m_frame_delay = std::move(move.m_frame_delay);
}

AnimatedImage::AnimatedImage(u32 width, u32 height, u32 frames, const FrameDelay& default_delay)
  : m_width(width), m_height(height), m_frame_size(width * height), m_frames(frames), m_pixels(frames * width * height),
    m_frame_delay(frames)
{
  for (FrameDelay& delay : m_frame_delay)
    delay = default_delay;
}

void AnimatedImage::Resize(u32 new_width, u32 new_height, u32 num_frames, const FrameDelay& default_delay,
                           bool preserve)
{
  DebugAssert(new_width > 0 && new_height > 0 && num_frames > 0);
  if (m_width == new_width && m_height == new_height && num_frames == m_frames)
    return;

  if (!preserve)
    m_pixels.deallocate();

  const u32 new_frame_size = new_width * new_height;

  PixelStorage new_pixels;
  new_pixels.resize(new_frame_size * num_frames);
  std::memset(new_pixels.data(), 0, new_pixels.size() * sizeof(u32));
  m_frame_delay.resize(num_frames);
  if (preserve && !m_pixels.empty())
  {
    const u32 copy_frames = std::min(num_frames, m_frames);
    for (u32 i = 0; i < copy_frames; i++)
    {
      StringUtil::StrideMemCpy(new_pixels.data() + i * new_frame_size, new_width * sizeof(u32),
                               m_pixels.data() + i * m_frame_size, m_width * sizeof(u32),
                               std::min(new_width, m_width) * sizeof(u32), std::min(new_height, m_height));
    }

    for (u32 i = m_frames; i < num_frames; i++)
      m_frame_delay[i] = default_delay;
  }

  m_width = new_width;
  m_height = new_height;
  m_frame_size = new_frame_size;
  m_frames = num_frames;
  m_pixels = std::move(new_pixels);
}

AnimatedImage& AnimatedImage::operator=(const AnimatedImage& copy)
{
  m_width = copy.m_width;
  m_height = copy.m_height;
  m_frame_size = copy.m_frame_size;
  m_frames = copy.m_frames;
  m_pixels = copy.m_pixels;
  m_frame_delay = copy.m_frame_delay;
  return *this;
}

AnimatedImage& AnimatedImage::operator=(AnimatedImage&& move)
{
  m_width = std::exchange(move.m_width, 0);
  m_height = std::exchange(move.m_height, 0);
  m_frame_size = std::exchange(move.m_frame_size, 0);
  m_frames = std::exchange(move.m_frames, 0);
  m_pixels = std::move(move.m_pixels);
  m_frame_delay = std::move(move.m_frame_delay);
  return *this;
}

u32 AnimatedImage::CalculatePitch(u32 width, u32 height)
{
  return width * sizeof(u32);
}

std::span<const AnimatedImage::PixelType> AnimatedImage::GetPixelsSpan(u32 frame) const
{
  DebugAssert(frame < m_frames);
  return m_pixels.cspan(frame * m_frame_size, m_frame_size);
}

std::span<AnimatedImage::PixelType> AnimatedImage::GetPixelsSpan(u32 frame)
{
  DebugAssert(frame < m_frames);
  return m_pixels.span(frame * m_frame_size, m_frame_size);
}

void AnimatedImage::Clear()
{
  std::memset(m_pixels.data(), 0, m_pixels.size_bytes());
}

void AnimatedImage::Invalidate()
{
  m_width = 0;
  m_height = 0;
  m_frame_size = 0;
  m_frames = 0;
  m_pixels.deallocate();
  m_frame_delay.deallocate();
}

void AnimatedImage::SetPixels(u32 frame, const void* pixels, u32 pitch)
{
  DebugAssert(frame < m_frames);
  StringUtil::StrideMemCpy(GetPixels(frame), m_width * sizeof(u32), pixels, pitch, m_width * sizeof(u32), m_height);
}

void AnimatedImage::SetDelay(u32 frame, const FrameDelay& delay)
{
  DebugAssert(frame < m_frames);
  m_frame_delay[frame] = delay;
}

AnimatedImage::PixelStorage AnimatedImage::TakePixels()
{
  m_width = 0;
  m_height = 0;
  m_frame_size = 0;
  m_frames = 0;
  return std::move(m_pixels);
}

bool AnimatedImage::LoadFromFile(const char* filename, Error* error /* = nullptr */)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb", error);
  if (!fp)
    return false;

  return LoadFromFile(filename, fp.get(), error);
}

bool AnimatedImage::SaveToFile(const char* filename, u8 quality /* = DEFAULT_SAVE_QUALITY */,
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

bool AnimatedImage::LoadFromFile(std::string_view filename, std::FILE* fp, Error* error /* = nullptr */)
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

bool AnimatedImage::LoadFromBuffer(std::string_view filename, std::span<const u8> data, Error* error /* = nullptr */)
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

bool AnimatedImage::SaveToFile(std::string_view filename, std::FILE* fp, u8 quality /* = DEFAULT_SAVE_QUALITY */,
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

std::optional<DynamicHeapArray<u8>> AnimatedImage::SaveToBuffer(std::string_view filename,
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

static bool PNGCommonLoader(AnimatedImage* image, png_structp png_ptr, png_infop info_ptr)
{
  png_read_info(png_ptr, info_ptr);

  const u32 width = png_get_image_width(png_ptr, info_ptr);
  const u32 height = png_get_image_height(png_ptr, info_ptr);
  const u32 num_frames = png_get_num_frames(png_ptr, info_ptr);
  const png_byte color_type = png_get_color_type(png_ptr, info_ptr);
  const png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

  if (num_frames == 0)
    png_error(png_ptr, "Image has zero frames");

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

  DebugAssert(num_frames > 0);
  image->Resize(width, height, num_frames, {1, 10}, false);
  if (num_frames > 1)
  {
    for (u32 i = 0; i < num_frames; i++)
    {
      png_read_frame_head(png_ptr, info_ptr);

      const u32 frame_width = png_get_next_frame_width(png_ptr, info_ptr);
      const u32 frame_height = png_get_next_frame_height(png_ptr, info_ptr);
      if (frame_width != width || frame_height != height)
        png_error(png_ptr, "Frame size does not match image size");

      const u16 delay_num = static_cast<u16>(png_get_next_frame_delay_num(png_ptr, info_ptr));
      const u16 delay_den = static_cast<u16>(png_get_next_frame_delay_den(png_ptr, info_ptr));
      image->SetDelay(i, {delay_num, std::max<u16>(delay_den, 1)});

      // TODO: blending/compose/etc.
      const int num_passes = png_set_interlace_handling(png_ptr);
      for (int pass = 0; pass < num_passes; pass++)
      {
        for (u32 y = 0; y < height; y++)
          png_read_row(png_ptr, reinterpret_cast<png_bytep>(image->GetRowPixels(i, y)), nullptr);
      }
    }
  }
  else
  {
    const int num_passes = png_set_interlace_handling(png_ptr);
    for (int pass = 0; pass < num_passes; pass++)
    {
      for (u32 y = 0; y < height; y++)
        png_read_row(png_ptr, reinterpret_cast<png_bytep>(image->GetRowPixels(0, y)), nullptr);
    }
  }

  return true;
}

bool PNGFileLoader(AnimatedImage* image, std::string_view filename, std::FILE* fp, Error* error)
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

  return PNGCommonLoader(image, png_ptr, info_ptr);
}

bool PNGBufferLoader(AnimatedImage* image, std::span<const u8> data, Error* error)
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

  return PNGCommonLoader(image, png_ptr, info_ptr);
}

static void PNGSaveCommon(const AnimatedImage& image, png_structp png_ptr, png_infop info_ptr, u8 quality)
{
  png_set_compression_level(png_ptr, std::clamp(quality / 10, 0, 9));
  png_set_IHDR(png_ptr, info_ptr, image.GetWidth(), image.GetHeight(), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  const u32 width = image.GetWidth();
  const u32 height = image.GetHeight();
  const u32 frames = image.GetFrames();
  if (frames > 1)
  {
    if (!png_set_acTL(png_ptr, info_ptr, frames, 0))
      png_error(png_ptr, "png_set_acTL() failed");

    png_write_info(png_ptr, info_ptr);

    for (u32 i = 0; i < frames; i++)
    {
      const AnimatedImage::FrameDelay& fd = image.GetFrameDelay(i);
      png_write_frame_head(png_ptr, info_ptr, width, height, 0, 0, fd.numerator, fd.denominator, PNG_DISPOSE_OP_NONE,
                           PNG_BLEND_OP_SOURCE);

      for (u32 y = 0; y < height; ++y)
        png_write_row(png_ptr, (png_bytep)image.GetRowPixels(i, y));

      png_write_frame_tail(png_ptr, info_ptr);
    }
  }
  else
  {
    // only one frame
    png_write_info(png_ptr, info_ptr);
    for (u32 y = 0; y < height; ++y)
      png_write_row(png_ptr, (png_bytep)image.GetRowPixels(0, y));
  }

  png_write_end(png_ptr, nullptr);
}

bool PNGFileSaver(const AnimatedImage& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
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

bool PNGBufferSaver(const AnimatedImage& image, DynamicHeapArray<u8>* data, u8 quality, Error* error)
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
    png_ptr, &iodata,
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
