// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "types.h"

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class Error;
class SmallStringBase;

class BinarySpanReader
{
public:
  BinarySpanReader();
  BinarySpanReader(std::span<const u8> buf);

  BinarySpanReader(const BinarySpanReader&) = delete;
  BinarySpanReader& operator=(const BinarySpanReader&) = delete;

  BinarySpanReader(BinarySpanReader&& move);
  BinarySpanReader& operator=(BinarySpanReader&& move);

  ALWAYS_INLINE const std::span<const u8>& GetSpan() const { return m_buf; }
  ALWAYS_INLINE bool IsValid() const { return !m_buf.empty(); }
  ALWAYS_INLINE bool CheckRemaining(size_t size) { return ((m_pos + size) <= m_buf.size()); }
  ALWAYS_INLINE size_t GetBufferRemaining() const { return (m_buf.size() - m_pos); }
  ALWAYS_INLINE size_t GetBufferConsumed() const { return m_pos; }

  std::span<const u8> GetRemainingSpan() const;
  std::span<const u8> GetRemainingSpan(size_t size) const;
  void IncrementPosition(size_t size);

  // clang-format off
  template<typename T> ALWAYS_INLINE bool ReadT(T* dst) { return Read(dst, sizeof(T)); }
  ALWAYS_INLINE bool ReadBool(bool* dst) { u8 val; if (!Read(&val, sizeof(val))) [[unlikely]] { return false; } *dst = (val != 0); return true; }
  ALWAYS_INLINE bool ReadS8(s8* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU8(u8* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS16(s16* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU16(u16* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS32(s32* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU32(u32* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS64(s64* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU64(u64* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadFloat(float* dst) { return ReadT(dst); }
  bool ReadCString(std::string* dst);
  bool ReadCString(std::string_view* dst);
  bool ReadCString(SmallStringBase* dst);
  bool ReadSizePrefixedString(std::string* dst);
  bool ReadSizePrefixedString(std::string_view* dst);
  bool ReadSizePrefixedString(SmallStringBase* dst);

  template<typename T> ALWAYS_INLINE T ReadT() { T ret; if (!Read(&ret, sizeof(ret))) [[unlikely]] { ret = {}; } return ret; }
  ALWAYS_INLINE bool ReadBool() { return (ReadT<u8>() != 0); }
  ALWAYS_INLINE s8 ReadS8() { return ReadT<s8>(); }
  ALWAYS_INLINE u8 ReadU8() { return ReadT<u8>(); }
  ALWAYS_INLINE s16 ReadS16() { return ReadT<s16>(); }
  ALWAYS_INLINE u16 ReadU16() { return ReadT<u16>(); }
  ALWAYS_INLINE s32 ReadS32() { return ReadT<s32>(); }
  ALWAYS_INLINE u32 ReadU32() { return ReadT<u32>(); }
  ALWAYS_INLINE s64 ReadS64() { return ReadT<s64>(); }
  ALWAYS_INLINE u64 ReadU64() { return ReadT<u64>(); }
  ALWAYS_INLINE float ReadFloat() { return ReadT<float>(); }
  std::string_view ReadCString();
  std::string_view ReadSizePrefixedString();

  template<typename T> ALWAYS_INLINE bool PeekT(T* dst) { return Peek(dst, sizeof(T)); }
  ALWAYS_INLINE bool PeekBool(bool* dst) { u8 val; if (!Peek(&val, sizeof(val))) [[unlikely]] { return false; } *dst = (val != 0); return true; }
  ALWAYS_INLINE bool PeekU8(u8* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU16(u16* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU32(u32* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU64(u64* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekFloat(float* dst) { return PeekT(dst); }
  bool PeekCString(std::string* dst);
  bool PeekCString(std::string_view* dst);
  bool PeekCString(SmallStringBase* dst);
  bool PeekSizePrefixedString(std::string* dst);
  bool PeekSizePrefixedString(std::string_view* dst);
  bool PeekSizePrefixedString(SmallStringBase* dst);

  ALWAYS_INLINE BinarySpanReader& operator>>(s8& val) { val = ReadT<s8>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u8& val) { val = ReadT<u8>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(s16& val) { val = ReadT<s16>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u16& val) { val = ReadT<u16>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(s32& val) { val = ReadT<s32>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u32& val) { val = ReadT<u32>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(s64& val) { val = ReadT<s64>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u64& val) { val = ReadT<u64>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(float& val) { val = ReadT<float>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(std::string_view& val) { val = ReadCString(); return *this; }
  // clang-format on

  template<typename T>
  ALWAYS_INLINE bool ReadOptionalT(std::optional<T>* dst)
  {
    u8 has_value;
    if (!ReadT(&has_value)) [[unlikely]]
      return false;

    if (has_value == 0)
    {
      dst->reset();
      return true;
    }

    T value;
    if (!ReadT(&value)) [[unlikely]]
      return false;

    *dst = value;
    return true;
  }

  ALWAYS_INLINE bool Read(void* buf, size_t size)
  {
    if ((m_pos + size) <= m_buf.size()) [[likely]]
    {
      std::memcpy(buf, &m_buf[m_pos], size);
      m_pos += size;
      return true;
    }

    return false;
  }

  ALWAYS_INLINE bool Peek(void* buf, size_t size)
  {
    if ((m_pos + size) <= m_buf.size()) [[likely]]
    {
      std::memcpy(buf, &m_buf[m_pos], size);
      return true;
    }

    return false;
  }

private:
  std::span<const u8> m_buf;
  size_t m_pos = 0;
};

class BinarySpanWriter
{
public:
  BinarySpanWriter();
  BinarySpanWriter(std::span<u8> buf);

  BinarySpanWriter(const BinarySpanWriter&) = delete;
  BinarySpanWriter& operator=(const BinarySpanWriter&) = delete;

  BinarySpanWriter(BinarySpanWriter&& move);
  BinarySpanWriter& operator=(BinarySpanWriter&& move);

  ALWAYS_INLINE const std::span<u8>& GetSpan() const { return m_buf; }
  ALWAYS_INLINE bool IsValid() const { return !m_buf.empty(); }
  ALWAYS_INLINE size_t GetBufferRemaining() const { return (m_buf.size() - m_pos); }
  ALWAYS_INLINE size_t GetBufferWritten() const { return m_pos; }

  std::span<u8> GetRemainingSpan() const;
  std::span<u8> GetRemainingSpan(size_t size) const;
  void IncrementPosition(size_t size);

  // clang-format off
  template<typename T> ALWAYS_INLINE bool WriteT(T dst) { return Write(&dst, sizeof(T)); }
  ALWAYS_INLINE bool WriteBool(bool val) { const bool bval = static_cast<u8>(val); return Write(&bval, sizeof(bval)); }
  ALWAYS_INLINE bool WriteS8(s8 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU8(u8 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS16(s16 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU16(u16 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS32(s32 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU32(u32 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS64(s64 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU64(u64 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteFloat(float val) { return WriteT(val); }
  bool WriteCString(std::string_view val);
  bool WriteSizePrefixedString(std::string_view val);

  ALWAYS_INLINE BinarySpanWriter& operator<<(s8 val) { WriteS8(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u8 val) { WriteU8(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(s16 val) { WriteS16(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u16 val) { WriteU16(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(s32 val) { WriteS32(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u32 val) { WriteU32(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(s64 val) { WriteS64(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u64 val) { WriteU64(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(float val) { WriteFloat(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(std::string_view val) { WriteCString(val); return *this; }
  // clang-format on

  template<typename T>
  ALWAYS_INLINE bool WriteOptionalT(const std::optional<T>& val)
  {
    return (WriteBool(val.has_value()) && (!val.has_value() || WriteT(val.value())));
  }

  ALWAYS_INLINE bool Write(const void* buf, size_t size)
  {
    if ((m_pos + size) <= m_buf.size()) [[likely]]
    {
      std::memcpy(&m_buf[m_pos], buf, size);
      m_pos += size;
      return true;
    }

    return false;
  }

private:
  std::span<u8> m_buf;
  size_t m_pos = 0;
};

class BinaryFileReader
{
public:
  BinaryFileReader();
  BinaryFileReader(std::FILE* fp);
  
  BinaryFileReader(const BinaryFileReader&) = delete;
  BinaryFileReader& operator=(const BinaryFileReader&) = delete;

  BinaryFileReader(BinaryFileReader&& move);
  BinaryFileReader& operator=(BinaryFileReader&& move);

  ALWAYS_INLINE const std::FILE* GetFile() const { return m_fp; }
  ALWAYS_INLINE bool HasError() const { return !m_good; }
  ALWAYS_INLINE bool IsGood() const { return m_good; }
  ALWAYS_INLINE bool IsOpen() const { return (m_fp != nullptr); }

  bool IsAtEnd();

  // clang-format off
  template<typename T> ALWAYS_INLINE bool ReadT(T* dst) { return Read(dst, sizeof(T)); }
  ALWAYS_INLINE bool ReadBool(bool* dst) { u8 val; if (!Read(&val, sizeof(val))) [[unlikely]] { return false; } *dst = (val != 0); return true; }
  ALWAYS_INLINE bool ReadS8(s8* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU8(u8* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS16(s16* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU16(u16* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS32(s32* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU32(u32* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadS64(s64* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU64(u64* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadFloat(float* dst) { return ReadT(dst); }
  bool ReadCString(std::string* dst);
  bool ReadCString(SmallStringBase* dst);
  bool ReadSizePrefixedString(std::string* dst);
  bool ReadSizePrefixedString(SmallStringBase* dst);

  template<typename T> ALWAYS_INLINE T ReadT() { T ret; if (!Read(&ret, sizeof(ret))) [[unlikely]] { ret = {}; } return ret; }
  ALWAYS_INLINE bool ReadBool() { return (ReadT<u8>() != 0); }
  ALWAYS_INLINE s8 ReadS8() { return ReadT<s8>(); }
  ALWAYS_INLINE u8 ReadU8() { return ReadT<u8>(); }
  ALWAYS_INLINE s16 ReadS16() { return ReadT<s16>(); }
  ALWAYS_INLINE u16 ReadU16() { return ReadT<u16>(); }
  ALWAYS_INLINE s32 ReadS32() { return ReadT<s32>(); }
  ALWAYS_INLINE u32 ReadU32() { return ReadT<u32>(); }
  ALWAYS_INLINE s64 ReadS64() { return ReadT<s64>(); }
  ALWAYS_INLINE u64 ReadU64() { return ReadT<u64>(); }
  ALWAYS_INLINE float ReadFloat() { return ReadT<float>(); }
  std::string ReadCString();
  std::string ReadSizePrefixedString();

  ALWAYS_INLINE BinaryFileReader& operator>>(s8& val) { val = ReadT<s8>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(u8& val) { val = ReadT<u8>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(s16& val) { val = ReadT<s16>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(u16& val) { val = ReadT<u16>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(s32& val) { val = ReadT<s32>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(u32& val) { val = ReadT<u32>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(s64& val) { val = ReadT<s64>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(u64& val) { val = ReadT<u64>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(float& val) { val = ReadT<float>(); return *this; }
  ALWAYS_INLINE BinaryFileReader& operator>>(std::string_view& val) { val = ReadCString(); return *this; }
  // clang-format on

  template<typename T>
  ALWAYS_INLINE bool ReadOptionalT(std::optional<T>* dst)
  {
    u8 has_value;
    if (!ReadT(&has_value)) [[unlikely]]
      return false;

    if (has_value == 0)
    {
      dst->reset();
      return true;
    }

    T value;
    if (!ReadT(&value)) [[unlikely]]
      return false;

    *dst = value;
    return true;
  }

  ALWAYS_INLINE bool Read(void* buf, size_t size) { return (m_good = (m_good && std::fread(buf, size, 1, m_fp) == 1)); }

private:
  std::FILE* m_fp;
  s64 m_size;
  bool m_good = true;
};

class BinaryFileWriter
{
public:
  BinaryFileWriter();
  BinaryFileWriter(std::FILE* fp);
  
  BinaryFileWriter(const BinaryFileWriter&) = delete;
  BinaryFileWriter& operator=(const BinaryFileWriter&) = delete;

  BinaryFileWriter(BinaryFileWriter&& move);
  BinaryFileWriter& operator=(BinaryFileWriter&& move);

  ALWAYS_INLINE const std::FILE* GetFile() const { return m_fp; }
  ALWAYS_INLINE bool HasError() const { return !m_good; }
  ALWAYS_INLINE bool IsGood() const { return m_good; }
  ALWAYS_INLINE bool IsOpen() const { return (m_fp != nullptr); }

  // clang-format off
  template<typename T> ALWAYS_INLINE bool WriteT(T dst) { return Write(&dst, sizeof(T)); }
  ALWAYS_INLINE bool WriteBool(bool val) { const bool bval = static_cast<u8>(val); return Write(&bval, sizeof(bval)); }
  ALWAYS_INLINE bool WriteS8(s8 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU8(u8 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS16(s16 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU16(u16 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS32(s32 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU32(u32 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteS64(s64 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU64(u64 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteFloat(float val) { return WriteT(val); }
  bool WriteCString(std::string_view val);
  bool WriteSizePrefixedString(std::string_view val);

  ALWAYS_INLINE BinaryFileWriter& operator<<(s8 val) { WriteS8(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(u8 val) { WriteU8(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(s16 val) { WriteS16(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(u16 val) { WriteU16(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(s32 val) { WriteS32(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(u32 val) { WriteU32(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(s64 val) { WriteS64(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(u64 val) { WriteU64(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(float val) { WriteFloat(val); return *this; }
  ALWAYS_INLINE BinaryFileWriter& operator<<(std::string_view val) { WriteCString(val); return *this; }
  // clang-format on

  template<typename T>
  ALWAYS_INLINE bool WriteOptionalT(const std::optional<T>& val)
  {
    return (WriteBool(val.has_value()) && (!val.has_value() || WriteT(val.value())));
  }

  ALWAYS_INLINE bool Write(const void* buf, size_t size)
  {
    return (m_good = (m_good && std::fwrite(buf, size, 1, m_fp) == 1));
  }

  bool Flush(Error* error = nullptr);

private:
  std::FILE* m_fp;
  bool m_good;
};
