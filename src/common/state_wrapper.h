#pragma once
#include "byte_stream.h"
#include "fifo_queue.h"
#include "heap_array.h"
#include "types.h"
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

class String;

class StateWrapper
{
public:
  enum class Mode
  {
    Read,
    Write
  };

  StateWrapper(ByteStream* stream, Mode mode, u32 version);
  StateWrapper(const StateWrapper&) = delete;
  ~StateWrapper();

  ByteStream* GetStream() const { return m_stream; }
  bool HasError() const { return m_error; }
  bool IsReading() const { return (m_mode == Mode::Read); }
  bool IsWriting() const { return (m_mode == Mode::Write); }
  Mode GetMode() const { return m_mode; }
  void SetMode(Mode mode) { m_mode = mode; }
  u32 GetVersion() const { return m_version; }

  /// Overload for integral or floating-point types. Writes bytes as-is.
  template<typename T, std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>, int> = 0>
  void Do(T* value_ptr)
  {
    if (m_mode == Mode::Read)
    {
      if (m_error || (m_error |= !m_stream->Read2(value_ptr, sizeof(T))) == true)
        *value_ptr = static_cast<T>(0);
    }
    else
    {
      if (!m_error)
        m_error |= !m_stream->Write2(value_ptr, sizeof(T));
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
      if (m_error || (m_error |= !m_stream->Read2(&temp, sizeof(TType))) == true)
        temp = static_cast<TType>(0);

      *value_ptr = static_cast<T>(temp);
    }
    else
    {
      TType temp;
      std::memcpy(&temp, value_ptr, sizeof(TType));
      if (!m_error)
        m_error |= !m_stream->Write2(&temp, sizeof(TType));
    }
  }

  /// Overload for POD types, such as structs.
  template<typename T, std::enable_if_t<std::is_pod_v<T>, int> = 0>
  void DoPOD(T* value_ptr)
  {
    if (m_mode == Mode::Read)
    {
      if (m_error || (m_error |= !m_stream->Read2(value_ptr, sizeof(T))) == true)
        std::memset(value_ptr, 0, sizeof(*value_ptr));
    }
    else
    {
      if (!m_error)
        m_error |= !m_stream->Write2(value_ptr, sizeof(T));
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

  void Do(bool* value_ptr);
  void Do(std::string* value_ptr);
  void Do(String* value_ptr);

  template<typename T, size_t N>
  void Do(std::array<T, N>* data)
  {
    DoArray(data->data(), data->size());
  }

  template<typename T, size_t N>
  void Do(HeapArray<T, N>* data)
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
    if (m_version < version_introduced)
    {
      *data = std::move(default_value);
      return;
    }

    Do(data);
  }

private:
  ByteStream* m_stream;
  Mode m_mode;
  u32 m_version;
  bool m_error = false;
};
