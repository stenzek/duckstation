#pragma once
#include "types.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// base byte stream creation functions
enum BYTESTREAM_OPEN_MODE
{
  BYTESTREAM_OPEN_READ = 1,           // open stream for writing
  BYTESTREAM_OPEN_WRITE = 2,          // open stream for writing
  BYTESTREAM_OPEN_APPEND = 4,         // seek to the end
  BYTESTREAM_OPEN_TRUNCATE = 8,       // truncate the file, seek to start
  BYTESTREAM_OPEN_CREATE = 16,        // if the file does not exist, create it
  BYTESTREAM_OPEN_ATOMIC_UPDATE = 64, //
  BYTESTREAM_OPEN_SEEKABLE = 128,
  BYTESTREAM_OPEN_STREAMED = 256,
};

// forward declarations for implemented classes
class ByteStream;
class MemoryByteStream;
class GrowableMemoryByteStream;
class ReadOnlyMemoryByteStream;
class NullByteStream;

// interface class used by readers, writers, etc.
class ByteStream
{
public:
  virtual ~ByteStream() {}

  // reads a single byte from the stream.
  virtual bool ReadByte(u8* pDestByte) = 0;

  // read bytes from this stream. returns the number of bytes read, if this isn't equal to the requested size, an error
  // or EOF occurred.
  virtual u32 Read(void* pDestination, u32 ByteCount) = 0;

  // read bytes from this stream, optionally returning the number of bytes read.
  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead = nullptr) = 0;

  // writes a single byte to the stream.
  virtual bool WriteByte(u8 SourceByte) = 0;

  // write bytes to this stream, returns the number of bytes written. if this isn't equal to the requested size, a
  // buffer overflow, or write error occurred.
  virtual u32 Write(const void* pSource, u32 ByteCount) = 0;

  // write bytes to this stream, optionally returning the number of bytes written.
  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten = nullptr) = 0;

  // seeks to the specified position in the stream
  // if seek failed, returns false.
  virtual bool SeekAbsolute(u64 Offset) = 0;
  virtual bool SeekRelative(s64 Offset) = 0;
  virtual bool SeekToEnd() = 0;

  // gets the current offset in the stream
  virtual u64 GetPosition() const = 0;

  // gets the size of the stream
  virtual u64 GetSize() const = 0;

  // flush any changes to the stream to disk
  virtual bool Flush() = 0;

  // if the file was opened in atomic update mode, discards any changes made to the file
  virtual bool Discard() = 0;

  // if the file was opened in atomic update mode, commits the file and replaces the temporary file
  virtual bool Commit() = 0;

  // state accessors
  inline bool InErrorState() const { return m_errorState; }
  inline void SetErrorState() { m_errorState = true; }
  inline void ClearErrorState() { m_errorState = false; }

  bool ReadU8(u8* dest);
  bool ReadU16(u16* dest);
  bool ReadU32(u32* dest);
  bool ReadU64(u64* dest);
  bool ReadS8(s8* dest);
  bool ReadS16(s16* dest);
  bool ReadS32(s32* dest);
  bool ReadS64(s64* dest);
  bool ReadSizePrefixedString(std::string* dest);

  bool WriteU8(u8 dest);
  bool WriteU16(u16 dest);
  bool WriteU32(u32 dest);
  bool WriteU64(u64 dest);
  bool WriteS8(s8 dest);
  bool WriteS16(s16 dest);
  bool WriteS32(s32 dest);
  bool WriteS64(s64 dest);
  bool WriteSizePrefixedString(const std::string_view& str);

  // base byte stream creation functions
  // opens a local file-based stream. fills in error if passed, and returns false if the file cannot be opened.
  static std::unique_ptr<ByteStream> OpenFile(const char* FileName, u32 OpenMode);

  // memory byte stream, caller is responsible for management, therefore it can be located on either the stack or on the
  // heap.
  static std::unique_ptr<MemoryByteStream> CreateMemoryStream(void* pMemory, u32 Size);

  // a growable memory byte stream will automatically allocate its own memory if the provided memory is overflowed.
  // a "pure heap" buffer, i.e. a buffer completely managed by this implementation, can be created by supplying a NULL
  // pointer and initialSize of zero.
  static std::unique_ptr<GrowableMemoryByteStream> CreateGrowableMemoryStream(void* pInitialMemory, u32 InitialSize);
  static std::unique_ptr<GrowableMemoryByteStream> CreateGrowableMemoryStream();

  // readable memory stream
  static std::unique_ptr<ReadOnlyMemoryByteStream> CreateReadOnlyMemoryStream(const void* pMemory, u32 Size);

  // null memory stream
  static std::unique_ptr<NullByteStream> CreateNullStream();

  // copies one stream's contents to another. rewinds source streams automatically, and returns it back to its old
  // position.
  static bool CopyStream(ByteStream* pDestinationStream, ByteStream* pSourceStream);

  // appends one stream's contents to another.
  static bool AppendStream(ByteStream* pSourceStream, ByteStream* pDestinationStream);

  // copies a number of bytes from one to another
  static u32 CopyBytes(ByteStream* pSourceStream, u32 byteCount, ByteStream* pDestinationStream);

  static std::string ReadStreamToString(ByteStream* stream, bool seek_to_start = true);
  static bool WriteStreamToString(const std::string_view& sv, ByteStream* stream);

  static std::vector<u8> ReadBinaryStream(ByteStream* stream, bool seek_to_start = true);
  static bool WriteBinaryToStream(ByteStream* stream, const void* data, size_t data_length);

protected:
  ByteStream() : m_errorState(false) {}

  // state bits
  bool m_errorState;

  // make it noncopyable
  ByteStream(const ByteStream&) = delete;
  ByteStream& operator=(const ByteStream&) = delete;
};

class NullByteStream : public ByteStream
{
public:
  NullByteStream();
  ~NullByteStream();

  virtual bool ReadByte(u8* pDestByte) override final;
  virtual u32 Read(void* pDestination, u32 ByteCount) override final;
  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */) override final;
  virtual bool WriteByte(u8 SourceByte) override final;
  virtual u32 Write(const void* pSource, u32 ByteCount) override final;
  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */) override final;
  virtual bool SeekAbsolute(u64 Offset) override final;
  virtual bool SeekRelative(s64 Offset) override final;
  virtual bool SeekToEnd() override final;
  virtual u64 GetSize() const override final;
  virtual u64 GetPosition() const override final;
  virtual bool Flush() override final;
  virtual bool Commit() override final;
  virtual bool Discard() override final;
};

class MemoryByteStream : public ByteStream
{
public:
  MemoryByteStream(void* pMemory, u32 MemSize);
  virtual ~MemoryByteStream();

  u8* GetMemoryPointer() const { return m_pMemory; }
  u32 GetMemorySize() const { return m_iSize; }

  virtual bool ReadByte(u8* pDestByte) override;
  virtual u32 Read(void* pDestination, u32 ByteCount) override;
  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */) override;
  virtual bool WriteByte(u8 SourceByte) override;
  virtual u32 Write(const void* pSource, u32 ByteCount) override;
  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */) override;
  virtual bool SeekAbsolute(u64 Offset) override;
  virtual bool SeekRelative(s64 Offset) override;
  virtual bool SeekToEnd() override;
  virtual u64 GetSize() const override;
  virtual u64 GetPosition() const override;
  virtual bool Flush() override;
  virtual bool Commit() override;
  virtual bool Discard() override;

private:
  u8* m_pMemory;
  u32 m_iPosition;
  u32 m_iSize;
};

class ReadOnlyMemoryByteStream : public ByteStream
{
public:
  ReadOnlyMemoryByteStream(const void* pMemory, u32 MemSize);
  virtual ~ReadOnlyMemoryByteStream();

  const u8* GetMemoryPointer() const { return m_pMemory; }
  u32 GetMemorySize() const { return m_iSize; }

  virtual bool ReadByte(u8* pDestByte) override;
  virtual u32 Read(void* pDestination, u32 ByteCount) override;
  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */) override;
  virtual bool WriteByte(u8 SourceByte) override;
  virtual u32 Write(const void* pSource, u32 ByteCount) override;
  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */) override;
  virtual bool SeekAbsolute(u64 Offset) override;
  virtual bool SeekRelative(s64 Offset) override;
  virtual bool SeekToEnd() override;
  virtual u64 GetSize() const override;
  virtual u64 GetPosition() const override;
  virtual bool Flush() override;
  virtual bool Commit() override;
  virtual bool Discard() override;

private:
  const u8* m_pMemory;
  u32 m_iPosition;
  u32 m_iSize;
};

class GrowableMemoryByteStream : public ByteStream
{
public:
  GrowableMemoryByteStream(void* pInitialMem, u32 InitialMemSize);
  virtual ~GrowableMemoryByteStream();

  u8* GetMemoryPointer() const { return m_pMemory; }
  u32 GetMemorySize() const { return m_iMemorySize; }

  void Resize(u32 new_size);
  void ResizeMemory(u32 new_size);
  void EnsureSpace(u32 space);
  void ShrinkToFit();

  virtual bool ReadByte(u8* pDestByte) override;
  virtual u32 Read(void* pDestination, u32 ByteCount) override;
  virtual bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead /* = nullptr */) override;
  virtual bool WriteByte(u8 SourceByte) override;
  virtual u32 Write(const void* pSource, u32 ByteCount) override;
  virtual bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten /* = nullptr */) override;
  virtual bool SeekAbsolute(u64 Offset) override;
  virtual bool SeekRelative(s64 Offset) override;
  virtual bool SeekToEnd() override;
  virtual u64 GetSize() const override;
  virtual u64 GetPosition() const override;
  virtual bool Flush() override;
  virtual bool Commit() override;
  virtual bool Discard() override;

private:
  void Grow(u32 MinimumGrowth);

  u8* m_pPrivateMemory;
  u8* m_pMemory;
  u32 m_iPosition;
  u32 m_iSize;
  u32 m_iMemorySize;
};
