// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/fifo_queue.h"
#include "common/heap_array.h"
#include "common/types.h"

#include <cstring>
#include <deque>
#include <string>
#include <span>
#include <type_traits>
#include <vector>

class SmallStringBase;

class StateWrapper
{
public:
  enum class Mode
  {
    Read,
    Write
  };

  StateWrapper(std::span<u8> data, Mode mode, u32 version);
  StateWrapper(std::span<const u8> data, Mode mode, u32 version);
  StateWrapper(const StateWrapper&) = delete;
  ~StateWrapper();

  ALWAYS_INLINE bool HasError() const { return m_error; }
  ALWAYS_INLINE bool IsReading() const { return (m_mode == Mode::Read); }
  ALWAYS_INLINE bool IsWriting() const { return (m_mode == Mode::Write); }
  ALWAYS_INLINE u32 GetVersion() const { return m_version; }
  ALWAYS_INLINE const u8* GetData() const { return m_data; }
  ALWAYS_INLINE size_t GetDataSize() const { return m_size; }
  ALWAYS_INLINE size_t GetPosition() const { return m_pos; }

  /// Overload for integral or floating-point types. Writes bytes as-is.
  template<typename T, std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>, int> = 0>
  void Do(T* value_ptr)
  {
    if (m_mode == Mode::Read)
    {
      if (!ReadData(value_ptr, sizeof(T))) [[unlikely]]
        *value_ptr = static_cast<T>(0);
    }
    else
    {
      WriteData(value_ptr, sizeof(T));
    }
  }

  /// Overload for enum types. Uses the underlying type.
  template<typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
  void Do(T* value_ptr)
  {
    using TType = std::underlying_type_t<T>;
    if (m_mode == Mode::Read)
    {
      TType temp;
      if (!ReadData(&temp, sizeof(temp))) [[unlikely]]
        temp = static_cast<TType>(0);

      *value_ptr = static_cast<T>(temp);
    }
    else
    {
      const TType temp = static_cast<TType>(*value_ptr);
      WriteData(&temp, sizeof(temp));
    }
  }

  /// Overload for POD types, such as structs.
  template<typename T, std::enable_if_t<std::is_standard_layout_v<T> && std::is_trivial_v<T>, int> = 0>
  void DoPOD(T* value_ptr)
  {
    if (m_mode == Mode::Read)
    {
      if (!ReadData(value_ptr, sizeof(T))) [[unlikely]]
        std::memset(value_ptr, 0, sizeof(*value_ptr));
    }
    else
    {
      WriteData(value_ptr, sizeof(T));
    }
  }

  template<typename T>
  void DoArray(T* values, size_t count)
  {
    for (size_t i = 0; i < count; i++)
      Do(&values[i]);
  }

  template<typename T>
  void DoPODArray(T* values, size_t count)
  {
    for (size_t i = 0; i < count; i++)
      DoPOD(&values[i]);
  }

  void DoBytes(void* data, size_t length);
  void DoBytesEx(void* data, size_t length, u32 version_introduced, const void* default_value);

  void Do(bool* value_ptr);
  void Do(std::string* value_ptr);
  void Do(std::string_view* value_ptr);
  void Do(SmallStringBase* value_ptr);

  template<typename T, size_t N>
  void Do(std::array<T, N>* data)
  {
    DoArray(data->data(), data->size());
  }

  template<typename T, size_t N>
  void Do(FixedHeapArray<T, N>* data)
  {
    DoArray(data->data(), data->size());
  }

  template<typename T>
  void Do(std::vector<T>* data)
  {
    u32 length = static_cast<u32>(data->size());
    Do(&length);
    if (m_mode == Mode::Read)
      data->resize(length);
    DoArray(data->data(), data->size());
  }

  template<typename T>
  void Do(std::deque<T>* data)
  {
    u32 length = static_cast<u32>(data->size());
    Do(&length);
    if (m_mode == Mode::Read)
    {
      data->clear();
      for (u32 i = 0; i < length; i++)
      {
        T value;
        Do(&value);
        data->push_back(value);
      }
    }
    else
    {
      for (u32 i = 0; i < length; i++)
        Do(&data[i]);
    }
  }

  template<typename T, u32 CAPACITY>
  void Do(FIFOQueue<T, CAPACITY>* data)
  {
    u32 size = data->GetSize();
    Do(&size);

    if (m_mode == Mode::Read)
    {
      T* temp = new T[size];
      DoArray(temp, size);
      data->Clear();
      data->PushRange(temp, size);
      delete[] temp;
    }
    else
    {
      for (u32 i = 0; i < size; i++)
      {
        T temp(data->Peek(i));
        Do(&temp);
      }
    }
  }

  bool DoMarker(const char* marker);

  template<typename T>
  void DoEx(T* data, u32 version_introduced, T default_value)
  {
    if (m_mode == Mode::Read && m_version < version_introduced) [[unlikely]]
    {
      *data = std::move(default_value);
      return;
    }

    Do(data);
  }

  void SkipBytes(size_t count)
  {
    if (m_mode != Mode::Read)
    {
      m_error = true;
      return;
    }

    m_error = (m_error || (m_pos + count) > m_size);
    if (!m_error) [[likely]]
      m_pos += count;
  }

private:
  bool ReadData(void* buf, size_t size);
  bool WriteData(const void* buf, size_t size);

  u8* m_data;
  size_t m_size;
  size_t m_pos = 0;
  Mode m_mode;
  u32 m_version;
  bool m_error = false;
};
