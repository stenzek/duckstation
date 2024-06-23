// SPDX-FileCopyrightText: 2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

class SmallStringBase;

class BinarySpanReader
{
public:
  BinarySpanReader();
  BinarySpanReader(std::span<const u8> buf);

  ALWAYS_INLINE const std::span<const u8>& GetSpan() const { return m_buf; }
  ALWAYS_INLINE bool IsValid() const { return !m_buf.empty(); }
  ALWAYS_INLINE bool CheckRemaining(size_t size) { return ((m_pos + size) <= m_buf.size()); }
  ALWAYS_INLINE size_t GetBufferRemaining() const { return (m_buf.size() - m_pos); }
  ALWAYS_INLINE size_t GetBufferConsumed() const { return m_pos; }

  // clang-format off
  template<typename T> ALWAYS_INLINE bool ReadT(T* dst) { return Read(dst, sizeof(T)); }
  ALWAYS_INLINE bool ReadU8(u8* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU16(u16* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU32(u32* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadU64(u64* dst) { return ReadT(dst); }
  ALWAYS_INLINE bool ReadFloat(float* dst) { return ReadT(dst); }
  bool ReadCString(std::string* dst);
  bool ReadCString(std::string_view* dst);
  bool ReadCString(SmallStringBase* dst);

  template<typename T> ALWAYS_INLINE T ReadT() { T ret; if (!Read(&ret, sizeof(ret))) [[unlikely]] { ret = {}; } return ret; }
  ALWAYS_INLINE u8 ReadU8() { return ReadT<u8>(); }
  ALWAYS_INLINE u16 ReadU16() { return ReadT<u16>(); }
  ALWAYS_INLINE u32 ReadU32() { return ReadT<u32>(); }
  ALWAYS_INLINE u64 ReadU64() { return ReadT<u64>(); }
  ALWAYS_INLINE float ReadFloat() { return ReadT<float>(); }
  std::string_view ReadCString();

  template<typename T> ALWAYS_INLINE bool PeekT(T* dst) { return Peek(dst, sizeof(T)); }
  ALWAYS_INLINE bool PeekU8(u8* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU16(u16* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU32(u32* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekU64(u64* dst) { return PeekT(dst); }
  ALWAYS_INLINE bool PeekFloat(float* dst) { return PeekT(dst); }
  bool PeekCString(std::string* dst);
  bool PeekCString(std::string_view* dst);
  bool PeekCString(SmallStringBase* dst);

  ALWAYS_INLINE BinarySpanReader& operator>>(u8& val) { val = ReadT<u8>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u16& val) { val = ReadT<u16>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u32& val) { val = ReadT<u32>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(u64& val) { val = ReadT<u64>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(float& val) { val = ReadT<float>(); return *this; }
  ALWAYS_INLINE BinarySpanReader& operator>>(std::string_view val) { val = ReadCString(); return *this; }
  // clang-format on

private:
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

  ALWAYS_INLINE const std::span<u8>& GetSpan() const { return m_buf; }
  ALWAYS_INLINE bool IsValid() const { return !m_buf.empty(); }
  ALWAYS_INLINE size_t GetBufferRemaining() const { return (m_buf.size() - m_pos); }
  ALWAYS_INLINE size_t GetBufferWritten() const { return m_pos; }

  // clang-format off
  template<typename T> ALWAYS_INLINE bool WriteT(T dst) { return Write(&dst, sizeof(T)); }
  ALWAYS_INLINE bool WriteU8(u8 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU16(u16 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU32(u32 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteU64(u64 val) { return WriteT(val); }
  ALWAYS_INLINE bool WriteFloat(float val) { return WriteT(val); }
  bool WriteCString(std::string_view val);

  ALWAYS_INLINE BinarySpanWriter& operator<<(u8 val) { WriteU8(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u16 val) { WriteU16(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u32 val) { WriteU32(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(u64 val) { WriteU64(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(float val) { WriteFloat(val); return *this; }
  ALWAYS_INLINE BinarySpanWriter& operator<<(std::string_view val) { WriteCString(val); return *this; }
  // clang-format on

private:
  ALWAYS_INLINE bool Write(void* buf, size_t size)
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
