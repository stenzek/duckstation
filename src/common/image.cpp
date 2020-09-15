#include "image.h"
#include "file_system.h"
#include "log.h"
#include "stb_image.h"
Log_SetChannel(Common::Image);

namespace Common {
bool LoadImageFromFile(Common::RGBA8Image* image, const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
  {
    return false;
  }

  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_file(fp.get(), &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from '%s': %s", filename, error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool LoadImageFromBuffer(Common::RGBA8Image* image, const void* buffer, std::size_t buffer_size)
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
} // namespace Common