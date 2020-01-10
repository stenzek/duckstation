#pragma once
#include "assert.h"
#include "types.h"
#include <algorithm>
#include <cstring>
#include <type_traits>

#include <malloc.h> // _aligned_malloc, memalign

template<typename T, u32 CAPACITY>
class FIFOQueue
{
public:
  const T* GetDataPointer() const { return m_ptr; }
  T* GetDataPointer() { return m_ptr; }
  const T* GetFrontPointer() const { return &m_ptr[m_head]; }
  T* GetFrontPointer() { return &m_ptr[m_head]; }
  constexpr u32 GetCapacity() const { return CAPACITY; }
  u32 GetSize() const { return m_size; }
  u32 GetSpace() const { return CAPACITY - m_size; }
  u32 GetContiguousSpace() const { return (m_tail >= m_head) ? (CAPACITY - m_tail) : (m_head - m_tail); }
  u32 GetContiguousSize() const { return std::min<u32>(CAPACITY - m_head, m_size); }
  bool IsEmpty() const { return m_size == 0; }
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
    new (&ref) T(std::forward<Args...>(args...));
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

  void Remove(u32 count)
  {
    Assert(m_size >= count);
    for (u32 i = 0; i < count; i++)
    {
      m_ptr[m_head].~T();
      m_head = (m_head + 1) % CAPACITY;
      m_size--;
    }
  }

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

  void PopRange(T* out_data, u32 count)
  {
    Assert(m_size >= count);

    for (u32 i = 0; i < count; i++)
    {
      out_data[i] = std::move(m_ptr[m_head]);
      m_ptr[m_head].~T();
      m_head = (m_head + 1) % CAPACITY;
      m_size--;
    }
  }

  template<u32 QUEUE_CAPACITY>
  void PushFromQueue(FIFOQueue<T, QUEUE_CAPACITY>* other_queue)
  {
    while (!other_queue->IsEmpty() && !IsFull())
    {
      T& dest = PushAndGetReference();
      dest = std::move(other_queue->Pop());
    }
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
  InlineFIFOQueue() : FIFOQueue<T, CAPACITY>() { this->m_ptr = m_inline_data; }

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
      this->m_ptr = static_cast<T*>(_aligned_malloc(sizeof(T) * CAPACITY, ALIGNMENT));
#else
      this->m_ptr = static_cast<T*>(memalign(ALIGNMENT, sizeof(T) * CAPACITY));
#endif
    }
    else
    {
      this->m_ptr = static_cast<T*>(std::malloc(sizeof(T) * CAPACITY));
    }

    if (!this->m_ptr)
      Panic("Heap allocation failed");

    std::memset(this->m_ptr, 0, sizeof(T) * CAPACITY);
  }

  ~HeapFIFOQueue()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _MSC_VER
      _aligned_free(this->m_ptr);
#else
      free(this->m_ptr);
#endif
    }
    else
    {
      free(this->m_ptr);
    }
  }
};
