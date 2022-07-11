#include "byte_stream.h"
#include "assert.h"
#include "file_system.h"
#include "log.h"
#include "string_util.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#if defined(_WIN32)
#include "windows_headers.h"
#include <direct.h>
#include <io.h>
#include <share.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef _MSC_VER
#include <malloc.h>
#else
#include <alloca.h>
#endif

Log_SetChannel(ByteStream);

class FileByteStream : public ByteStream
{
public:
  FileByteStream(FILE* pFile) : m_pFile(pFile) { DebugAssert(m_pFile != nullptr); }

  virtual ~FileByteStream() { fclose(m_pFile); }

  virtual bool ReadByte(u8* pDestByte) override
  {
    if (m_errorState)
      return false;

    if (fread(pDestByte, 1, 1, m_pFile) != 1)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual u32 Read(void* pDestination, u32 ByteCount) override
  {
    if (m_errorState)
      return 0;

    u32 readCount = (u32)fread(pDestination, 1, ByteCount, m_pFile);
    if (readCount != ByteCount && ferror(m_pFile) != 0)
      m_errorState = true;

    return readCount;
  }

  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */) override
  {
    if (m_errorState)
      return false;

    u32 bytesRead = Read(pDestination, ByteCount);

    if (pNumberOfBytesRead != nullptr)
      *pNumberOfBytesRead = bytesRead;

    if (bytesRead != ByteCount)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool WriteByte(u8 SourceByte) override
  {
    if (m_errorState)
      return false;

    if (fwrite(&SourceByte, 1, 1, m_pFile) != 1)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual u32 Write(const void* pSource, u32 ByteCount) override
  {
    if (m_errorState)
      return 0;

    u32 writeCount = (u32)fwrite(pSource, 1, ByteCount, m_pFile);
    if (writeCount != ByteCount)
      m_errorState = true;

    return writeCount;
  }

  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */) override
  {
    if (m_errorState)
      return false;

    u32 bytesWritten = Write(pSource, ByteCount);

    if (pNumberOfBytesWritten != nullptr)
      *pNumberOfBytesWritten = bytesWritten;

    if (bytesWritten != ByteCount)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

#if defined(_WIN32)

  virtual bool SeekAbsolute(u64 Offset) override
  {
    if (m_errorState)
      return false;

    if (_fseeki64(m_pFile, Offset, SEEK_SET) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool SeekRelative(s64 Offset) override
  {
    if (m_errorState)
      return false;

    if (_fseeki64(m_pFile, Offset, SEEK_CUR) != 0)
    {
      m_errorState = true;
      return true;
    }

    return true;
  }

  virtual bool SeekToEnd() override
  {
    if (m_errorState)
      return false;

    if (_fseeki64(m_pFile, 0, SEEK_END) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual u64 GetPosition() const override { return _ftelli64(m_pFile); }

  virtual u64 GetSize() const override
  {
    s64 OldPos = _ftelli64(m_pFile);
    _fseeki64(m_pFile, 0, SEEK_END);
    s64 Size = _ftelli64(m_pFile);
    _fseeki64(m_pFile, OldPos, SEEK_SET);
    return (u64)Size;
  }

#else

  virtual bool SeekAbsolute(u64 Offset) override
  {
    if (m_errorState)
      return false;

    if (fseeko(m_pFile, static_cast<off_t>(Offset), SEEK_SET) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool SeekRelative(s64 Offset) override
  {
    if (m_errorState)
      return false;

    if (fseeko(m_pFile, static_cast<off_t>(Offset), SEEK_CUR) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool SeekToEnd() override
  {
    if (m_errorState)
      return false;

    if (fseeko(m_pFile, 0, SEEK_END) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual u64 GetPosition() const override { return static_cast<u64>(ftello(m_pFile)); }

  virtual u64 GetSize() const override
  {
    off_t OldPos = ftello(m_pFile);
    fseeko(m_pFile, 0, SEEK_END);
    off_t Size = ftello(m_pFile);
    fseeko(m_pFile, OldPos, SEEK_SET);
    return (u64)Size;
  }

#endif

  virtual bool Flush() override
  {
    if (m_errorState)
      return false;

    if (fflush(m_pFile) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool Commit() override { return true; }

  virtual bool Discard() override { return false; }

protected:
  FILE* m_pFile;
};

class AtomicUpdatedFileByteStream : public FileByteStream
{
public:
  AtomicUpdatedFileByteStream(FILE* pFile, std::string originalFileName, std::string temporaryFileName)
    : FileByteStream(pFile), m_committed(false), m_discarded(false), m_originalFileName(std::move(originalFileName)),
      m_temporaryFileName(std::move(temporaryFileName))
  {
  }

  virtual ~AtomicUpdatedFileByteStream()
  {
    if (m_discarded)
    {
#if defined(_WIN32) && !defined(_UWP)
      // delete the temporary file
      if (!DeleteFileW(StringUtil::UTF8StringToWideString(m_temporaryFileName).c_str()))
      {
        Log_WarningPrintf(
          "AtomicUpdatedFileByteStream::~AtomicUpdatedFileByteStream(): Failed to delete temporary file '%s'",
          m_temporaryFileName.c_str());
      }
#elif defined(_UWP)
      // delete the temporary file
      if (!DeleteFileFromAppW(StringUtil::UTF8StringToWideString(m_temporaryFileName).c_str()))
      {
        Log_WarningPrintf(
          "AtomicUpdatedFileByteStream::~AtomicUpdatedFileByteStream(): Failed to delete temporary file '%s'",
          m_temporaryFileName.c_str());
      }
#else
      // delete the temporary file
      if (remove(m_temporaryFileName.c_str()) < 0)
        Log_WarningPrintf(
          "AtomicUpdatedFileByteStream::~AtomicUpdatedFileByteStream(): Failed to delete temporary file '%s'",
          m_temporaryFileName.c_str());
#endif
    }
    else if (!m_committed)
    {
      Commit();
    }

    // fclose called by FileByteStream destructor
  }

  virtual bool Flush() override
  {
    if (fflush(m_pFile) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool Commit() override
  {
    Assert(!m_discarded);
    if (m_committed)
      return Flush();

    fflush(m_pFile);

#if defined(_WIN32) && !defined(_UWP)
    // move the atomic file name to the original file name
    if (!MoveFileExW(StringUtil::UTF8StringToWideString(m_temporaryFileName).c_str(),
                     StringUtil::UTF8StringToWideString(m_originalFileName).c_str(), MOVEFILE_REPLACE_EXISTING))
    {
      Log_WarningPrintf("AtomicUpdatedFileByteStream::Commit(): Failed to rename temporary file '%s' to '%s'",
                        m_temporaryFileName.c_str(), m_originalFileName.c_str());
      m_discarded = true;
    }
    else
    {
      m_committed = true;
    }
#elif defined(_UWP)
    if (!FileSystem::RenamePath(m_temporaryFileName.c_str(), m_originalFileName.c_str()))
    {
      Log_WarningPrintf("AtomicUpdatedFileByteStream::Commit(): Failed to rename temporary file '%s' to '%s'",
                        m_temporaryFileName.c_str(), m_originalFileName.c_str());
      m_discarded = true;
    }
    else
    {
      m_committed = true;
    }
#else
    // move the atomic file name to the original file name
    if (rename(m_temporaryFileName.c_str(), m_originalFileName.c_str()) < 0)
    {
      Log_WarningPrintf("AtomicUpdatedFileByteStream::Commit(): Failed to rename temporary file '%s' to '%s'",
                        m_temporaryFileName.c_str(), m_originalFileName.c_str());
      m_discarded = true;
    }
    else
    {
      m_committed = true;
    }
#endif

    return (!m_discarded);
  }

  virtual bool Discard() override
  {
    Assert(!m_committed);
    m_discarded = true;
    return true;
  }

private:
  bool m_committed;
  bool m_discarded;
  std::string m_originalFileName;
  std::string m_temporaryFileName;
};

NullByteStream::NullByteStream() {}

NullByteStream::~NullByteStream() {}

bool NullByteStream::ReadByte(u8* pDestByte)
{
  *pDestByte = 0;
  return true;
}

u32 NullByteStream::Read(void* pDestination, u32 ByteCount)
{
  if (ByteCount > 0)
    std::memset(pDestination, 0, ByteCount);

  return ByteCount;
}

bool NullByteStream::Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */)
{
  if (ByteCount > 0)
    std::memset(pDestination, 0, ByteCount);

  if (pNumberOfBytesRead)
    *pNumberOfBytesRead = ByteCount;

  return true;
}

bool NullByteStream::WriteByte(u8 SourceByte)
{
  return true;
}

u32 NullByteStream::Write(const void* pSource, u32 ByteCount)
{
  return ByteCount;
}

bool NullByteStream::Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */)
{
  return true;
}

bool NullByteStream::SeekAbsolute(u64 Offset)
{
  return true;
}

bool NullByteStream::SeekRelative(s64 Offset)
{
  return true;
}

bool NullByteStream::SeekToEnd()
{
  return true;
}

u64 NullByteStream::GetSize() const
{
  return 0;
}

u64 NullByteStream::GetPosition() const
{
  return 0;
}

bool NullByteStream::Flush()
{
  return true;
}

bool NullByteStream::Commit()
{
  return true;
}

bool NullByteStream::Discard()
{
  return true;
}

MemoryByteStream::MemoryByteStream(void* pMemory, u32 MemSize)
{
  m_iPosition = 0;
  m_iSize = MemSize;
  m_pMemory = (u8*)pMemory;
}

MemoryByteStream::~MemoryByteStream() {}

bool MemoryByteStream::ReadByte(u8* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

u32 MemoryByteStream::Read(void* pDestination, u32 ByteCount)
{
  u32 sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool MemoryByteStream::Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */)
{
  u32 r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != NULL)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool MemoryByteStream::WriteByte(u8 SourceByte)
{
  if (m_iPosition < m_iSize)
  {
    m_pMemory[m_iPosition++] = SourceByte;
    return true;
  }

  return false;
}

u32 MemoryByteStream::Write(const void* pSource, u32 ByteCount)
{
  u32 sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(m_pMemory + m_iPosition, pSource, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool MemoryByteStream::Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */)
{
  u32 r = Write(pSource, ByteCount);
  if (pNumberOfBytesWritten != nullptr)
    *pNumberOfBytesWritten = r;

  return (r == ByteCount);
}

bool MemoryByteStream::SeekAbsolute(u64 Offset)
{
  u32 Offset32 = (u32)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

bool MemoryByteStream::SeekRelative(s64 Offset)
{
  s32 Offset32 = (s32)Offset;
  if ((Offset32 < 0 && -Offset32 > (s32)m_iPosition) || (u32)((s32)m_iPosition + Offset32) > m_iSize)
    return false;

  m_iPosition += Offset32;
  return true;
}

bool MemoryByteStream::SeekToEnd()
{
  m_iPosition = m_iSize;
  return true;
}

u64 MemoryByteStream::GetSize() const
{
  return (u64)m_iSize;
}

u64 MemoryByteStream::GetPosition() const
{
  return (u64)m_iPosition;
}

bool MemoryByteStream::Flush()
{
  return true;
}

bool MemoryByteStream::Commit()
{
  return true;
}

bool MemoryByteStream::Discard()
{
  return false;
}

ReadOnlyMemoryByteStream::ReadOnlyMemoryByteStream(const void* pMemory, u32 MemSize)
{
  m_iPosition = 0;
  m_iSize = MemSize;
  m_pMemory = reinterpret_cast<const u8*>(pMemory);
}

ReadOnlyMemoryByteStream::~ReadOnlyMemoryByteStream() {}

bool ReadOnlyMemoryByteStream::ReadByte(u8* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

u32 ReadOnlyMemoryByteStream::Read(void* pDestination, u32 ByteCount)
{
  u32 sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool ReadOnlyMemoryByteStream::Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */)
{
  u32 r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != nullptr)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool ReadOnlyMemoryByteStream::WriteByte(u8 SourceByte)
{
  return false;
}

u32 ReadOnlyMemoryByteStream::Write(const void* pSource, u32 ByteCount)
{
  return 0;
}

bool ReadOnlyMemoryByteStream::Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */)
{
  return false;
}

bool ReadOnlyMemoryByteStream::SeekAbsolute(u64 Offset)
{
  u32 Offset32 = (u32)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

bool ReadOnlyMemoryByteStream::SeekRelative(s64 Offset)
{
  s32 Offset32 = (s32)Offset;
  if ((Offset32 < 0 && -Offset32 > (s32)m_iPosition) || (u32)((s32)m_iPosition + Offset32) > m_iSize)
    return false;

  m_iPosition += Offset32;
  return true;
}

bool ReadOnlyMemoryByteStream::SeekToEnd()
{
  m_iPosition = m_iSize;
  return true;
}

u64 ReadOnlyMemoryByteStream::GetSize() const
{
  return (u64)m_iSize;
}

u64 ReadOnlyMemoryByteStream::GetPosition() const
{
  return (u64)m_iPosition;
}

bool ReadOnlyMemoryByteStream::Flush()
{
  return false;
}

bool ReadOnlyMemoryByteStream::Commit()
{
  return false;
}

bool ReadOnlyMemoryByteStream::Discard()
{
  return false;
}

GrowableMemoryByteStream::GrowableMemoryByteStream(void* pInitialMem, u32 InitialMemSize)
{
  m_iPosition = 0;
  m_iSize = 0;

  if (pInitialMem != nullptr)
  {
    m_iMemorySize = InitialMemSize;
    m_pPrivateMemory = nullptr;
    m_pMemory = (u8*)pInitialMem;
  }
  else
  {
    m_iMemorySize = std::max(InitialMemSize, (u32)64);
    m_pPrivateMemory = m_pMemory = (u8*)std::malloc(m_iMemorySize);
  }
}

GrowableMemoryByteStream::~GrowableMemoryByteStream()
{
  if (m_pPrivateMemory != nullptr)
    std::free(m_pPrivateMemory);
}

void GrowableMemoryByteStream::Resize(u32 new_size)
{
  if (new_size > m_iMemorySize)
    ResizeMemory(new_size);

  m_iSize = new_size;
}

void GrowableMemoryByteStream::ResizeMemory(u32 new_size)
{
  if (new_size == m_iMemorySize)
    return;

  if (m_pPrivateMemory == nullptr)
  {
    m_pPrivateMemory = (u8*)std::malloc(new_size);
    std::memcpy(m_pPrivateMemory, m_pMemory, m_iSize);
    m_pMemory = m_pPrivateMemory;
    m_iMemorySize = new_size;
  }
  else
  {
    m_pPrivateMemory = m_pMemory = (u8*)std::realloc(m_pPrivateMemory, new_size);
    m_iMemorySize = new_size;
  }
}

void GrowableMemoryByteStream::EnsureSpace(u32 space)
{
  if ((m_iSize + space) >= m_iMemorySize)
    return;

  Grow((m_iSize + space) - m_iMemorySize);
}

void GrowableMemoryByteStream::ShrinkToFit()
{
  if (!m_pPrivateMemory || m_iSize == m_iMemorySize)
    return;

  u8* new_ptr = static_cast<u8*>(std::realloc(m_pPrivateMemory, m_iSize));
  if (new_ptr)
  {
    m_pPrivateMemory = new_ptr;
    m_iMemorySize = m_iSize;
  }
}

bool GrowableMemoryByteStream::ReadByte(u8* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

u32 GrowableMemoryByteStream::Read(void* pDestination, u32 ByteCount)
{
  u32 sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool GrowableMemoryByteStream::Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */)
{
  u32 r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != NULL)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool GrowableMemoryByteStream::WriteByte(u8 SourceByte)
{
  if (m_iPosition == m_iMemorySize)
    Grow(1);

  m_pMemory[m_iPosition++] = SourceByte;
  m_iSize = std::max(m_iSize, m_iPosition);
  return true;
}

u32 GrowableMemoryByteStream::Write(const void* pSource, u32 ByteCount)
{
  if ((m_iPosition + ByteCount) > m_iMemorySize)
    Grow(ByteCount);

  std::memcpy(m_pMemory + m_iPosition, pSource, ByteCount);
  m_iPosition += ByteCount;
  m_iSize = std::max(m_iSize, m_iPosition);
  return ByteCount;
}

bool GrowableMemoryByteStream::Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */)
{
  u32 r = Write(pSource, ByteCount);
  if (pNumberOfBytesWritten != nullptr)
    *pNumberOfBytesWritten = r;

  return (r == ByteCount);
}

bool GrowableMemoryByteStream::SeekAbsolute(u64 Offset)
{
  u32 Offset32 = (u32)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

bool GrowableMemoryByteStream::SeekRelative(s64 Offset)
{
  s32 Offset32 = (s32)Offset;
  if ((Offset32 < 0 && -Offset32 > (s32)m_iPosition) || (u32)((s32)m_iPosition + Offset32) > m_iSize)
    return false;

  m_iPosition += Offset32;
  return true;
}

bool GrowableMemoryByteStream::SeekToEnd()
{
  m_iPosition = m_iSize;
  return true;
}

u64 GrowableMemoryByteStream::GetSize() const
{
  return (u64)m_iSize;
}

u64 GrowableMemoryByteStream::GetPosition() const
{
  return (u64)m_iPosition;
}

bool GrowableMemoryByteStream::Flush()
{
  return true;
}

bool GrowableMemoryByteStream::Commit()
{
  return true;
}

bool GrowableMemoryByteStream::Discard()
{
  return false;
}

void GrowableMemoryByteStream::Grow(u32 MinimumGrowth)
{
  u32 NewSize = std::max(m_iMemorySize + MinimumGrowth, m_iMemorySize * 2);
  ResizeMemory(NewSize);
}

bool ByteStream::ReadU8(u8* dest)
{
  return Read2(dest, sizeof(u8));
}

bool ByteStream::ReadU16(u16* dest)
{
  return Read2(dest, sizeof(u16));
}

bool ByteStream::ReadU32(u32* dest)
{
  return Read2(dest, sizeof(u32));
}

bool ByteStream::ReadU64(u64* dest)
{
  return Read2(dest, sizeof(u64));
}

bool ByteStream::ReadS8(s8* dest)
{
  return Read2(dest, sizeof(s8));
}

bool ByteStream::ReadS16(s16* dest)
{
  return Read2(dest, sizeof(s16));
}

bool ByteStream::ReadS32(s32* dest)
{
  return Read2(dest, sizeof(s32));
}

bool ByteStream::ReadS64(s64* dest)
{
  return Read2(dest, sizeof(s64));
}

bool ByteStream::ReadSizePrefixedString(std::string* dest)
{
  u32 size;
  if (!Read2(&size, sizeof(size)))
    return false;

  dest->resize(size);
  if (!Read2(dest->data(), size))
    return false;

  return true;
}

bool ByteStream::WriteU8(u8 dest)
{
  return Write2(&dest, sizeof(u8));
}

bool ByteStream::WriteU16(u16 dest)
{
  return Write2(&dest, sizeof(u16));
}

bool ByteStream::WriteU32(u32 dest)
{
  return Write2(&dest, sizeof(u32));
}

bool ByteStream::WriteU64(u64 dest)
{
  return Write2(&dest, sizeof(u64));
}

bool ByteStream::WriteS8(s8 dest)
{
  return Write2(&dest, sizeof(s8));
}

bool ByteStream::WriteS16(s16 dest)
{
  return Write2(&dest, sizeof(s16));
}

bool ByteStream::WriteS32(s32 dest)
{
  return Write2(&dest, sizeof(s32));
}

bool ByteStream::WriteS64(s64 dest)
{
  return Write2(&dest, sizeof(s64));
}

bool ByteStream::WriteSizePrefixedString(const std::string_view& str)
{
  const u32 size = static_cast<u32>(str.size());
  return (Write2(&size, sizeof(size)) && (size == 0 || Write2(str.data(), size)));
}

std::unique_ptr<ByteStream> ByteStream::OpenFile(const char* fileName, u32 openMode)
{
  if ((openMode & (BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE)) == BYTESTREAM_OPEN_WRITE)
  {
    // if opening with write but not create, the path must exist.
    if (!FileSystem::FileExists(fileName))
      return nullptr;
  }

  char modeString[16];
  u32 modeStringLength = 0;

  if (openMode & BYTESTREAM_OPEN_WRITE)
  {
    // if the file exists, use r+, otherwise w+
    // HACK: if we're not truncating, and the file exists (we want to only update it), we still have to use r+
    if (!FileSystem::FileExists(fileName))
    {
      modeString[modeStringLength++] = 'w';
      if (openMode & BYTESTREAM_OPEN_READ)
        modeString[modeStringLength++] = '+';
    }
    else
    {
      modeString[modeStringLength++] = 'r';
      modeString[modeStringLength++] = '+';
    }

    modeString[modeStringLength++] = 'b';
  }
  else if (openMode & BYTESTREAM_OPEN_READ)
  {
    modeString[modeStringLength++] = 'r';
    modeString[modeStringLength++] = 'b';
  }

  // doesn't work with _fdopen
  if (!(openMode & BYTESTREAM_OPEN_ATOMIC_UPDATE))
  {
    if (openMode & BYTESTREAM_OPEN_STREAMED)
      modeString[modeStringLength++] = 'S';
    else if (openMode & BYTESTREAM_OPEN_SEEKABLE)
      modeString[modeStringLength++] = 'R';
  }

  modeString[modeStringLength] = 0;

  if (openMode & BYTESTREAM_OPEN_ATOMIC_UPDATE)
  {
    DebugAssert(openMode & (BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE));
#ifdef _WIN32
#ifndef _UWP
    // generate the temporary file name
    u32 fileNameLength = static_cast<u32>(std::strlen(fileName));
    char* temporaryFileName = (char*)alloca(fileNameLength + 8);
    std::snprintf(temporaryFileName, fileNameLength + 8, "%s.XXXXXX", fileName);

    // fill in random characters
    _mktemp_s(temporaryFileName, fileNameLength + 8);
    const std::wstring wideTemporaryFileName(StringUtil::UTF8StringToWideString(temporaryFileName));
#else
    // On UWP, preserve the extension, as it affects permissions.
    std::string temporaryFileName;
    const char* extension = std::strrchr(fileName, '.');
    if (extension)
      temporaryFileName.append(fileName, extension - fileName);
    else
      temporaryFileName.append(fileName);

    temporaryFileName.append("_XXXXXX");
    _mktemp_s(temporaryFileName.data(), temporaryFileName.size() + 1);
    if (extension)
      temporaryFileName.append(extension);

    const std::wstring wideTemporaryFileName(StringUtil::UTF8StringToWideString(temporaryFileName));
#endif

    // massive hack here
    DWORD desiredAccess = GENERIC_WRITE;
    if (openMode & BYTESTREAM_OPEN_READ)
      desiredAccess |= GENERIC_READ;

#ifndef _UWP
    HANDLE hFile =
      CreateFileW(wideTemporaryFileName.c_str(), desiredAccess, FILE_SHARE_DELETE, NULL, CREATE_NEW, 0, NULL);
#else
    HANDLE hFile =
      CreateFile2FromAppW(wideTemporaryFileName.c_str(), desiredAccess, FILE_SHARE_DELETE, CREATE_NEW, nullptr);
#endif

    if (hFile == INVALID_HANDLE_VALUE)
      return nullptr;

    // get fd from this
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hFile), 0);
    if (fd < 0)
    {
      CloseHandle(hFile);
      DeleteFileW(wideTemporaryFileName.c_str());
      return nullptr;
    }

    // convert to a stream
    FILE* pTemporaryFile = _fdopen(fd, modeString);
    if (!pTemporaryFile)
    {
      _close(fd);
      DeleteFileW(wideTemporaryFileName.c_str());
      return nullptr;
    }

    // create the stream pointer
    std::unique_ptr<AtomicUpdatedFileByteStream> pStream =
      std::make_unique<AtomicUpdatedFileByteStream>(pTemporaryFile, fileName, temporaryFileName);

    // do we need to copy the existing file into this one?
    if (!(openMode & BYTESTREAM_OPEN_TRUNCATE))
    {
      FILE* pOriginalFile = FileSystem::OpenCFile(fileName, "rb");
      if (!pOriginalFile)
      {
        // this will delete the temporary file
        pStream->Discard();
        return nullptr;
      }

      static const size_t BUFFERSIZE = 4096;
      u8 buffer[BUFFERSIZE];
      while (!feof(pOriginalFile))
      {
        size_t nBytes = fread(buffer, BUFFERSIZE, sizeof(u8), pOriginalFile);
        if (nBytes == 0)
          break;

        if (pStream->Write(buffer, (u32)nBytes) != (u32)nBytes)
        {
          pStream->Discard();
          fclose(pOriginalFile);
          return nullptr;
        }
      }

      // close original file
      fclose(pOriginalFile);
    }

    // return pointer
    return pStream;
#else
    DebugAssert(openMode & (BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE));

    // generate the temporary file name
    const u32 fileNameLength = static_cast<u32>(std::strlen(fileName));
    char* temporaryFileName = (char*)alloca(fileNameLength + 8);
    std::snprintf(temporaryFileName, fileNameLength + 8, "%s.XXXXXX", fileName);

    // fill in random characters
#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
    mkstemp(temporaryFileName);
#else
    mktemp(temporaryFileName);
#endif

    // open the file
    std::FILE* pTemporaryFile = FileSystem::OpenCFile(temporaryFileName, modeString);
    if (pTemporaryFile == nullptr)
      return nullptr;

    // create the stream pointer
    std::unique_ptr<AtomicUpdatedFileByteStream> pStream =
      std::make_unique<AtomicUpdatedFileByteStream>(pTemporaryFile, fileName, temporaryFileName);

    // do we need to copy the existing file into this one?
    if (!(openMode & BYTESTREAM_OPEN_TRUNCATE))
    {
      std::FILE* pOriginalFile = FileSystem::OpenCFile(fileName, "rb");
      if (!pOriginalFile)
      {
        // this will delete the temporary file
        pStream->SetErrorState();
        return nullptr;
      }

      static const size_t BUFFERSIZE = 4096;
      u8 buffer[BUFFERSIZE];
      while (!std::feof(pOriginalFile))
      {
        size_t nBytes = std::fread(buffer, BUFFERSIZE, sizeof(u8), pOriginalFile);
        if (nBytes == 0)
          break;

        if (pStream->Write(buffer, (u32)nBytes) != (u32)nBytes)
        {
          pStream->SetErrorState();
          std::fclose(pOriginalFile);
          return nullptr;
        }
      }

      // close original file
      std::fclose(pOriginalFile);
    }

    // return pointer
    return pStream;
#endif
  }
  else
  {
    // forward through
    std::FILE* pFile = FileSystem::OpenCFile(fileName, modeString);
    if (!pFile)
      return nullptr;

    return std::make_unique<FileByteStream>(pFile);
  }
}

std::unique_ptr<MemoryByteStream> ByteStream::CreateMemoryStream(void* pMemory, u32 Size)
{
  DebugAssert(pMemory != nullptr && Size > 0);
  return std::make_unique<MemoryByteStream>(pMemory, Size);
}

std::unique_ptr<ReadOnlyMemoryByteStream> ByteStream::CreateReadOnlyMemoryStream(const void* pMemory, u32 Size)
{
  DebugAssert(pMemory != nullptr && Size > 0);
  return std::make_unique<ReadOnlyMemoryByteStream>(pMemory, Size);
}

std::unique_ptr<NullByteStream> ByteStream::CreateNullStream()
{
  return std::make_unique<NullByteStream>();
}

std::unique_ptr<GrowableMemoryByteStream> ByteStream::CreateGrowableMemoryStream(void* pInitialMemory, u32 InitialSize)
{
  return std::make_unique<GrowableMemoryByteStream>(pInitialMemory, InitialSize);
}

std::unique_ptr<GrowableMemoryByteStream> ByteStream::CreateGrowableMemoryStream()
{
  return std::make_unique<GrowableMemoryByteStream>(nullptr, 0);
}

bool ByteStream::CopyStream(ByteStream* pDestinationStream, ByteStream* pSourceStream)
{
  const u32 chunkSize = 4096;
  u8 chunkData[chunkSize];

  u64 oldSourcePosition = pSourceStream->GetPosition();
  if (!pSourceStream->SeekAbsolute(0) || !pDestinationStream->SeekAbsolute(0))
    return false;

  bool success = false;
  for (;;)
  {
    u32 nBytes = pSourceStream->Read(chunkData, chunkSize);
    if (nBytes == 0)
    {
      success = true;
      break;
    }

    if (pDestinationStream->Write(chunkData, nBytes) != nBytes)
      break;
  }

  return (pSourceStream->SeekAbsolute(oldSourcePosition) && success);
}

bool ByteStream::AppendStream(ByteStream* pSourceStream, ByteStream* pDestinationStream)
{
  const u32 chunkSize = 4096;
  u8 chunkData[chunkSize];

  u64 oldSourcePosition = pSourceStream->GetPosition();
  if (!pSourceStream->SeekAbsolute(0))
    return false;

  bool success = false;
  for (;;)
  {
    u32 nBytes = pSourceStream->Read(chunkData, chunkSize);
    if (nBytes == 0)
    {
      success = true;
      break;
    }

    if (pDestinationStream->Write(chunkData, nBytes) != nBytes)
      break;
  }

  return (pSourceStream->SeekAbsolute(oldSourcePosition) && success);
}

u32 ByteStream::CopyBytes(ByteStream* pSourceStream, u32 byteCount, ByteStream* pDestinationStream)
{
  const u32 chunkSize = 4096;
  u8 chunkData[chunkSize];

  u32 remaining = byteCount;
  while (remaining > 0)
  {
    u32 toCopy = std::min(remaining, chunkSize);
    u32 bytesRead = pSourceStream->Read(chunkData, toCopy);
    if (bytesRead == 0)
      break;

    u32 bytesWritten = pDestinationStream->Write(chunkData, bytesRead);
    if (bytesWritten == 0)
      break;

    remaining -= bytesWritten;
  }

  return byteCount - remaining;
}

std::string ByteStream::ReadStreamToString(ByteStream* stream, bool seek_to_start /* = true */)
{
  u64 pos = stream->GetPosition();
  u64 size = stream->GetSize();
  if (pos > 0 && seek_to_start)
  {
    if (!stream->SeekAbsolute(0))
      return {};

    pos = 0;
  }

  Assert(size >= pos);
  size -= pos;
  if (size == 0 || size > std::numeric_limits<u32>::max())
    return {};

  std::string ret;
  ret.resize(static_cast<size_t>(size));
  if (!stream->Read2(ret.data(), static_cast<u32>(size)))
    return {};

  return ret;
}

bool ByteStream::WriteStreamToString(const std::string_view& sv, ByteStream* stream)
{
  if (sv.size() > std::numeric_limits<u32>::max())
    return false;

  return stream->Write2(sv.data(), static_cast<u32>(sv.size()));
}

std::vector<u8> ByteStream::ReadBinaryStream(ByteStream* stream, bool seek_to_start /*= true*/)
{
  u64 pos = stream->GetPosition();
  u64 size = stream->GetSize();
  if (pos > 0 && seek_to_start)
  {
    if (!stream->SeekAbsolute(0))
      return {};

    pos = 0;
  }

  Assert(size >= pos);
  size -= pos;
  if (size == 0 || size > std::numeric_limits<u32>::max())
    return {};

  std::vector<u8> ret;
  ret.resize(static_cast<size_t>(size));
  if (!stream->Read2(ret.data(), static_cast<u32>(size)))
    return {};

  return ret;
}

bool ByteStream::WriteBinaryToStream(ByteStream* stream, const void* data, size_t data_length)
{
  if (data_length > std::numeric_limits<u32>::max())
    return false;

  return stream->Write2(data, static_cast<u32>(data_length));
}
