#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

// require vista+
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT _WIN32_WINNT_VISTA

#include <windows.h>

// hurr i'm windows, i like to conflict, fixme properly later please...
#if defined(FindTexture)
#undef FindTexture
#endif
#if defined(DrawText)
#undef DrawText
#endif
#if defined(CreateDirectory)
#undef CreateDirectory
#endif
#if defined(CopyFile)
#undef CopyFile
#endif
#if defined(DeleteFile)
#undef DeleteFile
#endif
#if defined(Yield)
#undef Yield
#endif
#if defined(LoadIcon)
#undef LoadIcon
#endif
#if defined(GetObject)
#undef GetObject
#endif