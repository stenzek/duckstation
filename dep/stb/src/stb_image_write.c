#include <stdlib.h>

#include "zlib.h"

// https://github.com/nothings/stb/issues/113
static unsigned char* compress_for_stbiw(unsigned char* data, int data_len, int* out_len, int quality)
{
  const uLongf bufSize = compressBound(data_len);

  // note that buf will be free'd by stb_image_write.h
  // with STBIW_FREE() (plain free() by default)
  unsigned char* buf = malloc(bufSize);
  if (buf == NULL)
    return NULL;

  if (compress2(buf, &bufSize, data, data_len, quality) != Z_OK)
  {
    free(buf);
    return NULL;
  }

  *out_len = bufSize;
  return buf;
}

#define STBIW_ZLIB_COMPRESS compress_for_stbiw
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
