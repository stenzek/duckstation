#include "error.h"
#include <cstdlib>
#include <cstring>
#include <type_traits>

// Platform-specific includes
#if defined(_WIN32)
#include "windows_headers.h"
static_assert(std::is_same<DWORD, unsigned long>::value, "DWORD is unsigned long");
static_assert(std::is_same<HRESULT, long>::value, "HRESULT is long");
#endif

namespace Common {

Error::Error() : m_type(Type::None)
{
  m_error.none = 0;
}

Error::Error(const Error& c)
{
  m_type = c.m_type;
  std::memcpy(&m_error, &c.m_error, sizeof(m_error));
  m_code_string.AppendString(c.m_code_string);
  m_message.AppendString(c.m_message);
}

Error::~Error() = default;

void Error::Clear()
{
  m_type = Type::None;
  m_error.none = 0;
  m_code_string.Clear();
  m_message.Clear();
}

void Error::SetErrno(int err)
{
  m_type = Type::Errno;
  m_error.errno_f = err;

  m_code_string.Format("%i", err);

#ifdef _MSC_VER
  strerror_s(m_message.GetWriteableCharArray(), m_message.GetBufferSize(), err);
  m_message.UpdateSize();
#else
  const char* message = std::strerror(err);
  if (message)
    m_message = message;
  else
    m_message = StaticString("<Could not get error message>");
#endif
}

void Error::SetSocket(int err)
{
// Socket errors are win32 errors on windows
#ifdef _WIN32
  SetWin32(err);
#else
  SetErrno(err);
#endif
}

void Error::SetMessage(const char* msg)
{
  m_type = Type::User;
  m_error.user = 0;
  m_code_string.Clear();
  m_message = msg;
}

void Error::SetUser(int err, const char* msg)
{
  m_type = Type::User;
  m_error.user = err;
  m_code_string.Format("%d", err);
  m_message = msg;
}

void Error::SetUser(const char* code, const char* message)
{
  m_type = Type::User;
  m_error.user = 0;
  m_code_string = code;
  m_message = message;
}

void Error::SetUserFormatted(int err, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  m_type = Type::User;
  m_error.user = err;
  m_code_string.Format("%d", err);
  m_message.FormatVA(format, ap);
  va_end(ap);
}

void Error::SetUserFormatted(const char* code, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  m_type = Type::User;
  m_error.user = 0;
  m_code_string = code;
  m_message.FormatVA(format, ap);
  va_end(ap);
}

void Error::SetFormattedMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  m_type = Type::User;
  m_error.user = 0;
  m_code_string.Clear();
  m_message.FormatVA(format, ap);
  va_end(ap);
}

#ifdef _WIN32

void Error::SetWin32(unsigned long err)
{
  m_type = Type::Win32;
  m_error.win32 = err;
  m_code_string.Format("%u", static_cast<u32>(err));
  m_message.Clear();

  const DWORD r = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, m_error.win32, 0, m_message.GetWriteableCharArray(),
                                 m_message.GetWritableBufferSize(), NULL);
  if (r > 0)
  {
    m_message.Resize(r);
    m_message.RStrip();
  }
  else
  {
    m_message = "<Could not resolve system error ID>";
  }
}

void Error::SetHResult(long err)
{
  m_type = Type::HResult;
  m_error.win32 = err;
  m_code_string.Format("%08X", static_cast<u32>(err));
  m_message.Clear();

  const DWORD r = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, m_error.win32, 0, m_message.GetWriteableCharArray(),
                                 m_message.GetWritableBufferSize(), NULL);
  if (r > 0)
  {
    m_message.Resize(r);
    m_message.RStrip();
  }
  else
  {
    m_message = "<Could not resolve system error ID>";
  }
}

#endif

// constructors
Error Error::CreateNone()
{
  Error ret;
  ret.Clear();
  return ret;
}

Error Error::CreateErrno(int err)
{
  Error ret;
  ret.SetErrno(err);
  return ret;
}

Error Error::CreateSocket(int err)
{
  Error ret;
  ret.SetSocket(err);
  return ret;
}

Error Error::CreateMessage(const char* msg)
{
  Error ret;
  ret.SetMessage(msg);
  return ret;
}

Error Error::CreateUser(int err, const char* msg)
{
  Error ret;
  ret.SetUser(err, msg);
  return ret;
}

Error Error::CreateUser(const char* code, const char* message)
{
  Error ret;
  ret.SetUser(code, message);
  return ret;
}

Error Error::CreateMessageFormatted(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  Error ret;
  ret.m_type = Type::User;
  ret.m_message.FormatVA(format, ap);

  va_end(ap);

  return ret;
}

Error Error::CreateUserFormatted(int err, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  Error ret;
  ret.m_type = Type::User;
  ret.m_error.user = err;
  ret.m_code_string.Format("%d", err);
  ret.m_message.FormatVA(format, ap);

  va_end(ap);

  return ret;
}

Error Error::CreateUserFormatted(const char* code, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  Error ret;
  ret.m_type = Type::User;
  ret.m_error.user = 0;
  ret.m_code_string = code;
  ret.m_message.FormatVA(format, ap);

  va_end(ap);

  return ret;
}

#ifdef _WIN32
Error Error::CreateWin32(unsigned long err)
{
  Error ret;
  ret.SetWin32(err);
  return ret;
}

Error Error::CreateHResult(long err)
{
  Error ret;
  ret.SetHResult(err);
  return ret;
}

#endif

Error& Error::operator=(const Error& e)
{
  m_type = e.m_type;
  std::memcpy(&m_error, &e.m_error, sizeof(m_error));
  m_code_string.Clear();
  m_code_string.AppendString(e.m_code_string);
  m_message.Clear();
  m_message.AppendString(e.m_message);
  return *this;
}

bool Error::operator==(const Error& e) const
{
  switch (m_type)
  {
    case Type::None:
      return true;

    case Type::Errno:
      return m_error.errno_f == e.m_error.errno_f;

    case Type::Socket:
      return m_error.socketerr == e.m_error.socketerr;

    case Type::User:
      return m_error.user == e.m_error.user;

#ifdef _WIN32
    case Type::Win32:
      return m_error.win32 == e.m_error.win32;

    case Type::HResult:
      return m_error.hresult == e.m_error.hresult;
#endif
  }

  return false;
}

bool Error::operator!=(const Error& e) const
{
  switch (m_type)
  {
    case Type::None:
      return false;

    case Type::Errno:
      return m_error.errno_f != e.m_error.errno_f;

    case Type::Socket:
      return m_error.socketerr != e.m_error.socketerr;

    case Type::User:
      return m_error.user != e.m_error.user;

#ifdef _WIN32
    case Type::Win32:
      return m_error.win32 != e.m_error.win32;

    case Type::HResult:
      return m_error.hresult != e.m_error.hresult;
#endif
  }

  return true;
}

SmallString Error::GetCodeAndMessage() const
{
  SmallString ret;
  GetCodeAndMessage(ret);
  return ret;
}

void Error::GetCodeAndMessage(String& dest) const
{
  if (m_code_string.IsEmpty())
    dest.Assign(m_message);
  else
    dest.Format("[%s]: %s", m_code_string.GetCharArray(), m_message.GetCharArray());
}

} // namespace Common