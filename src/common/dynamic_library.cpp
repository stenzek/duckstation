// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/dynamic_library.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include <cstring>

#ifdef _WIN32
#include "common/windows_headers.h"
#else
#include <dlfcn.h>
#endif

Log_SetChannel(DynamicLibrary);

DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(const char* filename)
{
  Error error;
  if (!Open(filename, &error))
    Log_ErrorPrint(error.GetDescription());
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& move) : m_handle(move.m_handle)
{
  move.m_handle = nullptr;
}

DynamicLibrary::~DynamicLibrary()
{
  Close();
}

std::string DynamicLibrary::GetUnprefixedFilename(const char* filename)
{
#if defined(_WIN32)
  return std::string(filename) + ".dll";
#elif defined(__APPLE__)
  return std::string(filename) + ".dylib";
#else
  return std::string(filename) + ".so";
#endif
}

std::string DynamicLibrary::GetVersionedFilename(const char* libname, int major, int minor)
{
#if defined(_WIN32)
  if (major >= 0 && minor >= 0)
    return fmt::format("{}-{}-{}.dll", libname, major, minor);
  else if (major >= 0)
    return fmt::format("{}-{}.dll", libname, major);
  else
    return fmt::format("{}.dll", libname);
#elif defined(__APPLE__)
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0)
    return fmt::format("{}{}.{}.{}.dylib", prefix, libname, major, minor);
  else if (major >= 0)
    return fmt::format("{}{}.{}.dylib", prefix, libname, major);
  else
    return fmt::format("{}{}.dylib", prefix, libname);
#else
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0)
    return fmt::format("{}{}.so.{}.{}", prefix, libname, major, minor);
  else if (major >= 0)
    return fmt::format("{}{}.so.{}", prefix, libname, major);
  else
    return fmt::format("{}{}.so", prefix, libname);
#endif
}

bool DynamicLibrary::Open(const char* filename, Error* error)
{
#ifdef _WIN32
  m_handle = reinterpret_cast<void*>(LoadLibraryW(StringUtil::UTF8StringToWideString(filename).c_str()));
  if (!m_handle)
  {
    Error::SetWin32(error, TinyString::from_format("Loading {} failed: ", filename), GetLastError());
    return false;
  }

  return true;
#else
  m_handle = dlopen(filename, RTLD_NOW);
  if (!m_handle)
  {
    const char* err = dlerror();
    Error::SetStringFmt(error, "Loading {} failed: {}", filename, err ? err : "<UNKNOWN>");
    return false;
  }

  return true;
#endif
}

void DynamicLibrary::Close()
{
  if (!IsOpen())
    return;

#ifdef _WIN32
  FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
  dlclose(m_handle);
#endif
  m_handle = nullptr;
}

void* DynamicLibrary::GetSymbolAddress(const char* name) const
{
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
  return reinterpret_cast<void*>(dlsym(m_handle, name));
#endif
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& move)
{
  Close();
  m_handle = move.m_handle;
  move.m_handle = nullptr;
  return *this;
}
