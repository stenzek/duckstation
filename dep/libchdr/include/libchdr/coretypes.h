#ifndef __CORETYPES_H__
#define __CORETYPES_H__

#include <stdint.h>
#include <stdio.h>

#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

#define ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

#if defined(__PS3__) || defined(__PSL1GHT__)
#undef UINT32
#undef UINT16
#undef UINT8
#undef INT32
#undef INT16
#undef INT8
#endif

typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef int64_t INT64;
typedef int32_t INT32;
typedef int16_t INT16;
typedef int8_t INT8;

#define core_file FILE
#define core_fopen(file) fopen(file, "rb")

#if defined USE_LIBRETRO_VFS
	#define core_fseek fseek
	#define core_ftell ftell
#elif defined(__WIN32__) || defined(_WIN32) || defined(WIN32) || defined(__WIN64__)
	#define core_fseek _fseeki64
	#define core_ftell _ftelli64
#elif defined(_LARGEFILE_SOURCE) && defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
	#define core_fseek fseeko64
	#define core_ftell ftello64
#elif defined(__PS3__) && !defined(__PSL1GHT__) || defined(__SWITCH__)
    #define core_fseek(x,y,z) fseek(x,(off_t)y,z)
    #define core_ftell(x) (off_t)ftell(x)
#else
	#define core_fseek fseeko
	#define core_ftell ftello
#endif
#define core_fread(fc, buff, len) fread(buff, 1, len, fc)
#define core_fclose fclose

static UINT64 core_fsize(core_file *f)
{
    UINT64 rv;
    UINT64 p = core_ftell(f);
    core_fseek(f, 0, SEEK_END);
    rv = core_ftell(f);
    core_fseek(f, p, SEEK_SET);
    return rv;
}

#endif
