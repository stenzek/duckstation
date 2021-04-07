#pragma once
#include "string.h"
#include "types.h"

namespace Common {

// this class provides enough storage room for all of these types
class Error
{
public:
  Error();
  Error(const Error& e);
  ~Error();

  enum class Type
  {
    None = 0,   // Set by default constructor, returns 'No Error'.
    Errno = 1,  // Error that is set by system functions, such as open().
    Socket = 2, // Error that is set by socket functions, such as socket(). On Unix this is the same as errno.
    User = 3,   // When translated, will return 'User Error %u' if no message is specified.
    Win32 = 4,  // Error that is returned by some Win32 functions, such as RegOpenKeyEx. Also used by other APIs through
               // GetLastError().
    HResult = 5, // Error that is returned by Win32 COM methods, e.g. S_OK.
  };

  ALWAYS_INLINE Type GetType() const { return m_type; }
  ALWAYS_INLINE int GetErrnoCode() const { return m_error.errno_f; }
  ALWAYS_INLINE int GetSocketCode() const { return m_error.socketerr; }
  ALWAYS_INLINE int GetUserCode() const { return m_error.user; }
#ifdef _WIN32
  ALWAYS_INLINE unsigned long GetWin32Code() const { return m_error.win32; }
  ALWAYS_INLINE long GetHResultCode() const { return m_error.hresult; }
#endif

  // get code, e.g. "0x00000002"
  ALWAYS_INLINE const String& GetCodeString() const { return m_code_string; }

  // get description, e.g. "File not Found"
  ALWAYS_INLINE const String& GetMessage() const { return m_message; }

  // setter functions
  void Clear();
  void SetErrno(int err);
  void SetSocket(int err);
  void SetMessage(const char* msg);
  void SetFormattedMessage(const char* format, ...) printflike(2, 3);
  void SetUser(int err, const char* msg);
  void SetUser(const char* code, const char* message);
  void SetUserFormatted(int err, const char* format, ...) printflike(3, 4);
  void SetUserFormatted(const char* code, const char* format, ...) printflike(3, 4);
#ifdef _WIN32
  void SetWin32(unsigned long err);
  void SetHResult(long err);
#endif

  // constructors
  static Error CreateNone();
  static Error CreateErrno(int err);
  static Error CreateSocket(int err);
  static Error CreateMessage(const char* msg);
  static Error CreateMessageFormatted(const char* format, ...) printflike(1, 2);
  static Error CreateUser(int err, const char* msg);
  static Error CreateUser(const char* code, const char* message);
  static Error CreateUserFormatted(int err, const char* format, ...) printflike(2, 3);
  static Error CreateUserFormatted(const char* code, const char* format, ...) printflike(2, 3);
#ifdef _WIN32
  static Error CreateWin32(unsigned long err);
  static Error CreateHResult(long err);
#endif

  // get code and description, e.g. "[0x00000002]: File not Found"
  SmallString GetCodeAndMessage() const;
  void GetCodeAndMessage(String& dest) const;

  // operators
  Error& operator=(const Error& e);
  bool operator==(const Error& e) const;
  bool operator!=(const Error& e) const;

private:
  Type m_type = Type::None;
  union
  {
    int none;
    int errno_f; // renamed from errno to avoid conflicts with #define'd errnos.
    int socketerr;
    int user;
#ifdef _WIN32
    unsigned long win32;
    long hresult;
#endif
  } m_error{};
  StackString<16> m_code_string;
  TinyString m_message;
};

} // namespace Common
