#pragma once
#include "YBaseLib/Assert.h"
#include "types.h"
#include <cstring>
#include <type_traits>

#ifdef _MSC_VER
#include <malloc.h> // _aligned_malloc
#else
#include <cstdlib>
#endif

template<typename T, u32 CAPACITY>
class FIFOQueue
{
public:
  const T* GetDataPointer() const { return m_ptr; }
  T* GetDataPointer() { return m_ptr; }
  const T* GetFrontPointer() const { return m_ptr[m_head]; }
  T* GetFrontPointer() { return m_ptr[m_head]; }
  constexpr u32 GetCapacity() const { return CAPACITY; }
  u32 GetSize() const { return m_size; }
  bool IsEmpty() const { return m_size > 0; }
  bool IsFull() const { return m_size == CAPACITY; }

  void Clear()
  {
    m_head = 0;
    m_tail = 0;
    m_size = 0;
  }

  template<class... Args>
  T& Emplace(Args&&... args)
  {
    T& ref = PushAndGetReference();
    new (&ref) T(std::forward<Args>(args...));
    return ref;
  }

  template<class Y = T, std::enable_if_t<std::is_pod_v<Y>, int> = 0>
  T& Push(const T& value)
  {
    T& ref = PushAndGetReference();
    std::memcpy(&ref, &value, sizeof(T));
    return ref;
  }

  template<class Y = T, std::enable_if_t<!std::is_pod_v<Y>, int> = 0>
  T& Push(const T& value)
  {
    T& ref = PushAndGetReference();
    new (&ref) T(value);
    return ref;
  }

  // faster version of push_back_range for POD types which can be memcpy()ed
  template<class Y = T, std::enable_if_t<std::is_pod_v<Y>, int> = 0>
  void PushRange(const T* data, u32 size)
  {
    Assert((m_size + size) <= CAPACITY);
    const u32 space_before_end = CAPACITY - m_tail;
    const u32 size_before_end = (size > space_before_end) ? space_before_end : size;
    const u32 size_after_end = size - size_before_end;

    std::memcpy(&m_ptr[m_tail], data, sizeof(T) * size_before_end);
    m_tail = (m_tail + size_before_end) % CAPACITY;

    if (size_after_end > 0)
    {
      std::memcpy(&m_ptr[m_tail], data + size_before_end, sizeof(T) * size_after_end);
      m_tail = (m_tail + size_after_end) % CAPACITY;
    }

    m_size += size;
  }

  template<class Y = T, std::enable_if_t<!std::is_pod_v<Y>, int> = 0>
  void PushRange(const T* data, u32 size)
  {
    Assert((m_size + size) <= CAPACITY);
    while (size > 0)
    {
      T& ref = PushAndGetReference();
      new (&ref) T(*data);
      data++;
      size--;
    }
  }

  const T& Peek() const { return m_ptr[m_head]; }
  const T& Peek(u32 offset) { return m_ptr[(m_head + offset) % CAPACITY]; }

  void RemoveOne()
  {
    Assert(m_size > 0);
    m_ptr[m_head].~T();
    m_head = (m_head + 1) % CAPACITY;
    m_size--;
  }

  // removes and returns moved value
  T Pop()
  {
    Assert(m_size > 0);
    T val = std::move(m_ptr[m_head]);
    m_ptr[m_head].~T();
    m_head = (m_head + 1) % CAPACITY;
    m_size--;
    return val;
  }

protected:
  FIFOQueue() = default;

  T& PushAndGetReference()
  {
    Assert(m_size < CAPACITY);
    T& ref = m_ptr[m_tail];
    m_tail = (m_tail + 1) % CAPACITY;
    m_size++;
    return ref;
  }

  T* m_ptr = nullptr;
  u32 m_head = 0;
  u32 m_tail = 0;
  u32 m_size = 0;
};

template<typename T, u32 CAPACITY>
class InlineFIFOQueue : public FIFOQueue<T, CAPACITY>
{
public:
  InlineFIFOQueue() : FIFOQueue<T, CAPACITY>() { m_ptr = m_inline_data; }

private:
  T m_inline_data[CAPACITY] = {};
};

template<typename T, u32 CAPACITY, u32 ALIGNMENT = 0>
class HeapFIFOQueue : public FIFOQueue<T, CAPACITY>
{
public:
  HeapFIFOQueue() : FIFOQueue<T, CAPACITY>()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _MSC_VER
      m_ptr = static_cast<T*>(_aligned_malloc(sizeof(T) * CAPACITY, ALIGNMENT));
#else
      m_ptr = static_cast<T*>(memalign(ALIGNMENT, sizeof(T) * CAPACITY));
#endif
    }
    else
    {
      m_ptr = static_cast<T*>(std::malloc(sizeof(T) * CAPACITY));
    }

    if (!m_ptr)
      Panic("Heap allocation failed");

    std::memset(m_ptr, 0, sizeof(T) * CAPACITY);
  }

  ~HeapFIFOQueue()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _MSC_VER
      _aligned_free(m_ptr);
#else
      free(m_ptr);
#endif
    }
    else
    {
      free(m_ptr);
    }
  }
};
