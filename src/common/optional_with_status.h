// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

/// OptionalWithStatus<T, S> combines a status code of type S with an optional value of type T.
/// Unlike std::optional, it carries an explicit status alongside the presence/absence of a value,
/// allowing callers to distinguish between different "no value" states (e.g. Miss vs Error).
///
/// The value is stored directly inside the object in aligned raw storage. The has-value bool is
/// packed next to the status field rather than inside a std::optional, avoiding the extra alignment
/// padding that an embedded std::optional<T> member would introduce.
///
/// Typical usage:
///   - Construct with status + value for a success case: {Status::Hit, std::move(data)}
///   - Construct with status only for a non-value case:  {Status::Miss} or {Status::Error}
///   - Check has_value() / operator bool() to test whether a value is held.
///   - Read status() to inspect the status regardless of whether a value is present.
template<typename T, typename S>
class alignas(T) OptionalWithStatus
{
public:
  /// Constructs an empty instance with a default-initialized status.
  ALWAYS_INLINE OptionalWithStatus() = default;

  /// Constructs with a status but no value (e.g. Miss or Error).
  ALWAYS_INLINE explicit OptionalWithStatus(S status) : m_status(status) {}

  /// Constructs with a status and a value copied from \p value.
  ALWAYS_INLINE OptionalWithStatus(S status, const T& value) : m_status(status), m_has_value(true)
  {
    new (m_storage) T(value);
  }

  /// Constructs with a status and a value moved from \p value.
  ALWAYS_INLINE OptionalWithStatus(S status, T&& value) : m_status(status), m_has_value(true)
  {
    new (m_storage) T(std::move(value));
  }

  ALWAYS_INLINE OptionalWithStatus(const OptionalWithStatus& other) : m_status(other.m_status), m_has_value(other.m_has_value)
  {
    if (m_has_value)
      new (m_storage) T(*other.get_ptr());
  }

  ALWAYS_INLINE OptionalWithStatus(OptionalWithStatus&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    : m_status(other.m_status), m_has_value(other.m_has_value)
  {
    if (m_has_value)
    {
      new (m_storage) T(std::move(*other.get_ptr()));
      other.destroy();
    }
  }

  ALWAYS_INLINE ~OptionalWithStatus()
  {
    if (m_has_value)
      destroy();
  }

  ALWAYS_INLINE OptionalWithStatus& operator=(const OptionalWithStatus& other)
  {
    if (this != &other)
    {
      reset();
      m_status = other.m_status;
      if (other.m_has_value)
      {
        new (m_storage) T(*other.get_ptr());
        m_has_value = true;
      }
    }
    return *this;
  }

  ALWAYS_INLINE OptionalWithStatus& operator=(OptionalWithStatus&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
  {
    if (this != &other)
    {
      reset();
      m_status = other.m_status;
      if (other.m_has_value)
      {
        new (m_storage) T(std::move(*other.get_ptr()));
        m_has_value = true;
        other.destroy();
      }
    }
    return *this;
  }

  /// Returns the status code.
  ALWAYS_INLINE S status() const { return m_status; }

  /// Returns true if a value is held.
  ALWAYS_INLINE bool has_value() const { return m_has_value; }

  /// Returns true if a value is held (same as has_value()).
  ALWAYS_INLINE explicit operator bool() const { return m_has_value; }

  /// Returns a reference to the held value. Asserts that a value is held.
  ALWAYS_INLINE T& value() { assert(m_has_value); return *get_ptr(); }
  ALWAYS_INLINE const T& value() const { assert(m_has_value); return *get_ptr(); }

  /// Returns a reference to the held value. Behaviour is undefined if no value is held.
  ALWAYS_INLINE T& operator*() { return *get_ptr(); }
  ALWAYS_INLINE const T& operator*() const { return *get_ptr(); }

  /// Returns a pointer to the held value. Behaviour is undefined if no value is held.
  ALWAYS_INLINE T* operator->() { return get_ptr(); }
  ALWAYS_INLINE const T* operator->() const { return get_ptr(); }

  /// Returns the held value if present, otherwise returns \p default_value.
  template<typename U>
  ALWAYS_INLINE T value_or(U&& default_value) const&
  {
    if (m_has_value)
      return *get_ptr();
    return static_cast<T>(std::forward<U>(default_value));
  }
  template<typename U>
  ALWAYS_INLINE T value_or(U&& default_value) &&
  {
    if (m_has_value)
    {
      T result(std::move(*get_ptr()));
      destroy();
      return result;
    }
    return static_cast<T>(std::forward<U>(default_value));
  }

private:
  ALWAYS_INLINE T* get_ptr() { return std::launder(reinterpret_cast<T*>(m_storage)); }
  ALWAYS_INLINE const T* get_ptr() const { return std::launder(reinterpret_cast<const T*>(m_storage)); }

  ALWAYS_INLINE void destroy()
  {
    get_ptr()->~T();
    m_has_value = false;
  }

  ALWAYS_INLINE void reset()
  {
    if (m_has_value)
      destroy();
  }

  alignas(T) unsigned char m_storage[sizeof(T)];
  S m_status = {};
  bool m_has_value = false;
};
