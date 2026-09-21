// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/dynamic_library.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include <cstring>

#ifdef _WIN32
#include "common/windows_headers.h"
#else
#include <dlfcn.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <link.h>
#endif
#endif

LOG_CHANNEL(DynamicLibrary);

DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(const char* filename)
{
  Error error;
  if (!Open(filename, &error))
    ERROR_LOG(error.GetDescription());
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
#ifdef _WIN32
  return std::string(filename) + ".dll";
#elifdef __APPLE__
  return std::string(filename) + ".dylib";
#else
  return std::string(filename) + ".so";
#endif
}

std::string DynamicLibrary::GetVersionedFilename(const char* libname, int major, int minor, int patch)
{
#ifdef _WIN32
  if (major >= 0 && minor >= 0 && patch >= 0)
    return fmt::format("{}-{}-{}-{}.dll", libname, major, minor, patch);
  else if (major >= 0 && minor >= 0)
    return fmt::format("{}-{}-{}.dll", libname, major, minor);
  else if (major >= 0)
    return fmt::format("{}-{}.dll", libname, major);
  else
    return fmt::format("{}.dll", libname);
#elifdef __APPLE__
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0 && patch >= 0)
    return fmt::format("{}{}.{}.{}.{}.dylib", prefix, libname, major, minor, patch);
  else if (major >= 0 && minor >= 0)
    return fmt::format("{}{}.{}.{}.dylib", prefix, libname, major, minor);
  else if (major >= 0)
    return fmt::format("{}{}.{}.dylib", prefix, libname, major);
  else
    return fmt::format("{}{}.dylib", prefix, libname);
#else
  const char* prefix = std::strncmp(libname, "lib", 3) ? "lib" : "";
  if (major >= 0 && minor >= 0 && patch >= 0)
    return fmt::format("{}{}.so.{}.{}.{}", prefix, libname, major, minor, patch);
  else if (major >= 0 && minor >= 0)
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

#ifdef __linux__
  struct link_map* map;
  if (dlinfo(m_handle, RTLD_DI_LINKMAP, &map) == 0)
    DEV_LOG("{} loaded as {}", filename, map->l_name);
#endif

  return true;
#endif
}

void DynamicLibrary::Adopt(void* handle)
{
  AssertMsg(handle, "Handle is valid");

  Close();

  m_handle = handle;
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

bool DynamicLibrary::ResolveSymbols(const SymbolTable* symbols, size_t count, Error* error /* = nullptr */) const
{
  for (size_t i = 0; i < count; i++)
  {
    if (!GetSymbol(symbols[i].name, symbols[i].ptr))
    {
      Error::SetStringFmt(error, "Failed to load symbol {} from library", symbols[i].name);
      ClearSymbols(symbols, count);
      return false;
    }
  }

  return true;
}

bool DynamicLibrary::ResolveSymbols(const OptionalSymbolTable* symbols, size_t count,
                                    Error* error /* = nullptr */) const
{
  for (size_t i = 0; i < count; i++)
  {
    if (!GetSymbol(symbols[i].name, symbols[i].ptr))
    {
      if (symbols[i].required)
      {
        Error::SetStringFmt(error, "Failed to load required symbol {} from library", symbols[i].name);
        ClearSymbols(symbols, count);
        return false;
      }
      else
      {
        WARNING_LOG("Failed to load optional symbol {} from library", symbols[i].name);
      }
    }
  }

  return true;
}

bool DynamicLibrary::ResolveSymbols(const std::span<const SymbolTable> symbols, Error* error /*= nullptr*/) const
{
  return ResolveSymbols(symbols.data(), symbols.size(), error);
}

bool DynamicLibrary::ResolveSymbols(const std::span<const OptionalSymbolTable> symbols,
                                    Error* error /*= nullptr*/) const
{
  return ResolveSymbols(symbols.data(), symbols.size(), error);
}

void DynamicLibrary::ClearSymbols(const SymbolTable* symbols, size_t count)
{
  for (size_t i = 0; i < count; i++)
    *symbols[i].ptr = nullptr;
}

void DynamicLibrary::ClearSymbols(const OptionalSymbolTable* symbols, size_t count)
{
  for (size_t i = 0; i < count; i++)
    *symbols[i].ptr = nullptr;
}

void DynamicLibrary::ClearSymbols(const std::span<const SymbolTable> symbols)
{
  ClearSymbols(symbols.data(), symbols.size());
}

void DynamicLibrary::ClearSymbols(const std::span<const OptionalSymbolTable> symbols)
{
  ClearSymbols(symbols.data(), symbols.size());
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& move)
{
  Close();
  m_handle = move.m_handle;
  move.m_handle = nullptr;
  return *this;
}
