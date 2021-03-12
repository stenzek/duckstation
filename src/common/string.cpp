#include "string.h"
#include "assert.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#define CASE_COMPARE _stricmp
#define CASE_N_COMPARE _strnicmp
#else
#define CASE_COMPARE strcasecmp
#define CASE_N_COMPARE strncasecmp
#endif

// globals
const String::StringData String::s_EmptyStringData = {const_cast<char*>(""), 0, 1, -1, true};
const String EmptyString;

// helper functions
static String::StringData* StringDataAllocate(u32 allocSize)
{
  DebugAssert(allocSize > 0);

  String::StringData* pStringData =
    reinterpret_cast<String::StringData*>(std::malloc(sizeof(String::StringData) + allocSize));
  pStringData->pBuffer = reinterpret_cast<char*>(pStringData + 1);
  pStringData->StringLength = 0;
  pStringData->BufferSize = allocSize;
  pStringData->ReadOnly = false;
  pStringData->ReferenceCount = 1;

  // if in debug build, set all to zero, otherwise only the first to zero.
#ifdef _DEBUG
  std::memset(pStringData->pBuffer, 0, allocSize);
#else
  pStringData->pBuffer[0] = 0;
  if (allocSize > 1)
    pStringData->pBuffer[allocSize - 1] = 0;
#endif

  return pStringData;
}

static inline void StringDataAddRef(String::StringData* pStringData)
{
  DebugAssert(pStringData->ReferenceCount > 0);
  ++pStringData->ReferenceCount;
}

static inline void StringDataRelease(String::StringData* pStringData)
{
  if (pStringData->ReferenceCount == -1)
    return;

  DebugAssert(pStringData->ReferenceCount > 0);
  u32 newRefCount = --pStringData->ReferenceCount;
  if (!newRefCount)
    std::free(pStringData);
}

static String::StringData* StringDataClone(const String::StringData* pStringData, u32 newSize, bool copyPastString)
{
  DebugAssert(newSize >= 0);

  String::StringData* pClone = StringDataAllocate(newSize);
  if (pStringData->StringLength > 0)
  {
    u32 copyLength;

    if (copyPastString)
    {
      copyLength = std::min(newSize, pStringData->BufferSize);
      if (copyLength > 0)
      {
        std::memcpy(pClone->pBuffer, pStringData->pBuffer, copyLength);
        if (copyLength < pStringData->BufferSize)
          pClone->pBuffer[copyLength - 1] = 0;
      }
    }
    else
    {
      copyLength = std::min(newSize, pStringData->StringLength);
      if (copyLength > 0)
      {
        std::memcpy(pClone->pBuffer, pStringData->pBuffer, copyLength);
        pClone->pBuffer[copyLength] = 0;
      }
    }

    pClone->StringLength = copyLength;
  }

  return pClone;
}

static String::StringData* StringDataReallocate(String::StringData* pStringData, u32 newSize)
{
  DebugAssert(newSize > pStringData->StringLength);
  DebugAssert(pStringData->ReferenceCount == 1);

  // perform realloc
  pStringData = reinterpret_cast<String::StringData*>(std::realloc(pStringData, sizeof(String::StringData) + newSize));
  pStringData->pBuffer = reinterpret_cast<char*>(pStringData + 1);

  // zero bytes in debug
#ifdef _DEBUG
  if (newSize > pStringData->BufferSize)
  {
    u32 bytesToZero = newSize - pStringData->BufferSize;
    std::memset(pStringData->pBuffer + (newSize - bytesToZero), 0, bytesToZero);
  }
#else
  if (newSize > pStringData->BufferSize)
  {
    pStringData->pBuffer[newSize - 1] = 0;
  }
#endif

  // update size
  pStringData->BufferSize = newSize;
  return pStringData;
}

static bool StringDataIsSharable(const String::StringData* pStringData)
{
  return pStringData->ReadOnly || pStringData->ReferenceCount != -1;
}

static bool StringDataIsShared(const String::StringData* pStringData)
{
  return pStringData->ReferenceCount > 1;
}

String::String() : m_pStringData(const_cast<String::StringData*>(&s_EmptyStringData)) {}

String::String(const String& copyString)
{
  // special case: empty strings
  if (copyString.IsEmpty())
  {
    m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
  }
  // is the string data sharable?
  else if (StringDataIsSharable(copyString.m_pStringData))
  {
    m_pStringData = copyString.m_pStringData;
    if (!m_pStringData->ReadOnly)
      StringDataAddRef(m_pStringData);
  }
  // create a clone for ourselves
  else
  {
    // since we're going to the effort of creating a clone, we might as well create it as the smallest size possible
    m_pStringData = StringDataClone(copyString.m_pStringData, copyString.m_pStringData->StringLength + 1, false);
  }
}

String::String(const char* Text) : m_pStringData(const_cast<String::StringData*>(&s_EmptyStringData))
{
  Assign(Text);
}

String::String(const char* Text, u32 Count) : m_pStringData(const_cast<String::StringData*>(&s_EmptyStringData))
{
  AppendString(Text, Count);
}

String::String(String&& moveString)
{
  Assign(moveString);
}

String::String(const std::string_view& sv)
{
  AppendString(sv.data(), static_cast<u32>(sv.size()));
}

String::~String()
{
  StringDataRelease(m_pStringData);
}

void String::EnsureOwnWritableCopy()
{
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    StringData* pNewStringData = StringDataClone(m_pStringData, m_pStringData->StringLength + 1, false);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;
  }
}

void String::EnsureRemainingSpace(u32 spaceRequired)
{
  StringData* pNewStringData;
  u32 requiredReserve = m_pStringData->StringLength + spaceRequired + 1;

  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    pNewStringData = StringDataClone(m_pStringData, std::max(requiredReserve, m_pStringData->BufferSize), false);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;
  }
  else if (m_pStringData->BufferSize < requiredReserve)
  {
    u32 newSize = std::max(requiredReserve, m_pStringData->BufferSize * 2);

    // if we are the only owner of the buffer, we can simply realloc it
    if (m_pStringData->ReferenceCount == 1)
    {
      // do realloc and update pointer
      m_pStringData = StringDataReallocate(m_pStringData, newSize);
    }
    else
    {
      // clone and release old
      pNewStringData = StringDataClone(m_pStringData, std::max(requiredReserve, newSize), false);
      StringDataRelease(m_pStringData);
      m_pStringData = pNewStringData;
    }
  }
}

void String::InternalAppend(const char* pString, u32 Length)
{
  EnsureRemainingSpace(Length);

  DebugAssert((Length + m_pStringData->StringLength) < m_pStringData->BufferSize);
  DebugAssert(m_pStringData->ReferenceCount <= 1 && !m_pStringData->ReadOnly);

  std::memcpy(m_pStringData->pBuffer + m_pStringData->StringLength, pString, Length);
  m_pStringData->StringLength += Length;
  m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
}

void String::InternalPrepend(const char* pString, u32 Length)
{
  EnsureRemainingSpace(Length);

  DebugAssert((Length + m_pStringData->StringLength) < m_pStringData->BufferSize);
  DebugAssert(m_pStringData->ReferenceCount <= 1 && !m_pStringData->ReadOnly);

  std::memmove(m_pStringData->pBuffer + Length, m_pStringData->pBuffer, m_pStringData->StringLength);
  std::memcpy(m_pStringData->pBuffer, pString, Length);
  m_pStringData->StringLength += Length;
  m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
}

void String::AppendCharacter(char c)
{
  InternalAppend(&c, 1);
}

void String::AppendString(const String& appendStr)
{
  if (appendStr.GetLength() > 0)
    InternalAppend(appendStr.GetCharArray(), appendStr.GetLength());
}

void String::AppendString(const char* appendText)
{
  u32 textLength = static_cast<u32>(std::strlen(appendText));
  if (textLength > 0)
    InternalAppend(appendText, textLength);
}

void String::AppendString(const char* appendString, u32 Count)
{
  if (Count > 0)
    InternalAppend(appendString, Count);
}

void String::AppendString(const std::string& appendString)
{
  if (!appendString.empty())
    InternalAppend(appendString.c_str(), static_cast<u32>(appendString.size()));
}

void String::AppendString(const std::string_view& appendString)
{
  if (!appendString.empty())
    InternalAppend(appendString.data(), static_cast<u32>(appendString.size()));
}

void String::AppendSubString(const String& appendStr, s32 Offset /* = 0 */, s32 Count /* = INT_std::max */)
{
  u32 appendStrLength = appendStr.GetLength();

  // calc real offset
  u32 realOffset;
  if (Offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)appendStrLength + Offset);
  else
    realOffset = std::min((u32)Offset, appendStrLength);

  // calc real count
  u32 realCount;
  if (Count < 0)
    realCount = std::min(appendStrLength - realOffset, (u32)std::max((s32)0, (s32)appendStrLength + Count));
  else
    realCount = std::min(appendStrLength - realOffset, (u32)Count);

  // should be safe
  DebugAssert((realOffset + realCount) <= appendStrLength);
  if (realCount > 0)
    InternalAppend(appendStr.GetCharArray() + realOffset, realCount);
}

void String::AppendSubString(const char* appendText, s32 Offset /* = 0 */, s32 Count /* = INT_std::max */)
{
  u32 appendTextLength = static_cast<u32>(std::strlen(appendText));

  // calc real offset
  u32 realOffset;
  if (Offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)appendTextLength + Offset);
  else
    realOffset = std::min((u32)Offset, appendTextLength);

  // calc real count
  u32 realCount;
  if (Count < 0)
    realCount = std::min(appendTextLength - realOffset, (u32)std::max((s32)0, (s32)appendTextLength + Count));
  else
    realCount = std::min(appendTextLength - realOffset, (u32)Count);

  // should be safe
  DebugAssert((realOffset + realCount) <= appendTextLength);
  if (realCount > 0)
    InternalAppend(appendText + realOffset, realCount);
}

void String::AppendFormattedString(const char* FormatString, ...)
{
  va_list ap;
  va_start(ap, FormatString);
  AppendFormattedStringVA(FormatString, ap);
  va_end(ap);
}

void String::AppendFormattedStringVA(const char* FormatString, va_list ArgPtr)
{
  // We have a 1KB byte buffer on the stack here. If this is too little, we'll grow it via the heap,
  // but 1KB should be enough for most strings.
  char stackBuffer[1024];
  char* pHeapBuffer = NULL;
  char* pBuffer = stackBuffer;
  u32 currentBufferSize = countof(stackBuffer);
  u32 charsWritten;

  for (;;)
  {
    va_list ArgPtrCopy;
    va_copy(ArgPtrCopy, ArgPtr);
    int ret = std::vsnprintf(pBuffer, currentBufferSize, FormatString, ArgPtrCopy);
    va_end(ArgPtrCopy);
    if (ret < 0 || ((u32)ret >= (currentBufferSize - 1)))
    {
      currentBufferSize *= 2;
      pBuffer = pHeapBuffer = reinterpret_cast<char*>(std::realloc(pHeapBuffer, currentBufferSize));
      continue;
    }

    charsWritten = (u32)ret;
    break;
  }

  InternalAppend(pBuffer, charsWritten);

  if (pHeapBuffer != NULL)
    std::free(pHeapBuffer);
}

void String::PrependCharacter(char c)
{
  InternalPrepend(&c, 1);
}

void String::PrependString(const String& appendStr)
{
  if (appendStr.GetLength() > 0)
    InternalPrepend(appendStr.GetCharArray(), appendStr.GetLength());
}

void String::PrependString(const char* appendText)
{
  u32 textLength = static_cast<u32>(std::strlen(appendText));
  if (textLength > 0)
    InternalPrepend(appendText, textLength);
}

void String::PrependString(const char* appendString, u32 Count)
{
  if (Count > 0)
    InternalPrepend(appendString, Count);
}

void String::PrependString(const std::string& appendStr)
{
  if (!appendStr.empty())
    InternalPrepend(appendStr.c_str(), static_cast<u32>(appendStr.size()));
}

void String::PrependString(const std::string_view& appendStr)
{
  if (!appendStr.empty())
    InternalPrepend(appendStr.data(), static_cast<u32>(appendStr.size()));
}

void String::PrependSubString(const String& appendStr, s32 Offset /* = 0 */, s32 Count /* = INT_std::max */)
{
  u32 appendStrLength = appendStr.GetLength();

  // calc real offset
  u32 realOffset;
  if (Offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)appendStrLength + Offset);
  else
    realOffset = std::min((u32)Offset, appendStrLength);

  // calc real count
  u32 realCount;
  if (Count < 0)
    realCount = std::min(appendStrLength - realOffset, (u32)std::max((s32)0, (s32)appendStrLength + Count));
  else
    realCount = std::min(appendStrLength - realOffset, (u32)Count);

  // should be safe
  DebugAssert((realOffset + realCount) <= appendStrLength);
  if (realCount > 0)
    InternalPrepend(appendStr.GetCharArray() + realOffset, realCount);
}

void String::PrependSubString(const char* appendText, s32 Offset /* = 0 */, s32 Count /* = INT_std::max */)
{
  u32 appendTextLength = static_cast<u32>(std::strlen(appendText));

  // calc real offset
  u32 realOffset;
  if (Offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)appendTextLength + Offset);
  else
    realOffset = std::min((u32)Offset, appendTextLength);

  // calc real count
  u32 realCount;
  if (Count < 0)
    realCount = std::min(appendTextLength - realOffset, (u32)std::max((s32)0, (s32)appendTextLength + Count));
  else
    realCount = std::min(appendTextLength - realOffset, (u32)Count);

  // should be safe
  DebugAssert((realOffset + realCount) <= appendTextLength);
  if (realCount > 0)
    InternalPrepend(appendText + realOffset, realCount);
}

void String::PrependFormattedString(const char* FormatString, ...)
{
  va_list ap;
  va_start(ap, FormatString);
  PrependFormattedStringVA(FormatString, ap);
  va_end(ap);
}

void String::PrependFormattedStringVA(const char* FormatString, va_list ArgPtr)
{
  // We have a 1KB byte buffer on the stack here. If this is too little, we'll grow it via the heap,
  // but 1KB should be enough for most strings.
  char stackBuffer[1024];
  char* pHeapBuffer = NULL;
  char* pBuffer = stackBuffer;
  u32 currentBufferSize = countof(stackBuffer);
  u32 charsWritten;

  for (;;)
  {
    int ret = std::vsnprintf(pBuffer, currentBufferSize, FormatString, ArgPtr);
    if (ret < 0 || ((u32)ret >= (currentBufferSize - 1)))
    {
      currentBufferSize *= 2;
      pBuffer = pHeapBuffer = reinterpret_cast<char*>(std::realloc(pHeapBuffer, currentBufferSize));
      continue;
    }

    charsWritten = (u32)ret;
    break;
  }

  InternalPrepend(pBuffer, charsWritten);

  if (pHeapBuffer != NULL)
    std::free(pHeapBuffer);
}

void String::InsertString(s32 offset, const String& appendStr)
{
  InsertString(offset, appendStr, appendStr.GetLength());
}

void String::InsertString(s32 offset, const char* appendStr)
{
  InsertString(offset, appendStr, static_cast<u32>(std::strlen(appendStr)));
}

void String::InsertString(s32 offset, const char* appendStr, u32 appendStrLength)
{
  if (appendStrLength == 0)
    return;

  EnsureRemainingSpace(appendStrLength);

  // calc real offset
  u32 realOffset;
  if (offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)m_pStringData->StringLength + offset);
  else
    realOffset = std::min((u32)offset, m_pStringData->StringLength);

  // determine number of characters after offset
  DebugAssert(realOffset <= m_pStringData->StringLength);
  u32 charactersAfterOffset = m_pStringData->StringLength - realOffset;
  if (charactersAfterOffset > 0)
    std::memmove(m_pStringData->pBuffer + offset + appendStrLength, m_pStringData->pBuffer + offset,
                 charactersAfterOffset);

  // insert the string
  std::memcpy(m_pStringData->pBuffer + realOffset, appendStr, appendStrLength);
  m_pStringData->StringLength += appendStrLength;

  // ensure null termination
  m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
}

void String::InsertString(s32 offset, const std::string& appendStr)
{
  InsertString(offset, appendStr.c_str(), static_cast<u32>(appendStr.size()));
}

void String::InsertString(s32 offset, const std::string_view& appendStr)
{
  InsertString(offset, appendStr.data(), static_cast<u32>(appendStr.size()));
}

void String::Format(const char* FormatString, ...)
{
  va_list ap;
  va_start(ap, FormatString);
  FormatVA(FormatString, ap);
  va_end(ap);
}

void String::FormatVA(const char* FormatString, va_list ArgPtr)
{
  if (GetLength() > 0)
    Clear();

  AppendFormattedStringVA(FormatString, ArgPtr);
}

void String::Assign(const String& copyString)
{
  // special case: empty strings
  if (copyString.IsEmpty())
  {
    m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
  }
  // is the string data sharable?
  else if (StringDataIsSharable(copyString.m_pStringData))
  {
    m_pStringData = copyString.m_pStringData;
    if (!m_pStringData->ReadOnly)
      StringDataAddRef(m_pStringData);
  }
  // create a clone for ourselves
  else
  {
    // since we're going to the effort of creating a clone, we might as well create it as the smallest size possible
    m_pStringData = StringDataClone(copyString.m_pStringData, copyString.m_pStringData->StringLength + 1, false);
  }
}

void String::Assign(const char* copyText)
{
  Clear();
  AppendString(copyText);
}

void String::Assign(String&& moveString)
{
  Clear();
  m_pStringData = moveString.m_pStringData;
  moveString.m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
}

void String::Assign(const std::string& copyString)
{
  Clear();
  AppendString(copyString.data(), static_cast<u32>(copyString.size()));
}

void String::Assign(const std::string_view& copyString)
{
  Clear();
  AppendString(copyString.data(), static_cast<u32>(copyString.size()));
}

void String::AssignCopy(const String& copyString)
{
  Clear();
  AppendString(copyString);
}

void String::Swap(String& swapString)
{
  std::swap(m_pStringData, swapString.m_pStringData);
}

bool String::Compare(const String& otherString) const
{
  return (std::strcmp(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer) == 0);
}

bool String::Compare(const char* otherText) const
{
  return (std::strcmp(m_pStringData->pBuffer, otherText) == 0);
}

bool String::SubCompare(const String& otherString, u32 Length) const
{
  return (std::strncmp(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer, Length) == 0);
}

bool String::SubCompare(const char* otherText, u32 Length) const
{
  return (std::strncmp(m_pStringData->pBuffer, otherText, Length) == 0);
}

bool String::CompareInsensitive(const String& otherString) const
{
  return (CASE_COMPARE(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer) == 0);
}

bool String::CompareInsensitive(const char* otherText) const
{
  return (CASE_COMPARE(m_pStringData->pBuffer, otherText) == 0);
}

bool String::SubCompareInsensitive(const String& otherString, u32 Length) const
{
  return (CASE_N_COMPARE(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer, Length) == 0);
}

bool String::SubCompareInsensitive(const char* otherText, u32 Length) const
{
  return (CASE_N_COMPARE(m_pStringData->pBuffer, otherText, Length) == 0);
}

int String::NumericCompare(const String& otherString) const
{
  return std::strcmp(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer);
}

int String::NumericCompare(const char* otherText) const
{
  return std::strcmp(m_pStringData->pBuffer, otherText);
}

int String::NumericCompareInsensitive(const String& otherString) const
{
  return CASE_COMPARE(m_pStringData->pBuffer, otherString.m_pStringData->pBuffer);
}

int String::NumericCompareInsensitive(const char* otherText) const
{
  return CASE_COMPARE(m_pStringData->pBuffer, otherText);
}

bool String::StartsWith(const char* compareString, bool caseSensitive /*= true*/) const
{
  u32 compareStringLength = static_cast<u32>(std::strlen(compareString));
  if (compareStringLength > m_pStringData->StringLength)
    return false;

  return (caseSensitive) ? (std::strncmp(compareString, m_pStringData->pBuffer, compareStringLength) == 0) :
                           (CASE_N_COMPARE(compareString, m_pStringData->pBuffer, compareStringLength) == 0);
}

bool String::StartsWith(const String& compareString, bool caseSensitive /*= true*/) const
{
  u32 compareStringLength = compareString.GetLength();
  if (compareStringLength > m_pStringData->StringLength)
    return false;

  return (caseSensitive) ?
           (std::strncmp(compareString.m_pStringData->pBuffer, m_pStringData->pBuffer, compareStringLength) == 0) :
           (CASE_N_COMPARE(compareString.m_pStringData->pBuffer, m_pStringData->pBuffer, compareStringLength) == 0);
}

bool String::EndsWith(const char* compareString, bool caseSensitive /*= true*/) const
{
  u32 compareStringLength = static_cast<u32>(std::strlen(compareString));
  if (compareStringLength > m_pStringData->StringLength)
    return false;

  u32 startOffset = m_pStringData->StringLength - compareStringLength;
  return (caseSensitive) ?
           (std::strncmp(compareString, m_pStringData->pBuffer + startOffset, compareStringLength) == 0) :
           (CASE_N_COMPARE(compareString, m_pStringData->pBuffer + startOffset, compareStringLength) == 0);
}

bool String::EndsWith(const String& compareString, bool caseSensitive /*= true*/) const
{
  u32 compareStringLength = compareString.GetLength();
  if (compareStringLength > m_pStringData->StringLength)
    return false;

  u32 startOffset = m_pStringData->StringLength - compareStringLength;
  return (caseSensitive) ? (std::strncmp(compareString.m_pStringData->pBuffer, m_pStringData->pBuffer + startOffset,
                                         compareStringLength) == 0) :
                           (CASE_N_COMPARE(compareString.m_pStringData->pBuffer, m_pStringData->pBuffer + startOffset,
                                           compareStringLength) == 0);
}

void String::Clear()
{
  if (m_pStringData == &s_EmptyStringData)
    return;

  // Do we have a shared buffer? If so, cancel it and allocate a new one when we need to.
  // Otherwise, clear the current buffer.
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    // replace with empty string data
    Obliterate();
  }
  else
  {
    // in debug, zero whole string, in release, zero only the first character
#if _DEBUG
    std::memset(m_pStringData->pBuffer, 0, m_pStringData->BufferSize);
#else
    m_pStringData->pBuffer[0] = '\0';
#endif
    m_pStringData->StringLength = 0;
  }
}

void String::Obliterate()
{
  if (m_pStringData == &s_EmptyStringData)
    return;

  // Force a release of the current buffer.
  StringDataRelease(m_pStringData);
  m_pStringData = const_cast<StringData*>(&s_EmptyStringData);
}

s32 String::Find(char c, u32 Offset /* = 0*/) const
{
  DebugAssert(Offset <= m_pStringData->StringLength);
  char* pAt = std::strchr(m_pStringData->pBuffer + Offset, c);
  return (pAt == NULL) ? -1 : s32(pAt - m_pStringData->pBuffer);
}

s32 String::RFind(char c, u32 Offset /* = 0*/) const
{
  DebugAssert(Offset <= m_pStringData->StringLength);
  char* pAt = std::strrchr(m_pStringData->pBuffer + Offset, c);
  return (pAt == NULL) ? -1 : s32(pAt - m_pStringData->pBuffer);
}

s32 String::Find(const char* str, u32 Offset /* = 0 */) const
{
  DebugAssert(Offset <= m_pStringData->StringLength);
  char* pAt = std::strstr(m_pStringData->pBuffer + Offset, str);
  return (pAt == NULL) ? -1 : s32(pAt - m_pStringData->pBuffer);
}

void String::Reserve(u32 newReserve, bool Force /* = false */)
{
  DebugAssert(!Force || newReserve >= m_pStringData->StringLength);

  u32 newSize = (Force) ? newReserve + 1 : std::max(newReserve + 1, m_pStringData->BufferSize);
  StringData* pNewStringData;

  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    pNewStringData = StringDataClone(m_pStringData, newSize, false);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;
  }
  else
  {
    // skip if smaller, and not forced
    if (newSize <= m_pStringData->BufferSize && !Force)
      return;

    // if we are the only owner of the buffer, we can simply realloc it
    if (m_pStringData->ReferenceCount == 1)
    {
      // do realloc and update pointer
      m_pStringData = StringDataReallocate(m_pStringData, newSize);
    }
    else
    {
      // clone and release old
      pNewStringData = StringDataClone(m_pStringData, newSize, false);
      StringDataRelease(m_pStringData);
      m_pStringData = pNewStringData;
    }
  }
}

void String::Resize(u32 newSize, char fillerCharacter /* = ' ' */, bool skrinkIfSmaller /* = false */)
{
  StringData* pNewStringData;

  // if going larger, or we don't own the buffer, realloc
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly || newSize >= m_pStringData->BufferSize)
  {
    pNewStringData = StringDataClone(m_pStringData, newSize + 1, true);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;

    if (m_pStringData->StringLength < newSize)
    {
      std::memset(m_pStringData->pBuffer + m_pStringData->StringLength, fillerCharacter,
                  m_pStringData->BufferSize - m_pStringData->StringLength - 1);
    }

    m_pStringData->StringLength = newSize;
  }
  else
  {
    // owns the buffer, and going smaller
    DebugAssert(newSize < m_pStringData->BufferSize);

    // update length and terminator
#if _DEBUG
    std::memset(m_pStringData->pBuffer + newSize, 0, m_pStringData->BufferSize - newSize);
#else
    m_pStringData->pBuffer[newSize] = 0;
#endif
    m_pStringData->StringLength = newSize;

    // shrink if requested
    if (skrinkIfSmaller)
      Shrink(false);
  }
}

void String::UpdateSize()
{
  EnsureOwnWritableCopy();
  m_pStringData->StringLength = static_cast<u32>(std::strlen(m_pStringData->pBuffer));
}

void String::Shrink(bool Force /* = false */)
{
  // only shrink of we own the buffer, or forced
  if (Force || m_pStringData->ReferenceCount == 1)
    Reserve(m_pStringData->StringLength);
}

String String::SubString(s32 Offset, s32 Count /* = -1 */) const
{
  String returnStr;
  returnStr.AppendSubString(*this, Offset, Count);
  return returnStr;
}

void String::Erase(s32 Offset, s32 Count /* = INT_std::max */)
{
  u32 currentLength = m_pStringData->StringLength;

  // calc real offset
  u32 realOffset;
  if (Offset < 0)
    realOffset = (u32)std::max((s32)0, (s32)currentLength + Offset);
  else
    realOffset = std::min((u32)Offset, currentLength);

  // calc real count
  u32 realCount;
  if (Count < 0)
    realCount = std::min(currentLength - realOffset, (u32)std::max((s32)0, (s32)currentLength + Count));
  else
    realCount = std::min(currentLength - realOffset, (u32)Count);

  // Fastpath: offset == 0, count < 0, wipe whole string.
  if (realOffset == 0 && realCount == currentLength)
  {
    Clear();
    return;
  }

  // Fastpath: offset >= 0, count < 0, wipe everything after offset + count
  if ((realOffset + realCount) == m_pStringData->StringLength)
  {
    m_pStringData->StringLength -= realCount;
#ifdef _DEBUG
    std::memset(m_pStringData->pBuffer + m_pStringData->StringLength, 0,
                m_pStringData->BufferSize - m_pStringData->StringLength);
#else
    m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
#endif
  }
  // Slowpath: offset >= 0, count < length
  else
  {
    u32 afterEraseBlock = m_pStringData->StringLength - realOffset - realCount;
    DebugAssert(afterEraseBlock > 0);

    std::memmove(m_pStringData->pBuffer + Offset, m_pStringData->pBuffer + realOffset + realCount, afterEraseBlock);
    m_pStringData->StringLength = m_pStringData->StringLength - realCount;

#ifdef _DEBUG
    std::memset(m_pStringData->pBuffer + m_pStringData->StringLength, 0,
                m_pStringData->BufferSize - m_pStringData->StringLength);
#else
    m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
#endif
  }
}

u32 String::Replace(char searchCharacter, char replaceCharacter)
{
  u32 nReplacements = 0;
  char* pCurrent = std::strchr(m_pStringData->pBuffer, searchCharacter);
  while (pCurrent != NULL)
  {
    if ((nReplacements++) == 0)
      EnsureOwnWritableCopy();

    *pCurrent = replaceCharacter;
    pCurrent = std::strchr(pCurrent + 1, searchCharacter);
  }

  return nReplacements;
}

u32 String::Replace(const char* searchString, const char* replaceString)
{
  u32 nReplacements = 0;
  u32 searchStringLength = static_cast<u32>(std::strlen(searchString));

#if 0
    u32 replaceStringLength = static_cast<u32>(std::strlen(replaceString));
    s32 lengthDifference = (s32)replaceStringLength - (s32)searchStringLength;

    char *pCurrent = std::strchr(m_pStringData->pBuffer, searchString);
    while (pCurrent != NULL)
    {
        if ((nReplacements++) == 0)
        {
            if (lengthDifference > 0)
                EnsureRemainingSpace(lengthDifference);
            else
                EnsureOwnCopy();
        }
        else if (lengthDifference > 0)
            EnsureRemainingSpace(lengthDifference);
    }
#endif

  // TODO: Fastpath if strlen(searchString) == strlen(replaceString)

  String tempString;
  char* pStart = m_pStringData->pBuffer;
  char* pCurrent = std::strstr(pStart, searchString);
  char* pLast = NULL;
  while (pCurrent != NULL)
  {
    if ((nReplacements++) == 0)
      tempString.Reserve(m_pStringData->StringLength);

    tempString.AppendSubString(*this, s32(pStart - pCurrent), s32(pStart - pCurrent - 1));
    tempString.AppendString(replaceString);
    pLast = pCurrent + searchStringLength;
    nReplacements++;

    pCurrent = std::strstr(pLast, searchString);
  }

  if (pLast != NULL)
    tempString.AppendSubString(*this, s32(pLast - pStart));

  if (nReplacements)
    Swap(tempString);

  return nReplacements;
}

void String::ToLower()
{
  // fixme for utf8
  EnsureOwnWritableCopy();
  for (u32 i = 0; i < m_pStringData->StringLength; i++)
  {
    if (std::isprint(m_pStringData->pBuffer[i]))
      m_pStringData->pBuffer[i] = static_cast<char>(std::tolower(m_pStringData->pBuffer[i]));
  }
}

void String::ToUpper()
{
  // fixme for utf8
  EnsureOwnWritableCopy();
  for (u32 i = 0; i < m_pStringData->StringLength; i++)
  {
    if (std::isprint(m_pStringData->pBuffer[i]))
      m_pStringData->pBuffer[i] = static_cast<char>(std::toupper(m_pStringData->pBuffer[i]));
  }
}

void String::LStrip(const char* szStripCharacters /* = " " */)
{
  u32 stripCharactersLen = static_cast<u32>(std::strlen(szStripCharacters));
  u32 removeCount = 0;
  u32 i = 0;
  u32 j;

  // for each character in str
  for (i = 0; i < m_pStringData->StringLength; i++)
  {
    char ch = m_pStringData->pBuffer[i];

    // if it exists in szStripCharacters
    for (j = 0; j < stripCharactersLen; j++)
    {
      if (ch == szStripCharacters[j])
      {
        removeCount++;
        goto OUTER;
      }
    }

    // not found, exit
    break;
  OUTER:
    continue;
  }

  // chars to remove?
  if (removeCount > 0)
    Erase(0, removeCount);
}

void String::RStrip(const char* szStripCharacters /* = " " */)
{
  u32 stripCharactersLen = static_cast<u32>(std::strlen(szStripCharacters));
  u32 removeCount = 0;
  u32 i = 0;
  u32 j;

  // for each character in str
  for (i = 0; i < m_pStringData->StringLength; i++)
  {
    char ch = m_pStringData->pBuffer[m_pStringData->StringLength - i - 1];

    // if it exists in szStripCharacters
    for (j = 0; j < stripCharactersLen; j++)
    {
      if (ch == szStripCharacters[j])
      {
        removeCount++;
        goto OUTER;
      }
    }

    // not found, exit
    break;
  OUTER:
    continue;
  }

  // chars to remove?
  if (removeCount > 0)
    Erase(m_pStringData->StringLength - removeCount);
}

void String::Strip(const char* szStripCharacters /* = " " */)
{
  RStrip(szStripCharacters);
  LStrip(szStripCharacters);
}

String String::FromFormat(const char* FormatString, ...)
{
  String returnStr;
  va_list ap;

  va_start(ap, FormatString);
  returnStr.FormatVA(FormatString, ap);
  va_end(ap);

  return returnStr;
}
