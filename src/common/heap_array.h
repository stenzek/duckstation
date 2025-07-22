// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

template<typename T, std::size_t SIZE, std::size_t ALIGNMENT = 0>
class FixedHeapArray
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using this_type = FixedHeapArray<T, SIZE, ALIGNMENT>;

  FixedHeapArray() { allocate(); }

  FixedHeapArray(const this_type& copy)
  {
    allocate();
    std::copy(copy.cbegin(), copy.cend(), begin());
  }

  FixedHeapArray(this_type&& move)
  {
    m_data = move.m_data;
    move.m_data = nullptr;
  }

  ~FixedHeapArray() { deallocate(); }

  size_type size() const { return SIZE; }
  size_type capacity() const { return SIZE; }
  bool empty() const { return false; }

  pointer begin() { return m_data; }
  pointer end() { return m_data + SIZE; }

  const_pointer data() const { return m_data; }
  pointer data() { return m_data; }

  const_pointer cbegin() const { return m_data; }
  const_pointer cend() const { return m_data + SIZE; }

  const_reference operator[](size_type index) const
  {
    assert(index < SIZE);
    return m_data[index];
  }
  reference operator[](size_type index)
  {
    assert(index < SIZE);
    return m_data[index];
  }

  const_reference front() const { return m_data[0]; }
  const_reference back() const { return m_data[SIZE - 1]; }
  reference front() { return m_data[0]; }
  reference back() { return m_data[SIZE - 1]; }

  void fill(const_reference value) { std::fill(begin(), end(), value); }

  void swap(this_type& move) { std::swap(m_data, move.m_data); }

  std::span<T, SIZE> span() { return std::span<T, SIZE>(m_data); }
  std::span<const T, SIZE> cspan() const { return std::span<const T, SIZE>(m_data); }

  this_type& operator=(const this_type& rhs)
  {
    std::copy(begin(), end(), rhs.cbegin());
    return *this;
  }

  this_type& operator=(this_type&& move)
  {
    deallocate();
    m_data = move.m_data;
    move.m_data = nullptr;
    return *this;
  }

#define RELATIONAL_OPERATOR(op)                                                                                        \
  bool operator op(const this_type& rhs) const                                                                         \
  {                                                                                                                    \
    for (size_type i = 0; i < SIZE; i++)                                                                               \
    {                                                                                                                  \
      if (!(m_data[i] op rhs.m_data[i]))                                                                               \
        return false;                                                                                                  \
    }                                                                                                                  \
  }

  RELATIONAL_OPERATOR(==);
  RELATIONAL_OPERATOR(!=);
  RELATIONAL_OPERATOR(<);
  RELATIONAL_OPERATOR(<=);
  RELATIONAL_OPERATOR(>);
  RELATIONAL_OPERATOR(>=);

#undef RELATIONAL_OPERATOR

private:
  void allocate()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _MSC_VER
      m_data = static_cast<T*>(_aligned_malloc(SIZE * sizeof(T), ALIGNMENT));
      assert(m_data);
      if (!m_data) [[unlikely]]
        std::abort();
#else
      if (posix_memalign(reinterpret_cast<void**>(&m_data), ALIGNMENT, SIZE * sizeof(T)) != 0) [[unlikely]]
        std::abort();
#endif
    }
    else
    {
      m_data = static_cast<T*>(std::malloc(SIZE * sizeof(T)));
      assert(m_data);
      if (!m_data) [[unlikely]]
        std::abort();
    }
  }
  void deallocate()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _MSC_VER
      _aligned_free(m_data);
#else
      std::free(m_data);
#endif
    }
    else
    {
      std::free(m_data);
    }
  }

  T* m_data;
};

template<typename T, size_t alignment = 0>
class DynamicHeapArray
{
  static_assert(std::is_trivially_copyable_v<T>, "T is trivially copyable");
  static_assert(std::is_standard_layout_v<T>, "T is standard layout");

public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using this_type = DynamicHeapArray<T, alignment>;

  DynamicHeapArray() : m_data(nullptr), m_size(0) {}
  explicit DynamicHeapArray(size_t size) { internal_resize(size, nullptr, 0); }
  DynamicHeapArray(const T* begin, const T* end)
  {
    const size_t size = reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(begin);
    if (size > 0)
    {
      internal_resize(size / sizeof(T), nullptr, 0);
      std::memcpy(m_data, begin, size);
    }
    else
    {
      m_data = nullptr;
      m_size = 0;
    }
  }
  DynamicHeapArray(const T* begin, size_t count)
  {
    if (count > 0)
    {
      internal_resize(count, nullptr, 0);
      std::memcpy(m_data, begin, sizeof(T) * count);
    }
    else
    {
      m_data = nullptr;
      m_size = 0;
    }
  }
  explicit DynamicHeapArray(const std::span<const T> data)
  {
    if (!data.empty())
    {
      internal_resize(data.size(), nullptr, 0);
      std::memcpy(m_data, data.data(), sizeof(T) * data.size());
    }
    else
    {
      m_data = nullptr;
      m_size = 0;
    }
  }

  DynamicHeapArray(const this_type& copy)
  {
    if (copy.m_size > 0)
    {
      internal_resize(copy.m_size, nullptr, 0);
      std::memcpy(m_data, copy.m_data, sizeof(T) * copy.m_size);
    }
    else
    {
      m_data = nullptr;
      m_size = 0;
    }
  }

  DynamicHeapArray(this_type&& move) noexcept
  {
    m_data = move.m_data;
    m_size = move.m_size;
    move.m_data = nullptr;
    move.m_size = 0;
  }

  ~DynamicHeapArray() { internal_deallocate(); }

  size_type size() const { return m_size; }
  size_type capacity() const { return m_size; }
  bool empty() const { return (m_size == 0); }

  pointer begin() { return m_data; }
  pointer end() { return m_data + m_size; }

  const_pointer data() const { return m_data; }
  pointer data() { return m_data; }

  const_pointer cbegin() const { return m_data; }
  const_pointer cend() const { return m_data + m_size; }

  const_reference operator[](size_type index) const
  {
    assert(index < m_size);
    return m_data[index];
  }
  reference operator[](size_type index)
  {
    assert(index < m_size);
    return m_data[index];
  }

  const_reference front() const { return m_data[0]; }
  const_reference back() const { return m_data[m_size - 1]; }
  reference front() { return m_data[0]; }
  reference back() { return m_data[m_size - 1]; }

  void fill(const_reference value) { std::fill(begin(), end(), value); }

  void swap(this_type& rhs)
  {
    std::swap(m_data, rhs.m_data);
    std::swap(m_size, rhs.m_size);
  }

  void resize(size_t new_size) { internal_resize(new_size, m_data, m_size); }

  void deallocate()
  {
    internal_deallocate();
    m_data = nullptr;
    m_size = 0;
  }

  void assign(const std::span<const T> data) { assign(data.data(), data.size()); }

  void assign(const T* begin, const T* end)
  {
    const size_t size = reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(begin);
    const size_t count = size / sizeof(T);
    if (count > 0)
    {
      if (m_size != count)
      {
        internal_deallocate();
        internal_resize(count, nullptr, 0);
      }

      std::memcpy(m_data, begin, size);
    }
    else
    {
      internal_deallocate();

      m_data = nullptr;
      m_size = 0;
    }
  }
  void assign(const T* begin, size_t count)
  {
    if (count > 0)
    {
      if (m_size != count)
      {
        internal_deallocate();
        internal_resize(count, nullptr, 0);
      }

      std::memcpy(m_data, begin, sizeof(T) * count);
    }
    else
    {
      internal_deallocate();

      m_data = nullptr;
      m_size = 0;
    }
  }
  void assign(const this_type& copy) { assign(copy.m_data, copy.m_size); }
  void assign(this_type&& move)
  {
    internal_deallocate();
    m_data = move.m_data;
    m_size = move.m_size;
    move.m_data = nullptr;
    move.m_size = 0;
  }

  std::span<T> span() { return std::span<T>(m_data, m_size); }
  std::span<const T> cspan() const { return std::span<const T>(m_data, m_size); }

  std::span<T> span(size_t offset, size_t size = static_cast<size_t>(-1))
  {
    std::span<T> ret;
    if (offset < m_size) [[likely]]
      ret = std::span<T>(m_data + offset, std::min(m_size - offset, size));
    return ret;
  }

  std::span<const T> cspan(size_t offset, size_t size = static_cast<size_t>(-1)) const
  {
    std::span<const T> ret;
    if (offset < m_size) [[likely]]
      ret = std::span<const T>(m_data + offset, std::min(m_size - offset, size));
    return ret;
  }

  this_type& operator=(const this_type& rhs)
  {
    assign(rhs);
    return *this;
  }

  this_type& operator=(this_type&& move) noexcept
  {
    assign(std::move(move));
    return *this;
  }

#define RELATIONAL_OPERATOR(op, size_op)                                                                               \
  bool operator op(const this_type& rhs) const                                                                         \
  {                                                                                                                    \
    if (m_size != rhs.m_size)                                                                                          \
      return m_size size_op rhs.m_size;                                                                                \
    for (size_type i = 0; i < m_size; i++)                                                                             \
    {                                                                                                                  \
      if (!(m_data[i] op rhs.m_data[i]))                                                                               \
        return false;                                                                                                  \
    }                                                                                                                  \
  }

  RELATIONAL_OPERATOR(==, !=);
  RELATIONAL_OPERATOR(!=, ==);
  RELATIONAL_OPERATOR(<, <);
  RELATIONAL_OPERATOR(<=, <=);
  RELATIONAL_OPERATOR(>, >);
  RELATIONAL_OPERATOR(>=, >=);

#undef RELATIONAL_OPERATOR

private:
  void internal_resize(size_t size, T* prev_ptr, [[maybe_unused]] size_t prev_size)
  {
    if constexpr (alignment > 0)
    {
#ifdef _MSC_VER
      m_data = static_cast<T*>(_aligned_realloc(prev_ptr, size * sizeof(T), alignment));
      assert(m_data);
      if (!m_data) [[unlikely]]
        std::abort();
#else
      if (posix_memalign(reinterpret_cast<void**>(&m_data), alignment, size * sizeof(T)) != 0) [[unlikely]]
        std::abort();

      if (prev_ptr)
      {
        std::memcpy(m_data, prev_ptr, prev_size * sizeof(T));
        std::free(prev_ptr);
      }
#endif
    }
    else
    {
      m_data = static_cast<T*>(std::realloc(prev_ptr, size * sizeof(T)));
      assert(m_data);
      if (!m_data) [[unlikely]]
        std::abort();
    }

    m_size = size;
  }
  void internal_deallocate()
  {
    if constexpr (alignment > 0)
    {
#ifdef _MSC_VER
      _aligned_free(m_data);
#else
      std::free(m_data);
#endif
    }
    else
    {
      std::free(m_data);
    }
  }

  T* m_data;
  size_t m_size;
};
