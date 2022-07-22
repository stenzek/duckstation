#include "image.h"
#include "byte_stream.h"
#include "file_system.h"
#include "log.h"
#include "path.h"
#include "scope_guard.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "string_util.h"
Log_SetChannel(Image);

using namespace Common;

#if 0
static bool PNGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size);
static bool PNGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, int quality);
static bool PNGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp);
static bool PNGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality);

static bool JPEGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size);
static bool JPEGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, int quality);
static bool JPEGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp);
static bool JPEGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality);
#endif

static bool STBBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size);
static bool STBFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp);
static bool STBBufferSaverPNG(const RGBA8Image& image, std::vector<u8>* buffer, int quality);
static bool STBBufferSaverJPEG(const RGBA8Image& image, std::vector<u8>* buffer, int quality);
static bool STBFileSaverPNG(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality);
static bool STBFileSaverJPEG(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality);

struct FormatHandler
{
  const char* extension;
  bool (*buffer_loader)(RGBA8Image*, const void*, size_t);
  bool (*buffer_saver)(const RGBA8Image&, std::vector<u8>*, int);
  bool (*file_loader)(RGBA8Image*, const char*, std::FILE*);
  bool (*file_saver)(const RGBA8Image&, const char*, std::FILE*, int);
};

static constexpr FormatHandler s_format_handlers[] = {
#if 0
  {"png", PNGBufferLoader, PNGBufferSaver, PNGFileLoader, PNGFileSaver},
  {"jpg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
  {"jpeg", JPEGBufferLoader, JPEGBufferSaver, JPEGFileLoader, JPEGFileSaver},
#else
  {"png", STBBufferLoader, STBBufferSaverPNG, STBFileLoader, STBFileSaverPNG},
  {"jpg", STBBufferLoader, STBBufferSaverJPEG, STBFileLoader, STBFileSaverJPEG},
  {"jpeg", STBBufferLoader, STBBufferSaverJPEG, STBFileLoader, STBFileSaverJPEG},
#endif
};

static const FormatHandler* GetFormatHandler(const std::string_view& extension)
{
  for (const FormatHandler& handler : s_format_handlers)
  {
    if (StringUtil::Strncasecmp(extension.data(), handler.extension, extension.size()))
      return &handler;
  }

  return nullptr;
}

RGBA8Image::RGBA8Image() = default;

RGBA8Image::RGBA8Image(const RGBA8Image& copy) : Image(copy) {}

RGBA8Image::RGBA8Image(u32 width, u32 height, const u32* pixels) : Image(width, height, pixels) {}

RGBA8Image::RGBA8Image(RGBA8Image&& move) : Image(move) {}

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

bool RGBA8Image::LoadFromFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return false;

  return LoadFromFile(filename, fp.get());
}

bool RGBA8Image::SaveToFile(const char* filename, int quality) const
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  if (SaveToFile(filename, fp.get(), quality))
    return true;

  // save failed
  fp.reset();
  FileSystem::DeleteFile(filename);
  return false;
}

bool RGBA8Image::LoadFromFile(const char* filename, std::FILE* fp)
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_loader)
  {
    Log_ErrorPrintf("Unknown extension '%.*s'", static_cast<int>(extension.size()), extension.data());
    return false;
  }

  return handler->file_loader(this, filename, fp);
}

bool RGBA8Image::LoadFromBuffer(const char* filename, const void* buffer, size_t buffer_size)
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->buffer_loader)
  {
    Log_ErrorPrintf("Unknown extension '%.*s'", static_cast<int>(extension.size()), extension.data());
    return false;
  }

  return handler->buffer_loader(this, buffer, buffer_size);
}

bool RGBA8Image::SaveToFile(const char* filename, std::FILE* fp, int quality) const
{
  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_saver)
  {
    Log_ErrorPrintf("Unknown extension '%.*s'", static_cast<int>(extension.size()), extension.data());
    return false;
  }

  if (!handler->file_saver(*this, filename, fp, quality))
    return false;

  return (std::fflush(fp) == 0);
}

std::optional<std::vector<u8>> RGBA8Image::SaveToBuffer(const char* filename, int quality) const
{
  std::optional<std::vector<u8>> ret;

  const std::string_view extension(Path::GetExtension(filename));
  const FormatHandler* handler = GetFormatHandler(extension);
  if (!handler || !handler->file_saver)
  {
    Log_ErrorPrintf("Unknown extension '%.*s'", static_cast<int>(extension.size()), extension.data());
    return ret;
  }

  ret = std::vector<u8>();
  if (!handler->buffer_saver(*this, &ret.value(), quality))
    ret.reset();

  return ret;
}

#if 0

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

bool PNGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp)
{
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr)
    return false;

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() { png_destroy_read_struct(&png_ptr, &info_ptr, nullptr); });

  std::vector<u32> new_data;
  std::vector<png_bytep> row_pointers;

  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_init_io(png_ptr, fp);
  return PNGCommonLoader(image, png_ptr, info_ptr, new_data, row_pointers);
}

bool PNGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size)
{
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr)
    return false;

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return false;
  }

  ScopedGuard cleanup([&png_ptr, &info_ptr]() { png_destroy_read_struct(&png_ptr, &info_ptr, nullptr); });

  std::vector<u32> new_data;
  std::vector<png_bytep> row_pointers;

  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  struct IOData
  {
    const u8* buffer;
    size_t buffer_size;
    size_t buffer_pos;
  };
  IOData data = {static_cast<const u8*>(buffer), buffer_size, 0};

  png_set_read_fn(png_ptr, &data, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
    IOData* data = static_cast<IOData*>(png_get_io_ptr(png_ptr));
    const size_t read_size = std::min<size_t>(data->buffer_size - data->buffer_pos, size);
    if (read_size > 0)
    {
      std::memcpy(data_ptr, data->buffer + data->buffer_pos, read_size);
      data->buffer_pos += read_size;
    }
  });

  return PNGCommonLoader(image, png_ptr, info_ptr, new_data, row_pointers);
}

static void PNGSaveCommon(const RGBA8Image& image, png_structp png_ptr, png_infop info_ptr, int quality)
{
  png_set_compression_level(png_ptr, std::clamp(quality / 10, 0, 9));
  png_set_IHDR(png_ptr, info_ptr, image.GetWidth(), image.GetHeight(), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png_ptr, info_ptr);

  for (u32 y = 0; y < image.GetHeight(); ++y)
    png_write_row(png_ptr, (png_bytep)image.GetRowPixels(y));

  png_write_end(png_ptr, nullptr);
}

bool PNGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality)
{
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = nullptr;
  if (!png_ptr)
    return false;

  ScopedGuard cleanup([&png_ptr, &info_ptr]() {
    if (png_ptr)
      png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
  });

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    return false;

  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_set_write_fn(
    png_ptr, fp,
    [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
      if (std::fwrite(data_ptr, size, 1, static_cast<std::FILE*>(png_get_io_ptr(png_ptr))) != 1)
        png_error(png_ptr, "file write error");
    },
    [](png_structp png_ptr) {});

  PNGSaveCommon(image, png_ptr, info_ptr, quality);
  return true;
}

bool PNGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, int quality)
{
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = nullptr;
  if (!png_ptr)
    return false;

  ScopedGuard cleanup([&png_ptr, &info_ptr]() {
    if (png_ptr)
      png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
  });

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    return false;

  buffer->reserve(image.GetWidth() * image.GetHeight() * 2);

  if (setjmp(png_jmpbuf(png_ptr)))
    return false;

  png_set_write_fn(
    png_ptr, buffer,
    [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
      std::vector<u8>* buffer = static_cast<std::vector<u8>*>(png_get_io_ptr(png_ptr));
      const size_t old_pos = buffer->size();
      buffer->resize(old_pos + size);
      std::memcpy(buffer->data() + old_pos, data_ptr, size);
    },
    [](png_structp png_ptr) {});

  PNGSaveCommon(image, png_ptr, info_ptr, quality);
  return true;
}

bool JPEGBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size)
{
  int width, height, file_comps;
  u8* data = jpgd::decompress_jpeg_image_from_memory(static_cast<const u8*>(buffer), static_cast<int>(buffer_size),
                                                     &width, &height, &file_comps, 4, 0);
  if (!data)
  {
    Console.Error("jpgd::decompress_jpeg_image_from_memory() failed");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(data));
  std::free(data);
  return true;
}

bool JPEGFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp)
{
  class FileStream : public jpgd::jpeg_decoder_stream
  {
    std::FILE* m_fp;
    bool m_error_flag = false;
    bool m_eof_flag = false;

  public:
    explicit FileStream(std::FILE* fp_) : m_fp(fp_) {}

    int read(jpgd::uint8* pBuf, int max_bytes_to_read, bool* pEOF_flag) override
    {
      if (m_eof_flag)
      {
        *pEOF_flag = true;
        return 0;
      }

      if (m_error_flag)
        return -1;

      int bytes_read = static_cast<int>(std::fread(pBuf, 1, max_bytes_to_read, m_fp));
      if (bytes_read < max_bytes_to_read)
      {
        if (std::ferror(m_fp))
        {
          m_error_flag = true;
          return -1;
        }

        m_eof_flag = true;
        *pEOF_flag = true;
      }

      return bytes_read;
    }
  };

  FileStream stream(fp);
  int width, height, file_comps;
  u8* data = jpgd::decompress_jpeg_image_from_stream(&stream, &width, &height, &file_comps, 4, 0);
  if (!data)
  {
    Console.Error("jpgd::decompress_jpeg_image_from_stream() failed");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(data));
  std::free(data);
  return true;
}

static bool JPEGCommonSaver(const RGBA8Image& image, jpge::output_stream& stream, int quality)
{
  jpge::params params;
  params.m_quality = quality;

  jpge::jpeg_encoder dst_image;
  if (!dst_image.init(&stream, image.GetWidth(), image.GetHeight(), 3, params))
    return false;

  // for RGBA->RGB
  std::vector<u8> row;
  row.resize(image.GetWidth() * 3);

  for (uint pass_index = 0; pass_index < dst_image.get_total_passes(); pass_index++)
  {
    for (u32 i = 0; i < image.GetHeight(); i++)
    {
      const u8* row_in = reinterpret_cast<const u8*>(image.GetRowPixels(i));
      u8* row_out = row.data();
      for (u32 j = 0; j < image.GetWidth(); j++)
      {
        *(row_out++) = *(row_in++);
        *(row_out++) = *(row_in++);
        *(row_out++) = *(row_in++);
        row_in++;
      }

      if (!dst_image.process_scanline(row.data()))
        return false;
    }
    if (!dst_image.process_scanline(NULL))
      return false;
  }

  dst_image.deinit();

  return true;
}

bool JPEGBufferSaver(const RGBA8Image& image, std::vector<u8>* buffer, int quality)
{
  class BufferStream : public jpge::output_stream
  {
    std::vector<u8>* buffer;

  public:
    explicit BufferStream(std::vector<u8>* buffer_) : buffer(buffer_) {}

    bool put_buf(const void* Pbuf, int len) override
    {
      const size_t old_size = buffer->size();
      buffer->resize(buffer->size() + static_cast<size_t>(len));
      std::memcpy(buffer->data() + old_size, Pbuf, static_cast<size_t>(len));
      return true;
    }
  };

  // give enough space to avoid reallocs
  buffer->reserve(image.GetWidth() * image.GetHeight() * 2);

  BufferStream stream(buffer);
  return JPEGCommonSaver(image, stream, quality);
}

bool JPEGFileSaver(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality)
{
  class FileStream : public jpge::output_stream
  {
    std::FILE* m_fp;
    bool m_error_flag = false;

  public:
    explicit FileStream(std::FILE* fp_) : m_fp(fp_) {}

    bool put_buf(const void* Pbuf, int len) override
    {
      if (m_error_flag)
        return false;

      if (std::fwrite(Pbuf, len, 1, m_fp) != 1)
      {
        m_error_flag = true;
        return false;
      }

      return true;
    }
  };

  FileStream stream(fp);
  return JPEGCommonSaver(image, stream, quality);
}

#endif

bool STBBufferLoader(RGBA8Image* image, const void* buffer, size_t buffer_size)
{
  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_memory(static_cast<const stbi_uc*>(buffer), static_cast<int>(buffer_size), &width,
                                         &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from memory: %s", error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool STBFileLoader(RGBA8Image* image, const char* filename, std::FILE* fp)
{
  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_file(fp, &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from memory: %s", error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool STBBufferSaverPNG(const RGBA8Image& image, std::vector<u8>* buffer, int quality)
{
  const auto write_func = [](void* context, void* data, int size) {
    std::vector<u8>* buffer = reinterpret_cast<std::vector<u8>*>(data);
    const u32 len = static_cast<u32>(size);
    buffer->resize(buffer->size() + len);
    std::memcpy(buffer->data(), data, len);
  };

  return (stbi_write_png_to_func(write_func, buffer, image.GetWidth(), image.GetHeight(), 4, image.GetPixels(),
                                 image.GetByteStride()) == 0);
}

bool STBBufferSaverJPEG(const RGBA8Image& image, std::vector<u8>* buffer, int quality)
{
  const auto write_func = [](void* context, void* data, int size) {
    std::vector<u8>* buffer = reinterpret_cast<std::vector<u8>*>(data);
    const u32 len = static_cast<u32>(size);
    buffer->resize(buffer->size() + len);
    std::memcpy(buffer->data(), data, len);
  };

  return (stbi_write_jpg_to_func(write_func, buffer, image.GetWidth(), image.GetHeight(), 4, image.GetPixels(),
                                 quality) == 0);
}

bool STBFileSaverPNG(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality)
{
  const auto write_func = [](void* context, void* data, int size) {
    std::fwrite(data, 1, size, static_cast<std::FILE*>(context));
  };

  return (stbi_write_png_to_func(write_func, fp, image.GetWidth(), image.GetHeight(), 4, image.GetPixels(),
                                 image.GetByteStride()) == 0);
}

bool STBFileSaverJPEG(const RGBA8Image& image, const char* filename, std::FILE* fp, int quality)
{
  const auto write_func = [](void* context, void* data, int size) {
    std::fwrite(data, 1, size, static_cast<std::FILE*>(context));
  };

  return (stbi_write_jpg_to_func(write_func, fp, image.GetWidth(), image.GetHeight(), 4, image.GetPixels(), quality) ==
          0);
}