#include "minizip_helpers.h"
#include "file_system.h"
#include "ioapi.h"
#include "types.h"
#include <algorithm>

namespace MinizipHelpers {

unzFile OpenUnzMemoryFile(const void* memory, size_t memory_size)
{
  struct MemoryFileInfo
  {
    const u8* data;
    ZPOS64_T data_size;
    ZPOS64_T position;
  };

  MemoryFileInfo* fi = new MemoryFileInfo;
  fi->data = static_cast<const u8*>(memory);
  fi->data_size = static_cast<ZPOS64_T>(memory_size);
  fi->position = 0;

#define FI static_cast<MemoryFileInfo*>(stream)

  zlib_filefunc64_def funcs = {
    [](voidpf opaque, const void* filename, int mode) -> voidpf { return opaque; }, // open
    [](voidpf opaque, voidpf stream, void* buf, uLong size) -> uLong {              // read
      const ZPOS64_T remaining = FI->data_size - FI->position;
      const ZPOS64_T to_read = std::min(remaining, static_cast<ZPOS64_T>(size));
      if (to_read > 0)
      {
        std::memcpy(buf, FI->data + FI->position, to_read);
        FI->position += to_read;
      }

      return static_cast<uLong>(to_read);
    },
    [](voidpf opaque, voidpf stream, const void* buf, uLong size) -> uLong { return 0; },         // write
    [](voidpf opaque, voidpf stream) -> ZPOS64_T { return static_cast<ZPOS64_T>(FI->position); }, // tell
    [](voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) -> long {                       // seek
      ZPOS64_T new_position = FI->position;
      if (origin == SEEK_SET)
        new_position = static_cast<int>(offset);
      else if (origin == SEEK_CUR)
        new_position += static_cast<int>(offset);
      else
        new_position = FI->data_size;
      if (new_position < 0 || new_position > FI->data_size)
        return -1;

      FI->position = new_position;
      return 0;
    },
    [](voidpf opaque, voidpf stream) -> int {
      delete FI;
      return 0;
    },                                                     // close
    [](voidpf opaque, voidpf stream) -> int { return 0; }, // testerror
    static_cast<voidpf>(fi)};

#undef FI

  unzFile zf = unzOpen2_64("", &funcs);
  if (!zf)
    delete fi;

  return zf;
}

unzFile OpenUnzFile(const char* filename)
{
  zlib_filefunc64_def funcs;
  fill_fopen64_filefunc(&funcs);

  funcs.zopen64_file = [](voidpf opaque, const void* filename, int mode) -> voidpf {
    const char* mode_fopen = NULL;
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ)
      mode_fopen = "rb";
    else if (mode & ZLIB_FILEFUNC_MODE_EXISTING)
      mode_fopen = "r+b";
    else if (mode & ZLIB_FILEFUNC_MODE_CREATE)
      mode_fopen = "wb";

    return FileSystem::OpenCFile(static_cast<const char*>(filename), mode_fopen);
  };

  return unzOpen2_64(filename, &funcs);
}

} // namespace MinizipHelpers