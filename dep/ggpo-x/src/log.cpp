#include "types.h"

#include <cstdio>
#include <cstdarg>

//#define ENABLE_LOGGING

void Log(const char *fmt, ...)
{
#ifdef ENABLE_LOGGING
   std::va_list args;
   va_start(args, fmt);
   Logv(fmt, args);
   va_end(args);
#endif
}

void Logv(const char *fmt, std::va_list args)
{
#ifdef ENABLE_LOGGING
  std::vfprintf(stderr, fmt, args);
#endif
}
