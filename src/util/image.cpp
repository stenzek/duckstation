// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "image.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include "lunasvg_c.h"

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

// clang-format off
#ifdef _MSC_VER
#pragma warning(disable : 4611) // warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable
#endif
// clang-format on

LOG_CHANNEL(Image);

static bool PNGBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error);
static bool PNGBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool PNGFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool PNGFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

static bool JPEGBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error);
static bool JPEGBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool JPEGFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool JPEGFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

static bool WebPBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error);
static bool WebPBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error);
static bool WebPFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error);
static bool WebPFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error);

struct FormatHandler
{
  const char* extension;
  bool (*buffer_loader)(RGBA8Image*, std::span<const u8>, Error*);
  bool (*buffer_saver)(const RGBA8Image&, DynamicHeapArray<u8>*, u8, Error*);
  bool (*file_loader)(RGBA8Image*, std::string_view, std::FILE*, Error*);
  bool (*file_saver)(const RGBA8Image&, std::string_view, std::FILE*, u8, Error*);
};

static constexpr FormatHandler s_format_handlers[] = {
  {"png", PNGBufferLoader, PNGBufferSaver, PNGFileLoader, PNGFileSaver},
  {"jpg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
  {"jpeg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
  {"webp", WebPBufferLoader, WebPBufferSaver, WebPFileLoader, WebPFileSaver},
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

RGBA8Image::RGBA8Image() = default;

RGBA8Image::RGBA8Image(const RGBA8Image& copy) : Image(copy)
{
}

RGBA8Image::RGBA8Image(u32 width, u32 height, const u32* pixels) : Image(width, height, pixels)
{
}

RGBA8Image::RGBA8Image(RGBA8Image&& move) : Image(move)
{
}

RGBA8Image::RGBA8Image(u32 width, u32 height) : Image(width, height)
{
}

RGBA8Image::RGBA8Image(u32 width, u32 height, std::vector<u32> pixels) : Image(width, height, std::move(pixels))
{
}

RGBA8Image& RGBA8Image::operator=(const RGBA8Image& copy)
{
  Image<u32>::operator=(copy);
  return *this;
}

RGBA8Image& RGBA8Image::operator=(RGBA8Image&& move)
{
  Image<u32>::operator=(move);
  return *this;
}

bool RGBA8Image::LoadFromFile(const char* filename, Error* error /* = nullptr */)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb", error);
  if (!fp)
    return false;

  return LoadFromFile(filename, fp.get(), error);
}

bool RGBA8Image::SaveToFile(const char* filename, u8 quality /* = DEFAULT_SAVE_QUALITY */,
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

bool RGBA8Image::LoadFromFile(std::string_view filename, std::FILE* fp, Error* error /* = nullptr */)
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

bool RGBA8Image::LoadFromBuffer(std::string_view filename, std::span<const u8> data, Error* error /* = nullptr */)
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

bool RGBA8Image::RasterizeSVG(const std::span<const u8> data, u32 width, u32 height, Error* error)
{
  if (width == 0 || height == 0)
  {
    Error::SetStringFmt(error, "Invalid dimensions: {}x{}", width, height);
    return false;
  }

  std::unique_ptr<lunasvg_document, void (*)(lunasvg_document*)> doc(
    lunasvg_document_load_from_data(data.data(), data.size()), lunasvg_document_destroy);
  if (!doc)
  {
    Error::SetStringView(error, "lunasvg_document_load_from_data() failed");
    return false;
  }

  std::unique_ptr<lunasvg_bitmap, void (*)(lunasvg_bitmap*)> bitmap(
    lunasvg_document_render_to_bitmap(doc.get(), width, height, 0), lunasvg_bitmap_destroy);
  if (!bitmap)
  {
    Error::SetStringView(error, "lunasvg_document_render_to_bitmap() failed");
    return false;
  }

  SetPixels(width, height, lunasvg_bitmap_data(bitmap.get()), lunasvg_bitmap_stride(bitmap.get()));
  SwapBGRAToRGBA(m_pixels.data(), m_width, m_height, GetPitch());
  return true;
}

bool RGBA8Image::SaveToFile(std::string_view filename, std::FILE* fp, u8 quality /* = DEFAULT_SAVE_QUALITY */,
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

std::optional<DynamicHeapArray<u8>> RGBA8Image::SaveToBuffer(std::string_view filename,
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

void RGBA8Image::SwapBGRAToRGBA(void* pixels, u32 width, u32 height, u32 pitch)
{
#ifdef GSVECTOR_HAS_FAST_INT_SHUFFLE8
  constexpr u32 pixels_per_vec = sizeof(GSVector4i) / 4;
  const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);
#endif

  u8* pixels_ptr = static_cast<u8*>(pixels);
  for (u32 y = 0; y < height; y++)
  {
    u8* row_pixels_ptr = pixels_ptr;
    u32 x;

#ifdef GSVECTOR_HAS_FAST_INT_SHUFFLE8
    for (x = 0; x < aligned_width; x += pixels_per_vec)
    {
      static constexpr GSVector4i mask = GSVector4i::cxpr8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
      GSVector4i::store<false>(row_pixels_ptr, GSVector4i::load<false>(row_pixels_ptr).shuffle8(mask));
      row_pixels_ptr += sizeof(GSVector4i);
    }
#endif

    for (; x < width; x++)
    {
      u32 pixel;
      std::memcpy(&pixel, row_pixels_ptr, sizeof(pixel));
      pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
      std::memcpy(row_pixels_ptr, &pixel, sizeof(pixel));
      row_pixels_ptr += sizeof(pixel);
    }

    pixels_ptr += pitch;
  }
}

#if 0

void RGBA8Image::Resize(u32 new_width, u32 new_height)
{
  if (m_width == new_width && m_height == new_height)
    return;

  std::vector<u32> resized_texture_data(new_width * new_height);
  u32 resized_texture_stride = sizeof(u32) * new_width;
  if (!stbir_resize_uint8(reinterpret_cast<u8*>(m_pixels.data()), m_width, m_height, GetPitch(),
                          reinterpret_cast<u8*>(resized_texture_data.data()), new_width, new_height,
                          resized_texture_stride, 4))
  {
    Panic("stbir_resize_uint8 failed");
    return;
  }

  SetPixels(new_width, new_height, std::move(resized_texture_data));
}

void RGBA8Image::Resize(const RGBA8Image* src_image, u32 new_width, u32 new_height)
{
  if (src_image->m_width == new_width && src_image->m_height == new_height)
  {
    SetPixels(src_image->m_width, src_image->m_height, src_image->m_pixels.data());
    return;
  }

  SetSize(new_width, new_height);
  if (!stbir_resize_uint8(reinterpret_cast<const u8*>(src_image->m_pixels.data()), src_image->m_width,
                          src_image->m_height, src_image->GetPitch(), reinterpret_cast<u8*>(m_pixels.data()), new_width,
                          new_height, GetPitch(), 4))
  {
    Panic("stbir_resize_uint8 failed");
    return;
  }
}

#endif

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

static bool PNGCommonLoader(RGBA8Image* image, png_structp png_ptr, png_infop info_ptr, std::vector<u32>& new_data,
                            std::vector<png_bytep>& row_pointers)
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

  new_data.resize(width * height);
  row_pointers.reserve(height);
  for (u32 y = 0; y < height; y++)
    row_pointers.push_back(reinterpret_cast<png_bytep>(new_data.data() + y * width));

  png_read_image(png_ptr, row_pointers.data());
  image->SetPixels(width, height, std::move(new_data));
  return true;
}

bool PNGFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error)
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

  std::vector<u32> new_data;
  std::vector<png_bytep> row_pointers;

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_set_read_fn(png_ptr, fp, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
    std::FILE* fp = static_cast<std::FILE*>(png_get_io_ptr(png_ptr));
    if (std::fread(data_ptr, size, 1, fp) != 1)
      png_error(png_ptr, "fread() failed");
  });

  return PNGCommonLoader(image, png_ptr, info_ptr, new_data, row_pointers);
}

bool PNGBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error)
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

  std::vector<u32> new_data;
  std::vector<png_bytep> row_pointers;

  PNGSetErrorFunction(png_ptr, error);
  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

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

  return PNGCommonLoader(image, png_ptr, info_ptr, new_data, row_pointers);
}

static void PNGSaveCommon(const RGBA8Image& image, png_structp png_ptr, png_infop info_ptr, u8 quality)
{
  png_set_compression_level(png_ptr, std::clamp(quality / 10, 0, 9));
  png_set_IHDR(png_ptr, info_ptr, image.GetWidth(), image.GetHeight(), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png_ptr, info_ptr);

  for (u32 y = 0; y < image.GetHeight(); ++y)
    png_write_row(png_ptr, (png_bytep)image.GetRowPixels(y));

  png_write_end(png_ptr, nullptr);
}

bool PNGFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
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

bool PNGBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error)
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

  JPEGErrorHandler()
  {
    jpeg_std_error(&err);
    err.error_exit = &ErrorExit;
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
static bool WrapJPEGDecompress(RGBA8Image* image, Error* error, T setup_func)
{
  std::vector<u8> scanline;
  jpeg_decompress_struct info = {};

  // NOTE: Be **very** careful not to allocate memory after calling this function.
  // It won't get freed, because fastjmp does not unwind the stack.
  JPEGErrorHandler errhandler;
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

  image->SetSize(info.image_width, info.image_height);
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
    u32* dst_ptr = image->GetRowPixels(y);
    for (u32 x = 0; x < info.image_width; x++)
    {
      *(dst_ptr++) =
        (ZeroExtend32(src_ptr[0]) | (ZeroExtend32(src_ptr[1]) << 8) | (ZeroExtend32(src_ptr[2]) << 16) | 0xFF000000u);
      src_ptr += 3;
    }
  }

  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  return result;
}

bool JPEGBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error)
{
  return WrapJPEGDecompress(image, error, [data](jpeg_decompress_struct& info) {
    jpeg_mem_src(&info, static_cast<const unsigned char*>(data.data()), static_cast<unsigned long>(data.size()));
  });
}

bool JPEGFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error)
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
static bool WrapJPEGCompress(const RGBA8Image& image, u8 quality, Error* error, T setup_func)
{
  std::vector<u8> scanline;
  jpeg_compress_struct info = {};

  // NOTE: Be **very** careful not to allocate memory after calling this function.
  // It won't get freed, because fastjmp does not unwind the stack.
  JPEGErrorHandler errhandler;
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
    const u32* src_ptr = image.GetRowPixels(y);
    for (u32 x = 0; x < info.image_width; x++)
    {
      const u32 rgba = *(src_ptr++);
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

bool JPEGBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* buffer, u8 quality, Error* error)
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

bool JPEGFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
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

bool WebPBufferLoader(RGBA8Image* image, std::span<const u8> data, Error* error)
{
  int width, height;
  if (!WebPGetInfo(data.data(), data.size(), &width, &height) || width <= 0 || height <= 0)
  {
    Error::SetStringView(error, "WebPGetInfo() failed");
    return false;
  }

  std::vector<u32> pixels;
  pixels.resize(static_cast<u32>(width) * static_cast<u32>(height));
  if (!WebPDecodeRGBAInto(data.data(), data.size(), reinterpret_cast<u8*>(pixels.data()), sizeof(u32) * pixels.size(),
                          sizeof(u32) * static_cast<u32>(width)))
  {
    Error::SetStringView(error, "WebPDecodeRGBAInto() failed");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), std::move(pixels));
  return true;
}

bool WebPBufferSaver(const RGBA8Image& image, DynamicHeapArray<u8>* data, u8 quality, Error* error)
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

bool WebPFileLoader(RGBA8Image* image, std::string_view filename, std::FILE* fp, Error* error)
{
  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(fp, error);
  if (!data.has_value())
    return false;

  return WebPBufferLoader(image, data->cspan(), error);
}

bool WebPFileSaver(const RGBA8Image& image, std::string_view filename, std::FILE* fp, u8 quality, Error* error)
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