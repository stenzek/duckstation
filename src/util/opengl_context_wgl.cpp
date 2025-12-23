// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_wgl.h"
#include "opengl_loader.h"
#include "gpu_texture.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/scoped_guard.h"

LOG_CHANNEL(GPUDevice);

#ifdef __clang__
#pragma clang diagnostic ignored "-Wmicrosoft-cast"
#endif

namespace dyn_libs {

#define OPENGL_FUNCTIONS(X)                                                                                            \
  X(wglCreateContext)                                                                                                  \
  X(wglDeleteContext)                                                                                                  \
  X(wglGetCurrentContext)                                                                                              \
  X(wglGetCurrentDC)                                                                                                   \
  X(wglGetProcAddress)                                                                                                 \
  X(wglMakeCurrent)                                                                                                    \
  X(wglShareLists)

static bool LoadOpenGLLibrary(Error* error);
static void CloseOpenGLLibrary();
static void* GetProcAddressCallback(const char* name);

static DynamicLibrary s_opengl_library;

#define DECLARE_OPENGL_FUNCTION(F) static decltype(&::F) F;
OPENGL_FUNCTIONS(DECLARE_OPENGL_FUNCTION)
#undef DECLARE_OPENGL_FUNCTION
} // namespace dyn_libs

bool dyn_libs::LoadOpenGLLibrary(Error* error)
{
  if (s_opengl_library.IsOpen())
    return true;
  else if (!s_opengl_library.Open("opengl32.dll", error))
    return false;

  bool result = true;
#define RESOLVE_OPENGL_FUNCTION(F) result = result && s_opengl_library.GetSymbol(#F, &F);
  OPENGL_FUNCTIONS(RESOLVE_OPENGL_FUNCTION);
#undef RESOLVE_OPENGL_FUNCTION

  if (!result)
  {
    CloseOpenGLLibrary();
    Error::SetStringView(error, "One or more required functions from opengl32.dll is missing.");
    return false;
  }

  std::atexit(&CloseOpenGLLibrary);
  return true;
}

void dyn_libs::CloseOpenGLLibrary()
{
#define CLOSE_OPENGL_FUNCTION(F) F = nullptr;
  OPENGL_FUNCTIONS(CLOSE_OPENGL_FUNCTION);
#undef CLOSE_OPENGL_FUNCTION

  s_opengl_library.Close();
}

#undef OPENGL_FUNCTIONS

void* dyn_libs::GetProcAddressCallback(const char* name)
{
  void* addr = dyn_libs::wglGetProcAddress(name);
  if (addr)
    return addr;

  // try opengl32.dll
  return s_opengl_library.GetSymbolAddress(name);
}

static bool ReloadWGL(HDC dc, Error* error)
{
  if (!gladLoadWGL(dc, [](const char* name) { return (GLADapiproc)dyn_libs::wglGetProcAddress(name); }))
  {
    Error::SetStringView(error, "Loading GLAD WGL functions failed");
    return false;
  }

  return true;
}

OpenGLContextWGL::OpenGLContextWGL() = default;

OpenGLContextWGL::~OpenGLContextWGL()
{
  if (m_rc)
  {
    if (dyn_libs::wglGetCurrentContext() == m_rc)
      dyn_libs::wglMakeCurrent(nullptr, nullptr);

    dyn_libs::wglDeleteContext(m_rc);
  }

  if (m_pbuffer)
  {
    wglReleasePbufferDCARB(m_pbuffer, m_pbuffer_dc);
    wglDestroyPbufferARB(m_pbuffer);
    DeleteDC(m_dummy_dc);
    DestroyWindow(m_dummy_window);
  }
}

std::unique_ptr<OpenGLContext> OpenGLContextWGL::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                        std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextWGL> context = std::make_unique<OpenGLContextWGL>();
  if (!dyn_libs::LoadOpenGLLibrary(error) || !context->Initialize(wi, surface, versions_to_try, error))
    context.reset();

  return context;
}

bool OpenGLContextWGL::Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try,
                                  Error* error)
{
  const HDC hdc = wi.IsSurfaceless() ? GetPBufferDC(error) : CreateDCAndSetPixelFormat(wi, error);
  if (!hdc)
    return false;

  // Everything including core/ES requires a dummy profile to load the WGL extensions.
  if (!CreateAnyContext(hdc, nullptr, true, error))
    return false;

  for (const Version& cv : versions_to_try)
  {
    if (cv.profile == Profile::NoProfile)
    {
      // we already have the dummy context, so just use that
      m_version = cv;
      *surface = hdc;
      return true;
    }
    else if (CreateVersionContext(cv, hdc, nullptr, true, error))
    {
      m_version = cv;
      *surface = hdc;
      return true;
    }
  }

  Error::SetStringView(error, "Failed to create any contexts.");
  return false;
}

void* OpenGLContextWGL::GetProcAddress(const char* name)
{
  return dyn_libs::GetProcAddressCallback(name);
}

OpenGLContext::SurfaceHandle OpenGLContextWGL::CreateSurface(WindowInfo& wi, Error* error /*= nullptr*/)
{
  if (wi.IsSurfaceless()) [[unlikely]]
  {
    Error::SetStringView(error, "Trying to create a surfaceless surface.");
    return nullptr;
  }

  return CreateDCAndSetPixelFormat(wi, error);
}

void OpenGLContextWGL::DestroySurface(SurfaceHandle handle)
{
  // pbuffer/surfaceless?
  if (!handle)
    return;

  // current buffer? switch to pbuffer first
  if (dyn_libs::wglGetCurrentDC() == static_cast<HDC>(handle))
    MakeCurrent(nullptr);

  DeleteDC(static_cast<HDC>(handle));
}

void OpenGLContextWGL::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  RECT client_rc = {};
  GetClientRect(static_cast<HWND>(wi.window_handle), &client_rc);
  wi.surface_width = static_cast<u16>(client_rc.right - client_rc.left);
  wi.surface_height = static_cast<u16>(client_rc.bottom - client_rc.top);
}

bool OpenGLContextWGL::SwapBuffers()
{
  return ::SwapBuffers(m_current_dc);
}

bool OpenGLContextWGL::IsCurrent() const
{
  return (m_rc && dyn_libs::wglGetCurrentContext() == m_rc);
}

bool OpenGLContextWGL::MakeCurrent(SurfaceHandle surface, Error* error /* = nullptr */)
{
  const HDC new_dc = surface ? static_cast<HDC>(surface) : GetPBufferDC(error);
  if (!new_dc)
    return false;
  else if (m_current_dc == new_dc)
    return true;

  if (!dyn_libs::wglMakeCurrent(new_dc, m_rc))
  {
    ERROR_LOG("wglMakeCurrent() failed: {}", GetLastError());
    return false;
  }

  m_current_dc = new_dc;
  return true;
}

bool OpenGLContextWGL::DoneCurrent()
{
  if (!dyn_libs::wglMakeCurrent(m_current_dc, nullptr))
    return false;

  m_current_dc = nullptr;
  return true;
}

bool OpenGLContextWGL::SupportsNegativeSwapInterval() const
{
  return GLAD_WGL_EXT_swap_control && GLAD_WGL_EXT_swap_control_tear;
}

bool OpenGLContextWGL::SetSwapInterval(s32 interval, Error* error)
{
  if (!GLAD_WGL_EXT_swap_control)
  {
    Error::SetStringView(error, "WGL_EXT_swap_control is not supported.");
    return false;
  }

  if (!wglSwapIntervalEXT(interval))
  {
    Error::SetWin32(error, "wglSwapIntervalEXT() failed: ", GetLastError());
    return false;
  }

  return true;
}

std::unique_ptr<OpenGLContext> OpenGLContextWGL::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                     Error* error)
{
  std::unique_ptr<OpenGLContextWGL> context = std::make_unique<OpenGLContextWGL>();
  const HDC hdc = wi.IsSurfaceless() ? context->GetPBufferDC(error) : context->CreateDCAndSetPixelFormat(wi, error);
  if (!hdc)
    return nullptr;

  if (m_version.profile == Profile::NoProfile)
  {
    if (!context->CreateAnyContext(hdc, m_rc, false, error))
      return nullptr;
  }
  else
  {
    if (!context->CreateVersionContext(m_version, hdc, m_rc, false, error))
      return nullptr;
  }

  context->m_version = m_version;
  *surface = wi.IsSurfaceless() ? hdc : nullptr;
  return context;
}

HDC OpenGLContextWGL::CreateDCAndSetPixelFormat(WindowInfo& wi, Error* error)
{
  if (wi.type != WindowInfo::Type::Win32)
  {
    Error::SetStringFmt(error, "Unknown window info type {}", static_cast<unsigned>(wi.type));
    return NULL;
  }

  PIXELFORMATDESCRIPTOR pfd = {};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.dwLayerMask = PFD_MAIN_PLANE;
  pfd.cRedBits = 8;
  pfd.cGreenBits = 8;
  pfd.cBlueBits = 8;
  pfd.cColorBits = 24;

  const HWND hwnd = static_cast<HWND>(wi.window_handle);
  HDC hDC = ::GetDC(hwnd);
  if (!hDC)
  {
    Error::SetWin32(error, "GetDC() failed: ", GetLastError());
    return {};
  }

  if (!m_pixel_format.has_value())
  {
    const int pf = ChoosePixelFormat(hDC, &pfd);
    if (pf == 0)
    {
      Error::SetWin32(error, "ChoosePixelFormat() failed: ", GetLastError());
      DeleteDC(hDC);
      return {};
    }

    m_pixel_format = pf;
  }

  if (!SetPixelFormat(hDC, m_pixel_format.value(), &pfd))
  {
    Error::SetWin32(error, "SetPixelFormat() failed: ", GetLastError());
    DeleteDC(hDC);
    return {};
  }

  wi.surface_format = GPUTextureFormat::RGBA8;
  return hDC;
}

HDC OpenGLContextWGL::GetPBufferDC(Error* error)
{
  static bool window_class_registered = false;
  static const wchar_t* window_class_name = L"ContextWGLPBuffer";

  if (m_pbuffer_dc)
    return m_pbuffer_dc;

  if (!window_class_registered)
  {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = 0;
    wc.lpfnWndProc = DefWindowProcW;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = window_class_name;
    wc.hIconSm = NULL;

    if (!RegisterClassExW(&wc))
    {
      Error::SetStringView(error, "(ContextWGL::CreatePBuffer) RegisterClassExW() failed");
      return NULL;
    }

    window_class_registered = true;
  }

  Assert(m_dummy_window == NULL);
  Assert(m_pbuffer == NULL);

  HWND hwnd = CreateWindowExW(0, window_class_name, window_class_name, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
  if (!hwnd)
  {
    Error::SetStringView(error, "(ContextWGL::CreatePBuffer) CreateWindowEx() failed");
    return NULL;
  }

  ScopedGuard hwnd_guard([hwnd]() { DestroyWindow(hwnd); });

  WindowInfo wi;
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = hwnd;
  HDC hdc = CreateDCAndSetPixelFormat(wi, error);
  if (!hdc)
    return NULL;

  ScopedGuard hdc_guard([hdc, hwnd]() { ::ReleaseDC(hwnd, hdc); });

  static constexpr const int pb_attribs[] = {0, 0};

  HGLRC temp_rc = nullptr;
  ScopedGuard temp_rc_guard([&temp_rc, hdc]() {
    if (temp_rc)
    {
      dyn_libs::wglMakeCurrent(hdc, nullptr);
      dyn_libs::wglDeleteContext(temp_rc);
    }
  });

  if (!GLAD_WGL_ARB_pbuffer)
  {
    // we're probably running completely surfaceless... need a temporary context.
    temp_rc = dyn_libs::wglCreateContext(hdc);
    if (!temp_rc || !dyn_libs::wglMakeCurrent(hdc, temp_rc))
    {
      Error::SetStringView(error, "Failed to create temporary context to load WGL for pbuffer.");
      return NULL;
    }

    if (!ReloadWGL(hdc, error))
      return NULL;

    if (!GLAD_WGL_ARB_pbuffer)
    {
      Error::SetStringView(error, "Missing WGL_ARB_pbuffer");
      return NULL;
    }
  }

  AssertMsg(m_pixel_format.has_value(), "Has pixel format for pbuffer");
  HPBUFFERARB pbuffer = wglCreatePbufferARB(hdc, m_pixel_format.value(), 1, 1, pb_attribs);
  if (!pbuffer)
  {
    Error::SetStringView(error, "(ContextWGL::CreatePBuffer) wglCreatePbufferARB() failed");
    return NULL;
  }

  ScopedGuard pbuffer_guard([pbuffer]() { wglDestroyPbufferARB(pbuffer); });

  HDC dc = wglGetPbufferDCARB(pbuffer);
  if (!dc)
  {
    Error::SetStringView(error, "(ContextWGL::CreatePbuffer) wglGetPbufferDCARB() failed");
    return NULL;
  }

  m_dummy_window = hwnd;
  m_dummy_dc = hdc;
  m_pbuffer = pbuffer;
  m_pbuffer_dc = dc;

  temp_rc_guard.Run();
  pbuffer_guard.Cancel();
  hdc_guard.Cancel();
  hwnd_guard.Cancel();
  return dc;
}

bool OpenGLContextWGL::CreateAnyContext(HDC hdc, HGLRC share_context, bool make_current, Error* error)
{
  m_rc = dyn_libs::wglCreateContext(hdc);
  if (!m_rc)
  {
    Error::SetWin32(error, "wglCreateContext() failed: ", GetLastError());
    return false;
  }

  if (make_current)
  {
    if (!dyn_libs::wglMakeCurrent(hdc, m_rc))
    {
      Error::SetWin32(error, "wglMakeCurrent() failed: ", GetLastError());
      return false;
    }

    m_current_dc = hdc;

    // re-init glad-wgl
    if (!ReloadWGL(m_current_dc, error))
    {
      Error::SetStringView(error, "Loading GLAD WGL functions failed");
      return false;
    }
  }

  if (share_context && !dyn_libs::wglShareLists(share_context, m_rc))
  {
    Error::SetWin32(error, "wglShareLists() failed: ", GetLastError());
    return false;
  }

  return true;
}

bool OpenGLContextWGL::CreateVersionContext(const Version& version, HDC hdc, HGLRC share_context, bool make_current,
                                            Error* error)
{
  // we need create context attribs
  if (!GLAD_WGL_ARB_create_context)
  {
    Error::SetStringView(error, "Missing GLAD_WGL_ARB_create_context.");
    return false;
  }

  HGLRC new_rc;
  if (version.profile == Profile::Core)
  {
    const int attribs[] = {WGL_CONTEXT_PROFILE_MASK_ARB,
                           WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                           WGL_CONTEXT_MAJOR_VERSION_ARB,
                           version.major_version,
                           WGL_CONTEXT_MINOR_VERSION_ARB,
                           version.minor_version,
#ifdef _DEBUG
                           WGL_CONTEXT_FLAGS_ARB,
                           WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB | WGL_CONTEXT_DEBUG_BIT_ARB,
#else
                           WGL_CONTEXT_FLAGS_ARB,
                           WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
#endif
                           0,
                           0};

    new_rc = wglCreateContextAttribsARB(hdc, share_context, attribs);
  }
  else if (version.profile == Profile::ES)
  {
    if ((version.major_version >= 2 && !GLAD_WGL_EXT_create_context_es2_profile) ||
        (version.major_version < 2 && !GLAD_WGL_EXT_create_context_es_profile))
    {
      Error::SetStringView(error, "WGL_EXT_create_context_es_profile not supported");
      return false;
    }

    const int attribs[] = {
      WGL_CONTEXT_PROFILE_MASK_ARB,
      ((version.major_version >= 2) ? WGL_CONTEXT_ES2_PROFILE_BIT_EXT : WGL_CONTEXT_ES_PROFILE_BIT_EXT),
      WGL_CONTEXT_MAJOR_VERSION_ARB,
      version.major_version,
      WGL_CONTEXT_MINOR_VERSION_ARB,
      version.minor_version,
      0,
      0};

    new_rc = wglCreateContextAttribsARB(hdc, share_context, attribs);
  }
  else
  {
    Error::SetStringView(error, "Unknown profile");
    return false;
  }

  if (!new_rc)
    return false;

  // destroy and swap contexts
  if (m_rc)
  {
    if (!dyn_libs::wglMakeCurrent(hdc, make_current ? new_rc : nullptr))
    {
      Error::SetWin32(error, "wglMakeCurrent() failed: ", GetLastError());
      dyn_libs::wglDeleteContext(new_rc);
      return false;
    }

    m_current_dc = hdc;

    // re-init glad-wgl
    if (make_current && !ReloadWGL(hdc, error))
      return false;

    dyn_libs::wglDeleteContext(m_rc);
  }

  m_rc = new_rc;
  return true;
}
