#pragma once
#include "fmt/core.h"
#include "types.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

//
// String
// Implements a UTF-8 string container with copy-on-write behavior.
// The data class is not currently threadsafe (creating a mutex on each container would be overkill),
// so locking is still required when multiple threads are involved.
//
class String
{
public:
  // Internal StringData class.
  struct StringData
  {
    // Pointer to memory where the string is located
    char* pBuffer;

    // Length of the string located in pBuffer (in characters)
    u32 StringLength;

    // Size of the buffer pointed to by pBuffer
    u32 BufferSize;

    // Reference count of this data object. If set to -1,
    // it is considered noncopyable and any copies of the string
    // will always create their own copy.
    s32 ReferenceCount;

    // Whether the memory pointed to by pBuffer is writable.
    bool ReadOnly;
  };

public:
  using value_type = char;

  // Creates an empty string.
  String();

  // Creates a string containing the specified text.
  // Note that this will incur a heap allocation, even if Text is on the stack.
  // For strings that do not allocate any space on the heap, see StaticString.
  String(const char* Text);

  // Creates a string contained the specified text (with length).
  String(const char* Text, u32 Count);

  // Creates a string using the same buffer as another string (copy-on-write).
  String(const String& copyString);

  // Move constructor, take reference from other string.
  String(String&& moveString);

  // Construct a string from a data object, does not increment the reference count on the string data, use carefully.
  explicit String(StringData* pStringData) : m_pStringData(pStringData) {}

  // Creates string from string_view.
  String(const std::string_view& sv);

  // Destructor. Child classes may not have any destructors, as this is not virtual.
  ~String();

  // manual assignment
  void Assign(const String& copyString);
  void Assign(const char* copyText);
  void Assign(const std::string& copyString);
  void Assign(const std::string_view& copyString);
  void Assign(String&& moveString);

  // assignment but ensures that we have our own copy.
  void AssignCopy(const String& copyString);

  // Ensures that the string has its own unique copy of the data.
  void EnsureOwnWritableCopy();

  // Ensures that we have our own copy of the buffer, and spaceRequired bytes free in the buffer.
  void EnsureRemainingSpace(u32 spaceRequired);

  // clears the contents of the string
  void Clear();

  // clear the contents of the string, and free any memory currently being used
  void Obliterate();

  // swaps strings
  void Swap(String& swapString);

  // append a single character to this string
  void AppendCharacter(char c);

  // append a string to this string
  void AppendString(const String& appendStr);
  void AppendString(const char* appendText);
  void AppendString(const char* appendString, u32 Count);
  void AppendString(const std::string& appendString);
  void AppendString(const std::string_view& appendString);

  // append a substring of the specified string to this string
  void AppendSubString(const String& appendStr, s32 Offset = 0, s32 Count = std::numeric_limits<s32>::max());
  void AppendSubString(const char* appendText, s32 Offset = 0, s32 Count = std::numeric_limits<s32>::max());

  // append formatted string to this string
  void AppendFormattedString(const char* FormatString, ...) printflike(2, 3);
  void AppendFormattedStringVA(const char* FormatString, va_list ArgPtr);

  template<typename... T>
  void AppendFmtString(fmt::format_string<T...> fmt, T&&... args);

  // append a single character to this string
  void PrependCharacter(char c);

  // append a string to this string
  void PrependString(const String& appendStr);
  void PrependString(const char* appendText);
  void PrependString(const char* appendString, u32 Count);
  void PrependString(const std::string& appendStr);
  void PrependString(const std::string_view& appendStr);

  // append a substring of the specified string to this string
  void PrependSubString(const String& appendStr, s32 Offset = 0, s32 Count = std::numeric_limits<s32>::max());
  void PrependSubString(const char* appendText, s32 Offset = 0, s32 Count = std::numeric_limits<s32>::max());

  // append formatted string to this string
  void PrependFormattedString(const char* FormatString, ...) printflike(2, 3);
  void PrependFormattedStringVA(const char* FormatString, va_list ArgPtr);

  template<typename... T>
  void PrependFmtString(fmt::format_string<T...> fmt, T&&... args);

  // insert a string at the specified offset
  void InsertString(s32 offset, const String& appendStr);
  void InsertString(s32 offset, const char* appendStr);
  void InsertString(s32 offset, const char* appendStr, u32 appendStrLength);
  void InsertString(s32 offset, const std::string& appendStr);
  void InsertString(s32 offset, const std::string_view& appendStr);

  // set to formatted string
  void Format(const char* FormatString, ...) printflike(2, 3);
  void FormatVA(const char* FormatString, va_list ArgPtr);

  template<typename... T>
  void Fmt(fmt::format_string<T...> fmt, T&&... args);

  // compare one string to another
  bool Compare(const String& otherString) const;
  bool Compare(const char* otherText) const;
  bool SubCompare(const String& otherString, u32 Length) const;
  bool SubCompare(const char* otherText, u32 Length) const;
  bool CompareInsensitive(const String& otherString) const;
  bool CompareInsensitive(const char* otherText) const;
  bool SubCompareInsensitive(const String& otherString, u32 Length) const;
  bool SubCompareInsensitive(const char* otherText, u32 Length) const;

  // numerical compares
  int NumericCompare(const String& otherString) const;
  int NumericCompare(const char* otherText) const;
  int NumericCompareInsensitive(const String& otherString) const;
  int NumericCompareInsensitive(const char* otherText) const;

  // starts with / ends with
  bool StartsWith(const char* compareString, bool caseSensitive = true) const;
  bool StartsWith(const String& compareString, bool caseSensitive = true) const;
  bool EndsWith(const char* compareString, bool caseSensitive = true) const;
  bool EndsWith(const String& compareString, bool caseSensitive = true) const;

  // searches for a character inside a string
  // rfind is the same except it starts at the end instead of the start
  // returns -1 if it is not found, otherwise the offset in the string
  s32 Find(char c, u32 Offset = 0) const;
  s32 RFind(char c, u32 Offset = 0) const;

  // searches for a string inside a string
  // rfind is the same except it starts at the end instead of the start
  // returns -1 if it is not found, otherwise the offset in the string
  s32 Find(const char* str, u32 Offset = 0) const;

  // alters the length of the string to be at least len bytes long
  void Reserve(u32 newReserve, bool Force = false);

  // Cuts characters off the string to reduce it to len bytes long.
  void Resize(u32 newSize, char fillerCharacter = ' ', bool skrinkIfSmaller = false);

  // updates the internal length counter when the string is externally modified
  void UpdateSize();

  // shrink the string to the minimum size possible
  void Shrink(bool Force = false);

  // gets the size of the string
  u32 GetLength() const { return m_pStringData->StringLength; }
  bool IsEmpty() const { return (m_pStringData->StringLength == 0); }

  // gets the maximum number of bytes we can write to the string, currently
  u32 GetBufferSize() const { return m_pStringData->BufferSize; }
  u32 GetWritableBufferSize()
  {
    EnsureOwnWritableCopy();
    return m_pStringData->BufferSize;
  }

  // creates a new string using part of this string
  String SubString(s32 Offset, s32 Count = std::numeric_limits<s32>::max()) const;

  // erase count characters at offset from this string. if count is less than zero, everything past offset is erased
  void Erase(s32 Offset, s32 Count = std::numeric_limits<s32>::max());

  // replaces all instances of character c with character r in this string
  // returns the number of changes
  u32 Replace(char searchCharacter, char replaceCharacter);

  // replaces all instances of string s with string r in this string
  // returns the number of changes
  u32 Replace(const char* searchString, const char* replaceString);

  // convert string to lowercase
  void ToLower();

  // convert string to upper
  void ToUpper();

  // strip characters from start and end of the string
  void LStrip(const char* szStripCharacters = " \t\r\n");
  void RStrip(const char* szStripCharacters = " \t\r\n");
  void Strip(const char* szStripCharacters = " \t\r\n");

  // gets a constant pointer to the string
  const char* GetCharArray() const { return m_pStringData->pBuffer; }

  // gets a writable char array, do not write more than reserve characters to it.
  char* GetWriteableCharArray()
  {
    EnsureOwnWritableCopy();
    return m_pStringData->pBuffer;
  }

  // creates a new string from the specified format
  static String FromFormat(const char* FormatString, ...) printflike(1, 2);

  // accessor operators
  // const char &operator[](u32 i) const { DebugAssert(i < m_pStringData->StringLength); return
  // m_pStringData->pBuffer[i]; }  char &operator[](u32 i) { DebugAssert(i < m_pStringData->StringLength); return
  // m_pStringData->pBuffer[i]; }
  operator const char*() const { return GetCharArray(); }
  operator char*() { return GetWriteableCharArray(); }
  operator std::string_view() const
  {
    return IsEmpty() ? std::string_view() : std::string_view(GetCharArray(), GetLength());
  }

  // Will use the string data provided.
  String& operator=(const String& copyString)
  {
    Assign(copyString);
    return *this;
  }

  // Allocates own buffer and copies text.
  String& operator=(const char* Text)
  {
    Assign(Text);
    return *this;
  }
  String& operator=(const std::string& Text)
  {
    Assign(Text);
    return *this;
  }
  String& operator=(const std::string_view& Text)
  {
    Assign(Text);
    return *this;
  }

  // Move operator.
  String& operator=(String&& moveString)
  {
    Assign(moveString);
    return *this;
  }

  // comparative operators
  bool operator==(const String& compString) const { return Compare(compString); }
  bool operator==(const char* compString) const { return Compare(compString); }
  bool operator!=(const String& compString) const { return !Compare(compString); }
  bool operator!=(const char* compString) const { return !Compare(compString); }
  bool operator<(const String& compString) const { return (NumericCompare(compString) < 0); }
  bool operator<(const char* compString) const { return (NumericCompare(compString) < 0); }
  bool operator>(const String& compString) const { return (NumericCompare(compString) > 0); }
  bool operator>(const char* compString) const { return (NumericCompare(compString) > 0); }

  // STL adapters
  ALWAYS_INLINE void push_back(value_type&& val) { AppendCharacter(val); }

protected:
  // Internal append function.
  void InternalPrepend(const char* pString, u32 Length);
  void InternalAppend(const char* pString, u32 Length);

  // Pointer to string data.
  StringData* m_pStringData;

  // Empty string data.
  static const StringData s_EmptyStringData;
};

// static string, stored in .rodata
#define StaticString(Text)                                                                                             \
  []() noexcept -> String {                                                                                            \
    static constexpr u32 buffer_size = sizeof(Text);                                                                   \
    static constexpr u32 length = buffer_size - 1;                                                                     \
    static constexpr String::StringData data{const_cast<char*>(Text), length, buffer_size, static_cast<s32>(-1),       \
                                             true};                                                                    \
    return String(const_cast<String::StringData*>(&data));                                                             \
  }()

// stack-allocated string
template<u32 L>
class StackString : public String
{
public:
  StackString() : String(&m_sStringData) { InitStackStringData(); }

  StackString(const char* Text) : String(&m_sStringData)
  {
    InitStackStringData();
    Assign(Text);
  }

  StackString(const char* Text, u32 Count) : String(&m_sStringData)
  {
    InitStackStringData();
    AppendString(Text, Count);
  }

  StackString(const String& copyString) : String(&m_sStringData)
  {
    // force a copy by passing it a string pointer, instead of a string object
    InitStackStringData();
    Assign(copyString.GetCharArray());
  }

  StackString(const StackString& copyString) : String(&m_sStringData)
  {
    // force a copy by passing it a string pointer, instead of a string object
    InitStackStringData();
    Assign(copyString.GetCharArray());
  }

  StackString(const std::string_view& sv) : String(&m_sStringData)
  {
    InitStackStringData();
    AppendString(sv.data(), static_cast<u32>(sv.size()));
  }

  // Override the fromstring method
  static StackString FromFormat(const char* FormatString, ...) printflike(1, 2)
  {
    va_list argPtr;
    va_start(argPtr, FormatString);

    StackString returnValue;
    returnValue.FormatVA(FormatString, argPtr);

    va_end(argPtr);

    return returnValue;
  }

  template<typename... T>
  static StackString FromFmt(fmt::format_string<T...> fmt, T&&... args)
  {
    StackString ret;
    fmt::vformat_to(std::back_inserter(ret), fmt, fmt::make_format_args(args...));
    return ret;
  }

  // Will use the string data provided.
  StackString& operator=(const StackString& copyString)
  {
    Assign(copyString.GetCharArray());
    return *this;
  }
  StackString& operator=(const String& copyString)
  {
    Assign(copyString.GetCharArray());
    return *this;
  }

  // Allocates own buffer and copies text.
  StackString& operator=(const char* Text)
  {
    Assign(Text);
    return *this;
  }
  StackString& operator=(const std::string& Text)
  {
    Assign(Text);
    return *this;
  }
  StackString& operator=(const std::string_view& Text)
  {
    Assign(Text);
    return *this;
  }

private:
  StringData m_sStringData;
  char m_strStackBuffer[L + 1];

  inline void InitStackStringData()
  {
    m_sStringData.pBuffer = m_strStackBuffer;
    m_sStringData.StringLength = 0;
    m_sStringData.BufferSize = countof(m_strStackBuffer);
    m_sStringData.ReadOnly = false;
    m_sStringData.ReferenceCount = -1;

#ifdef _DEBUG
    std::memset(m_strStackBuffer, 0, sizeof(m_strStackBuffer));
#else
    m_strStackBuffer[0] = '\0';
#endif
  }
};

// stack string types
typedef StackString<64> TinyString;
typedef StackString<256> SmallString;
typedef StackString<512> LargeString;
typedef StackString<512> PathString;

// empty string global
extern const String EmptyString;

template<typename... T>
void String::AppendFmtString(fmt::format_string<T...> fmt, T&&... args)
{
  fmt::vformat_to(std::back_inserter(*this), fmt, fmt::make_format_args(args...));
}

template<typename... T>
void String::PrependFmtString(fmt::format_string<T...> fmt, T&&... args)
{
  TinyString str;
  fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
  PrependString(str);
}

template<typename... T>
void String::Fmt(fmt::format_string<T...> fmt, T&&... args)
{
  Clear();
  fmt::vformat_to(std::back_inserter(*this), fmt, fmt::make_format_args(args...));
}
