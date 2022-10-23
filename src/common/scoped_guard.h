#pragma once
#include "types.h"
#include <optional>
#include <utility>

/// ScopedGuard provides an object which runs a function (usually a lambda) when
/// it goes out of scope. This can be useful for releasing resources or handles
/// which do not normally have C++ types to automatically release.
template<typename T>
class ScopedGuard final
{
public:
  ALWAYS_INLINE ScopedGuard(T&& func) : m_func(std::forward<T>(func)) {}
  ALWAYS_INLINE ScopedGuard(ScopedGuard&& other) : m_func(std::move(other.m_func)) { other.m_func = nullptr; }

  ALWAYS_INLINE ~ScopedGuard() { Run(); }

  ScopedGuard(const ScopedGuard&) = delete;
  void operator=(const ScopedGuard&) = delete;

  /// Runs the destructor function now instead of when we go out of scope.
  ALWAYS_INLINE void Run()
  {
    if (!m_func.has_value())
      return;

    m_func.value()();
    m_func.reset();
  }

  /// Prevents the function from being invoked when we go out of scope.
  ALWAYS_INLINE void Cancel() { m_func.reset(); }

private:
  std::optional<T> m_func;
};
