// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "dyn_freetype.h"
#include "dyn_libjpeg.h"
#include "dyn_libpng.h"
#include "dyn_libwebp.h"
#include "dyn_libzip.h"
#include "dyn_plutosvg.h"
#include "dyn_sdl.h"
#include "dyn_shaderc.h"
#include "dyn_spirv_cross.h"
#include "dyn_sqlite.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"

#include <atomic>
#include <utility>

LOG_CHANNEL(DynamicLibrary);

// Ensure it's in .bss
static_assert(std::is_trivially_copyable_v<DynFreeType> && std::is_standard_layout_v<DynFreeType>);
static_assert(std::is_trivially_copyable_v<DynPlutoSvg> && std::is_standard_layout_v<DynPlutoSvg>);
static_assert(std::is_trivially_copyable_v<DynLibPNG> && std::is_standard_layout_v<DynLibPNG>);
static_assert(std::is_trivially_copyable_v<DynLibJPEG> && std::is_standard_layout_v<DynLibJPEG>);
static_assert(std::is_trivially_copyable_v<DynLibWebP> && std::is_standard_layout_v<DynLibWebP>);
static_assert(std::is_trivially_copyable_v<DynLibZip> && std::is_standard_layout_v<DynLibZip>);
static_assert(std::is_trivially_copyable_v<DynSDL> && std::is_standard_layout_v<DynSDL>);
static_assert(std::is_trivially_copyable_v<DynShaderc> && std::is_standard_layout_v<DynShaderc>);
static_assert(std::is_trivially_copyable_v<DynSpirvCross> && std::is_standard_layout_v<DynSpirvCross>);
static_assert(std::is_trivially_copyable_v<DynSqlite> && std::is_standard_layout_v<DynSqlite>);

// Because of course friggin linux is different...
#ifdef _WIN32
static constexpr int FREETYPE_MAJOR_VERSION = -1;
static constexpr int PLUTOSVG_MAJOR_VERSION = -1;
static constexpr int LIBPNG_MAJOR_VERSION = -1;
static constexpr int LIBSDL_MAJOR_VERSION = -1;
static constexpr int LIBZIP_MAJOR_VERSION = -1;
static constexpr int SQLITE_MAJOR_VERSION = -1;
static constexpr int SPIRV_CROSS_MAJOR_VERSION = -1;
#else
static constexpr int FREETYPE_MAJOR_VERSION = 6;
static constexpr int PLUTOSVG_MAJOR_VERSION = 0;
static constexpr int LIBPNG_MAJOR_VERSION = 16;
static constexpr int LIBZIP_MAJOR_VERSION = 5;
static constexpr int LIBSDL_MAJOR_VERSION = 0;
static constexpr int SQLITE_MAJOR_VERSION = 3;
static constexpr int SPIRV_CROSS_MAJOR_VERSION = SPVC_C_API_VERSION_MAJOR;
#endif

namespace {
struct Locals
{
  ~Locals();

  DynamicLibrary freetype_library;
  std::once_flag freetype_init_flag;
  DynamicLibrary plutosvg_library;
  std::once_flag plutosvg_init_flag;
  DynamicLibrary libjpeg_library;
  std::once_flag libjpeg_init_flag;
  DynamicLibrary libpng_library;
  std::once_flag libpng_init_flag;
  DynamicLibrary libwebp_library;
  std::once_flag libwebp_init_flag;
  DynamicLibrary libzip_library;
  std::once_flag libzip_init_flag;
  DynamicLibrary sdl_library;
  std::once_flag sdl_init_flag;
  std::atomic<SDL_InitFlags> sdl_init_subsystems{0};
  DynamicLibrary sqlite_library;
  std::once_flag sqlite_init_flag;
  DynamicLibrary shaderc_library;
  std::once_flag shaderc_init_flag;
  shaderc_compiler_t shaderc_compiler;
  DynamicLibrary spirv_cross_library;
  std::once_flag spirv_cross_init_flag;
};
} // namespace

DynFreeType g_dyn_freetype;
DynPlutoSvg g_dyn_plutosvg;
DynLibJPEG g_dyn_libjpeg;
DynLibWebP g_dyn_libwebp;
DynLibPNG g_dyn_libpng;
DynLibZip g_dyn_libzip;
DynSDL g_dyn_sdl;
DynSqlite g_dyn_sqlite;
DynShaderc g_dyn_shaderc;
DynSpirvCross g_dyn_spirv_cross;
static Locals s_locals;

Locals::~Locals()
{
  DebugAssert(!s_locals.shaderc_compiler);

  // Quit any persistent SDL subsystems before unloading the library.
  if (const SDL_InitFlags active_subsystems = s_locals.sdl_init_subsystems.load(std::memory_order_acquire))
    g_dyn_sdl.SDL_QuitSubSystem(active_subsystems);
}

static bool LoadDynLib(const char* libname, int major_version, DynamicLibrary& dynlib, std::once_flag& once_flag,
                       std::span<const DynamicLibrary::SymbolTable> symbols, Error* const error,
                       bool (*extra_loader)(Error* const) = nullptr)
{
  std::call_once(once_flag, [&libname, &major_version, &dynlib, &symbols, extra_loader, error]() {
    Error lerror;
    DynamicLibrary lib;
    if (!lib.Open(DynamicLibrary::GetVersionedFilename(libname, major_version).c_str(), &lerror) ||
        !lib.ResolveSymbols(symbols.data(), symbols.size(), &lerror))
    {
      ERROR_LOG("Failed to load {}: {}", libname, lerror.GetDescription());
      Error::SetStringFmt(error, "Failed to load {}: {}", libname, lerror.GetDescription());
      return;
    }

    if (extra_loader && !extra_loader(error))
    {
      DynamicLibrary::ClearSymbols(symbols.data(), symbols.size());
      return;
    }

    dynlib = std::move(lib);
  });

  return dynlib.IsOpen();
}

static const DynamicLibrary::SymbolTable s_freetype_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_freetype.F},
  DYN_FREETYPE_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynFreeType::Open(Error* const error)
{
  if (s_locals.freetype_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("freetype", FREETYPE_MAJOR_VERSION, s_locals.freetype_library, s_locals.freetype_init_flag,
                    s_freetype_symbols, error);
}

static const DynamicLibrary::SymbolTable s_plutosvg_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_plutosvg.F},
  DYN_PLUTOSVG_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynPlutoSvg::Open(Error* const error)
{
  if (s_locals.plutosvg_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("plutosvg", PLUTOSVG_MAJOR_VERSION, s_locals.plutosvg_library, s_locals.plutosvg_init_flag,
                    s_plutosvg_symbols, error);
}

static const DynamicLibrary::SymbolTable s_libpng_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_libpng.F},
  DYN_LIBPNG_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynLibPNG::Open(Error* const error)
{
  if (s_locals.libpng_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("libpng16", LIBPNG_MAJOR_VERSION, s_locals.libpng_library, s_locals.libpng_init_flag,
                    s_libpng_symbols, error);
}

static const DynamicLibrary::SymbolTable s_libjpeg_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_libjpeg.F},
  DYN_LIBJPEG_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynLibJPEG::Open(Error* const error)
{
  if (s_locals.libjpeg_library.IsOpen()) [[likely]]
    return true;

#ifdef _WIN32
  static constexpr const char* libname = "jpeg62";
  static constexpr int major_version = -1;
#else
  static constexpr const char* libname = "jpeg";
  static constexpr int major_version = 62;
#endif

  return LoadDynLib(libname, major_version, s_locals.libjpeg_library, s_locals.libjpeg_init_flag, s_libjpeg_symbols,
                    error);
}

static const DynamicLibrary::SymbolTable s_libwebp_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_libwebp.F},
  DYN_LIBWEBP_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynLibWebP::Open(Error* const error)
{
  if (s_locals.libwebp_library.IsOpen()) [[likely]]
    return true;

#ifdef _WIN32
  static constexpr const char* libname = "libwebp";
  static constexpr int major_version = -1;
#else
  static constexpr const char* libname = "webp";
  static constexpr int major_version = 7;
#endif

  return LoadDynLib(libname, major_version, s_locals.libwebp_library, s_locals.libwebp_init_flag, s_libwebp_symbols,
                    error);
}

static const DynamicLibrary::SymbolTable s_libzip_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_libzip.F},
  DYN_LIBZIP_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynLibZip::Open(Error* const error)
{
  if (s_locals.libzip_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("zip", LIBZIP_MAJOR_VERSION, s_locals.libzip_library, s_locals.libzip_init_flag, s_libzip_symbols,
                    error);
}

static const DynamicLibrary::SymbolTable s_sdl_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_sdl.F},
  DYN_SDL_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

static const DynamicLibrary::SymbolTable s_sqlite_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_sqlite.F},
  DYN_SQLITE_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynSqlite::Open(Error* error)
{
  if (s_locals.sqlite_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("sqlite3", SQLITE_MAJOR_VERSION, s_locals.sqlite_library, s_locals.sqlite_init_flag,
                    s_sqlite_symbols, error);
}

static void SDLLogCallback(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
  static constexpr Log::Level priority_map[SDL_LOG_PRIORITY_COUNT] = {
    Log::Level::Debug,   // SDL_LOG_PRIORITY_INVALID
    Log::Level::Trace,   // SDL_LOG_PRIORITY_TRACE
    Log::Level::Verbose, // SDL_LOG_PRIORITY_VERBOSE
    Log::Level::Debug,   // SDL_LOG_PRIORITY_DEBUG
    Log::Level::Info,    // SDL_LOG_PRIORITY_INFO
    Log::Level::Warning, // SDL_LOG_PRIORITY_WARN
    Log::Level::Error,   // SDL_LOG_PRIORITY_ERROR
    Log::Level::Error,   // SDL_LOG_PRIORITY_CRITICAL
  };

  GENERIC_LOG(Log::Channel::SDL, priority_map[priority], Log::Color::Default, message);
}

static bool SDLLoadCallback(Error* const error)
{
  g_dyn_sdl.SDL_SetLogOutputFunction(SDLLogCallback, nullptr);
#if defined(_DEBUG) || defined(_DEVEL)
  g_dyn_sdl.SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#else
  g_dyn_sdl.SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
#endif
  return true;
}

bool DynSDL::Open(Error* const error)
{
  if (s_locals.sdl_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("SDL3", LIBSDL_MAJOR_VERSION, s_locals.sdl_library, s_locals.sdl_init_flag, s_sdl_symbols, error,
                    SDLLoadCallback);
}

bool DynSDL::InitSubSystem(SDL_InitFlags flags, Error* const error)
{
  // anything added?
  const SDL_InitFlags prev_subsystems = s_locals.sdl_init_subsystems.fetch_or(flags, std::memory_order_acq_rel);
  const SDL_InitFlags subsystems_to_init = (flags & ~prev_subsystems);
  if (subsystems_to_init == 0 || SDL_InitSubSystem(subsystems_to_init))
    return true;

  s_locals.sdl_init_subsystems.fetch_and(~subsystems_to_init, std::memory_order_acq_rel);

  const char* sdl_error = SDL_GetError();
  Error::SetStringFmt(error, "SDL_InitSubSystem(0x{:08X}) failed: {}", static_cast<u32>(subsystems_to_init),
                      sdl_error ? sdl_error : "");
  return false;
}

void DynSDL::QuitSubSystem(SDL_InitFlags flags)
{
  // anything removed?
  const SDL_InitFlags prev_subsystems = s_locals.sdl_init_subsystems.fetch_and(~flags, std::memory_order_acq_rel);
  const SDL_InitFlags subsystems_to_release = (flags & prev_subsystems);
  if (subsystems_to_release != 0)
    SDL_QuitSubSystem(subsystems_to_release);
}

static const DynamicLibrary::SymbolTable s_shaderc_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_shaderc.F},
  DYN_SHADERC_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};

bool DynShaderc::Open(Error* const error)
{
  if (s_locals.shaderc_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("shaderc_shared", -1, s_locals.shaderc_library, s_locals.shaderc_init_flag, s_shaderc_symbols,
                    error, [](Error* const error) {
                      g_dyn_shaderc.compiler = g_dyn_shaderc.shaderc_compiler_initialize();
                      if (!g_dyn_shaderc.compiler)
                      {
                        ERROR_LOG("shaderc_compiler_initialize() failed");
                        Error::SetStringView(error, "shaderc_compiler_initialize() failed");
                        return false;
                      }

                      return true;
                    });
}

void DynShaderc::Close()
{
  if (compiler)
    shaderc_compiler_release(std::exchange(compiler, nullptr));
}

// clang-format off
static const DynamicLibrary::SymbolTable s_spirv_cross_symbols[] = {
#define RESOLVE_SYMBOL(F) {#F, (void**)&g_dyn_spirv_cross.F},
  DYN_SPIRV_CROSS_FUNCTIONS(RESOLVE_SYMBOL)
  DYN_SPIRV_CROSS_HLSL_FUNCTIONS(RESOLVE_SYMBOL)
  DYN_SPIRV_CROSS_MSL_FUNCTIONS(RESOLVE_SYMBOL)
#undef RESOLVE_SYMBOL
};
// clang-format on

bool DynSpirvCross::Open(Error* const error)
{
  if (s_locals.spirv_cross_library.IsOpen()) [[likely]]
    return true;

  return LoadDynLib("spirv-cross-c-shared", SPIRV_CROSS_MAJOR_VERSION, s_locals.spirv_cross_library,
                    s_locals.spirv_cross_init_flag, s_spirv_cross_symbols, error);
}
