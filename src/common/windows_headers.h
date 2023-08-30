// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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

#if defined(CreateDirectory)
#undef CreateDirectory
#endif
#if defined(CopyFile)
#undef CopyFile
#endif
#if defined(DeleteFile)
#undef DeleteFile
#endif
#if defined(GetMessage)
#undef GetMessage
#endif
