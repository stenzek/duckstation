/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Docs: https://fburl.com/fbcref_function
//

/**
 * @class folly::Function
 * @refcode folly/docs/examples/folly/Function.cpp
 *
 * A polymorphic function wrapper that is not copyable and does not
 * require the wrapped function to be copy constructible.
 *
 * `folly::Function` is a polymorphic function wrapper, similar to
 * `std::function`. The template parameters of the `folly::Function` define
 * the parameter signature of the wrapped callable, but not the specific
 * type of the embedded callable. E.g. a `folly::Function<int(int)>`
 * can wrap callables that return an `int` when passed an `int`. This can be a
 * function pointer or any class object implementing one or both of
 *
 *     int operator(int);
 *     int operator(int) const;
 *
 * If both are defined, the non-const one takes precedence.
 *
 * Unlike `std::function`, a `folly::Function` can wrap objects that are not
 * copy constructible. As a consequence of this, `folly::Function` itself
 * is not copyable, either.
 *
 * Another difference is that, unlike `std::function`, `folly::Function` treats
 * const-ness of methods correctly. While a `std::function` allows to wrap
 * an object that only implements a non-const `operator()` and invoke
 * a const-reference of the `std::function`, `folly::Function` requires you to
 * declare a function type as const in order to be able to execute it on a
 * const-reference.
 *
 * For example:
 *
 *     class Foo {
 *      public:
 *       void operator()() {
 *         // mutates the Foo object
 *       }
 *     };
 *
 *     class Bar {
 *       std::function<void(void)> foo_; // wraps a Foo object
 *      public:
 *       void mutateFoo() const
 *       {
 *         foo_();
 *       }
 *     };
 *
 * Even though `mutateFoo` is a const-method, so it can only reference `foo_`
 * as const, it is able to call the non-const `operator()` of the Foo
 * object that is embedded in the foo_ function.
 *
 * `folly::Function` will not allow you to do that. You will have to decide
 * whether you need to invoke your wrapped callable from a const reference
 * (like in the example above), in which case it will only wrap a
 * `operator() const`. If your functor does not implement that,
 * compilation will fail. If you do not require to be able to invoke the
 * wrapped function in a const context, you can wrap any functor that
 * implements either or both of const and non-const `operator()`.
 *
 * The template parameter of `folly::Function`, the `FunctionType`, can be
 * const-qualified. Be aware that the const is part of the function signature.
 * It does not mean that the function type is a const type.
 *
 *   using FunctionType = R(Args...);
 *   using ConstFunctionType = R(Args...) const;
 *
 * In this example, `FunctionType` and `ConstFunctionType` are different
 * types. `ConstFunctionType` is not the same as `const FunctionType`.
 * As a matter of fact, trying to use the latter should emit a compiler
 * warning or error, because it has no defined meaning.
 *
 *     // This will not compile:
 *     folly::Function<void(void) const> func = Foo();
 *     // because Foo does not have a member function of the form:
 *     //   void operator()() const;
 *
 *     // This will compile just fine:
 *     folly::Function<void(void)> func = Foo();
 *     // and it will wrap the existing member function:
 *     //   void operator()();
 *
 * When should a const function type be used? As a matter of fact, you will
 * probably not need to use const function types very often. See the following
 * example:
 *
 *     class Bar {
 *       folly::Function<void()> func_;
 *       folly::Function<void() const> constFunc_;
 *
 *       void someMethod() {
 *         // Can call func_.
 *         func_();
 *         // Can call constFunc_.
 *         constFunc_();
 *       }
 *
 *       void someConstMethod() const {
 *         // Can call constFunc_.
 *         constFunc_();
 *         // However, cannot call func_ because a non-const method cannot
 *         // be called from a const one.
 *       }
 *     };
 *
 * As you can see, whether the `folly::Function`'s function type should
 * be declared const or not is identical to whether a corresponding method
 * would be declared const or not.
 *
 * You only require a `folly::Function` to hold a const function type, if you
 * intend to invoke it from within a const context. This is to ensure that
 * you cannot mutate its inner state when calling in a const context.
 *
 * This is how the const/non-const choice relates to lambda functions:
 *
 *     // Non-mutable lambdas: can be stored in a non-const...
 *     folly::Function<void(int)> print_number =
 *       [] (int number) { std::cout << number << std::endl; };
 *
 *     // ...as well as in a const folly::Function
 *     folly::Function<void(int) const> print_number_const =
 *       [] (int number) { std::cout << number << std::endl; };
 *
 *     // Mutable lambda: can only be stored in a non-const folly::Function:
 *     int number = 0;
 *     folly::Function<void()> print_number =
 *       [number] () mutable { std::cout << ++number << std::endl; };
 *     // Trying to store the above mutable lambda in a
 *     // `folly::Function<void() const>` would lead to a compiler error:
 *     // error: no viable conversion from '(lambda at ...)' to
 *     // 'folly::Function<void () const>'
 *
 * Casting between const and non-const `folly::Function`s:
 * conversion from const to non-const signatures happens implicitly. Any
 * function that takes a `folly::Function<R(Args...)>` can be passed
 * a `folly::Function<R(Args...) const>` without explicit conversion.
 * This is safe, because casting from const to non-const only entails giving
 * up the ability to invoke the function from a const context.
 * Casting from a non-const to a const signature is potentially dangerous,
 * as it means that a function that may change its inner state when invoked
 * is made possible to call from a const context. Therefore this cast does
 * not happen implicitly. The function `folly::constCastFunction` can
 * be used to perform the cast.
 *
 *     // Mutable lambda: can only be stored in a non-const folly::Function:
 *     int number = 0;
 *     folly::Function<void()> print_number =
 *       [number] () mutable { std::cout << ++number << std::endl; };
 *
 *     // const-cast to a const folly::Function:
 *     folly::Function<void() const> print_number_const =
 *       constCastFunction(std::move(print_number));
 *
 * When to use const function types?
 * Generally, only when you need them. When you use a `folly::Function` as a
 * member of a struct or class, only use a const function signature when you
 * need to invoke the function from const context.
 * When passing a `folly::Function` to a function, the function should accept
 * a non-const `folly::Function` whenever possible, i.e. when it does not
 * need to pass on or store a const `folly::Function`. This is the least
 * possible constraint: you can always pass a const `folly::Function` when
 * the function accepts a non-const one.
 *
 * How does the const behaviour compare to `std::function`?
 * `std::function` can wrap object with non-const invocation behaviour but
 * exposes them as const. The equivalent behaviour can be achieved with
 * `folly::Function` like so:
 *
 *     std::function<void(void)> stdfunc = someCallable;
 *
 *     folly::Function<void(void) const> uniqfunc = constCastFunction(
 *       folly::Function<void(void)>(someCallable)
 *     );
 *
 * You need to wrap the callable first in a non-const `folly::Function` to
 * select a non-const invoke operator (or the const one if no non-const one is
 * present), and then move it into a const `folly::Function` using
 * `constCastFunction`.
 */

// DuckStation: Single-header port of folly/Function.h, used as a std::move_only_function polyfill where the standard
// library lacks it. To avoid pulling in the rest of folly, the pieces of the folly headers it depends on are copied in
// below, reduced to what Function.h uses. Mechanical changes from upstream are not marked: folly:: traits replaced by
// their std:: equivalents and FOLLY_LIKELY replaced by [[likely]], since the folly versions are not ported, and
// throw_exception/terminate_with replaced by std::abort(), since we build without exceptions. Behavioural changes are
// marked with "DuckStation:" comments.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

//#include <folly/CppAttributes.h>
//#include <folly/Portability.h>
//#include <folly/Traits.h>
//#include <folly/functional/Invoke.h>
//#include <folly/lang/Align.h>
//#include <folly/lang/Exception.h>
//#include <folly/lang/New.h>

// always inline
#ifdef _MSC_VER
#define FOLLY_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__)
#define FOLLY_ALWAYS_INLINE inline __attribute__((__always_inline__))
#else
#define FOLLY_ALWAYS_INLINE inline
#endif

// attribute hidden
#if defined(_MSC_VER)
#define FOLLY_ATTR_VISIBILITY_HIDDEN
#elif defined(__GNUC__)
#define FOLLY_ATTR_VISIBILITY_HIDDEN __attribute__((__visibility__("hidden")))
#else
#define FOLLY_ATTR_VISIBILITY_HIDDEN
#endif

//  FOLLY_ERASE
//
//  A conceptual attribute/syntax combo for erasing a function from the build
//  artifacts and forcing all call-sites to inline the callee, at least as far
//  as each compiler supports.
//
//  Semantically includes the inline specifier.
#define FOLLY_ERASE FOLLY_ALWAYS_INLINE FOLLY_ATTR_VISIBILITY_HIDDEN

// Define FOLLY_HAS_EXCEPTIONS
#if defined(__cpp_exceptions) && __cpp_exceptions >= 199711
#define FOLLY_HAS_EXCEPTIONS 1
#elif __GNUC__
#if defined(__EXCEPTIONS) && __EXCEPTIONS
#define FOLLY_HAS_EXCEPTIONS 1
#else // __EXCEPTIONS
#define FOLLY_HAS_EXCEPTIONS 0
#endif // __EXCEPTIONS
#elif defined(_MSC_VER)
#if _CPPUNWIND
#define FOLLY_HAS_EXCEPTIONS 1
#else // _CPPUNWIND
#define FOLLY_HAS_EXCEPTIONS 0
#endif // _CPPUNWIND
#else
#define FOLLY_HAS_EXCEPTIONS 1 // default assumption for unknown platforms
#endif

/*
 * FOLLY_DECLVAL(T)
 *
 * This macro works like std::declval<T>() but does the same thing in a way
 * that does not require instantiating a function template.
 *
 * Use this macro instead of std::declval<T>() in places that are widely
 * instantiated to reduce compile-time overhead of instantiating function
 * templates.
 *
 * Note that, like std::declval<T>(), this macro can only be used in
 * unevaluated contexts.
 *
 * There are some small differences between this macro and std::declval<T>().
 * - This macro results in a value of type 'T' instead of 'T&&'.
 * - This macro requires the type T to be a complete type at the
 *   point of use.
 *   If this is a problem then use FOLLY_DECLVAL(T&&) instead, or if T might
 *   be 'void', then use FOLLY_DECLVAL(std::add_rvalue_reference_t<T>).
 */
#define FOLLY_DECLVAL(...) static_cast<__VA_ARGS__ (*)() noexcept>(nullptr)()

namespace folly {

/**
 *  type_t
 *
 *  A type alias for the first template type argument. `type_t` is useful for
 *  controlling class-template and function-template partial specialization.
 *
 *  Example:
 *
 *    template <typename Value>
 *    class Container {
 *     public:
 *      template <typename... Args>
 *      Container(
 *          type_t<in_place_t, decltype(Value(std::declval<Args>()...))>,
 *          Args&&...);
 *    };
 *
 *  void_t
 *
 *  A type alias for `void`. `void_t` is useful for controlling class-template
 *  and function-template partial specialization.
 *
 *  Example:
 *
 *    // has_value_type<T>::value is true if T has a nested type `value_type`
 *    template <class T, class = void>
 *    struct has_value_type
 *        : std::false_type {};
 *
 *    template <class T>
 *    struct has_value_type<T, folly::void_t<typename T::value_type>>
 *        : std::true_type {};
 */

/**
 * There is a bug in libstdc++, libc++, and MSVC's STL that causes it to
 * ignore unused template parameter arguments in template aliases and does not
 * cause substitution failures. This defect has been recorded here:
 * http://open-std.org/JTC1/SC22/WG21/docs/cwg_defects.html#1558.
 *
 * This causes the implementation of std::void_t to be buggy, as it is likely
 * defined as something like the following:
 *
 *  template <typename...>
 *  using void_t = void;
 *
 * This causes the compiler to ignore all the template arguments and does not
 * help when one wants to cause substitution failures.  Rather declarations
 * which have void_t in orthogonal specializations are treated as the same.
 * For example, assuming the possible `T` types are only allowed to have
 * either the alias `one` or `two` and never both or none:
 *
 *  template <typename T,
 *            typename std::void_t<std::decay_t<T>::one>* = nullptr>
 *  void foo(T&&) {}
 *  template <typename T,
 *            typename std::void_t<std::decay_t<T>::two>* = nullptr>
 *  void foo(T&&) {}
 *
 * The second foo() will be a redefinition because it conflicts with the first
 * one; void_t does not cause substitution failures - the template types are
 * just ignored.
 */

namespace traits_detail {
template <class T, class...>
struct type_t_ {
  using type = T;
};
} // namespace traits_detail

template <class T, class... Ts>
using type_t = typename traits_detail::type_t_<T, Ts...>::type;
template <class... Ts>
using void_t = type_t<void, Ts...>;

/// nonesuch
///
/// A tag type which traits may use to indicate lack of a result type.
///
/// Similar to void in that no values of this type may be constructed. Different
/// from void in that no functions may be defined with this return type and no
/// complete expressions may evaluate with this expression type.
///
/// mimic: std::experimental::nonesuch, Library Fundamentals TS v2
struct nonesuch {
  ~nonesuch() = delete;
  nonesuch(nonesuch const&) = delete;
  void operator=(nonesuch const&) = delete;
};

namespace detail {

template <typename Void, typename D, template <typename...> class, typename...>
struct detected_ {
  using value_t = std::false_type;
  using type = D;
};
template <typename D, template <typename...> class T, typename... A>
struct detected_<void_t<T<A...>>, D, T, A...> {
  using value_t = std::true_type;
  using type = T<A...>;
};

} // namespace detail

/// detected_or
///
/// If T<A...> substitutes, has member type alias value_t as std::true_type
/// and has member type alias type as T<A...>. Otherwise, has member type
/// alias value_t as std::false_type and has member type alias type as D.
///
/// mimic: std::experimental::detected_or, Library Fundamentals TS v2
///
/// Note: not resilient against incomplete types; may violate ODR.
template <typename D, template <typename...> class T, typename... A>
using detected_or = detail::detected_<void, D, T, A...>;

/// detected_or_t
///
/// A trait type alias which results in T<A...> if substitution would succeed
/// and in D otherwise.
///
/// Equivalent to detected_or<D, T, A...>::type.
///
/// mimic: std::experimental::detected_or_t, Library Fundamentals TS v2
///
/// Note: not resilient against incomplete types; may violate ODR.
template <typename D, template <typename...> class T, typename... A>
using detected_or_t = typename detected_or<D, T, A...>::type;

/// detected_t
///
/// A trait type alias which results in T<A...> if substitution would succeed
/// and in nonesuch otherwise.
///
/// Equivalent to detected_or_t<nonesuch, T, A...>.
///
/// mimic: std::experimental::detected_t, Library Fundamentals TS v2
///
/// Note: not resilient against incomplete types; may violate ODR.
template <template <typename...> class T, typename... A>
using detected_t = detected_or_t<nonesuch, T, A...>;

//  is_detected_v
//  is_detected
//
//  A trait variable and type to test whether some metafunction from types to
//  types would succeed or fail in substitution over a given set of arguments.
//
//  The trait variable is_detected_v<T, A...> is equivalent to
//  detected_or<nonesuch, T, A...>::value_t::value.
//  The trait type is_detected<T, A...> unambiguously inherits
//  std::bool_constant<V> where V is is_detected_v<T, A...>.
//
//  mimic: std::experimental::is_detected, std::experimental::is_detected_v,
//    Library Fundamentals TS v2
//
//  Note: not resilient against incomplete types; may violate ODR.
//
//  Note: the trait type is_detected differs here by being deferred.
template <template <typename...> class T, typename... A>
inline constexpr bool is_detected_v =
    detected_or<nonesuch, T, A...>::value_t::value;
template <template <typename...> class T, typename... A>
struct is_detected : detected_or<nonesuch, T, A...>::value_t {};

namespace detail {
constexpr std::size_t aligned_storage_default_align(std::size_t len) {
  // Match std::aligned_storage default: the most stringent fundamental
  // alignment for any type whose size is no greater than len.
  // Compute bit_floor(min(len, alignof(max_align_t))): propagate the highest
  // set bit to all lower positions, then isolate it. O(log(bits)) steps.
  len = len < alignof(std::max_align_t) ? len : alignof(std::max_align_t);
  len |= len >> 1;
  len |= len >> 2;
  len |= len >> 4;
  len |= len >> 8;
  if constexpr (sizeof(std::size_t) > 2) {
    len |= len >> 16;
  }
  if constexpr (sizeof(std::size_t) > 4) {
    len |= len >> 32;
  }
  return len - (len >> 1);
}

template <std::size_t Len, std::size_t Align>
struct aligned_storage {
  struct type {
    alignas(Align) std::byte data[Len];
  };
};
} // namespace detail

template <
    std::size_t Len,
    std::size_t Align = detail::aligned_storage_default_align(Len)>
using aligned_storage_t = typename detail::aligned_storage<Len, Align>::type;

template <typename T>
using aligned_storage_for_t = aligned_storage_t<sizeof(T), alignof(T)>;

/// is_instantiation_of_v
/// is_instantiation_of
/// instantiated_from
/// uncvref_instantiated_from
///
/// A trait variable and type to check if a given type is an instantiation of a
/// class template. And corresponding concepts.
///
/// Note that this only works with type template parameters. It does not work
/// with non-type template parameters, template template parameters, or alias
/// templates.
template<template<typename...> class, typename>
inline constexpr bool is_instantiation_of_v = false;
template<template<typename...> class C, typename... T>
inline constexpr bool is_instantiation_of_v<C, C<T...>> = true;
template<template<typename...> class C, typename... T>
struct is_instantiation_of : std::bool_constant<is_instantiation_of_v<C, T...>>
{
};

#if defined(__cpp_concepts)

template<typename T, template<typename...> class Templ>
concept instantiated_from = is_instantiation_of_v<Templ, T>;

template<typename T, template<typename...> class Templ>
concept uncvref_instantiated_from = is_instantiation_of_v<Templ, std::remove_cvref_t<T>>;

#endif

/// member_pointer_traits
///
/// For a member-pointer, reveals its constituent member-type and object-type.
///
/// Works for both member-object-pointer and member-function-pointer.
template<typename>
struct member_pointer_traits;
template<typename M, typename O>
struct member_pointer_traits<M O::*>
{
  using member_type = M;
  using object_type = O;
};

//  unsafe_default_initialized
//  unsafe_default_initialized_cv
//
//  An object which is explicitly convertible to any default-constructible type
//  and which, upon conversion, yields a default-initialized value of that type.
//
//  https://en.cppreference.com/w/cpp/language/default_initialization
//
//  For fundamental types, a default-initialized instance may have indeterminate
//  value. Reading an indeterminate value is undefined behavior but may offer a
//  performance optimization. When using an indeterminate value as a performance
//  optimization, it is best to be explicit.
//
//  Useful as an escape hatch when enabling warnings or errors:
//  * gcc:
//    * uninitialized
//    * maybe-uninitialized
//  * clang:
//    * uninitialized
//    * conditional-uninitialized
//    * sometimes-uninitialized
//    * uninitialized-const-reference
//  * msvc:
//    * C4701: potentially uninitialized local variable used
//    * C4703: potentially uninitialized local pointer variable used
//
//  Example:
//
//      int local = folly::unsafe_default_initialized;
//      store_value_into_int_ptr(&value); // suppresses possible warning
//      use_value(value); // suppresses possible warning
// DuckStation: Upstream's FOLLY_PUSH_WARNING etc. expanded to pragmas, since the macros from folly/CPortability.h are
// not ported. As in CPortability.h, check for GCC/clang first so clang-cl uses the clang pragmas.
struct unsafe_default_initialized_cv {
#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wuninitialized"
  #ifndef __clang__
    #pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
  #endif
#elif defined(_MSC_VER)
  #pragma warning(push)
  #pragma warning(disable:4700)
  #pragma warning(disable:4701)
  #pragma warning(disable:4703)
  #pragma warning(disable:6001)
#endif

  template <typename T>
  FOLLY_ERASE constexpr /* implicit */ operator T() const noexcept {
#if defined(__cpp_lib_is_constant_evaluated)
#if __cpp_lib_is_constant_evaluated >= 201811L
#if (defined(_MSC_VER) && !defined(__MSVC_RUNTIME_CHECKS)) || \
    (defined(__clang__) && !defined(__GNUC__))
    if (!std::is_constant_evaluated()) {
      T uninit;
      return uninit;
    }
#endif
#endif
#endif
    return T();
  }

#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
  #pragma warning(pop)
#endif
};
inline constexpr unsafe_default_initialized_cv unsafe_default_initialized{};

//  register_pass_max_size
//
//  The platform-specific maximum size of a value which may be passed by-value
//  in registers.
//
//  According to each platform ABI, trivially-copyable types up to this maximum
//  size may, if the stars align, be passed by-value in registers rather than
//  implicitly by-reference to stack copies.
//
//  Approximate. Accuracy is not promised.
#ifdef _MSC_VER
constexpr std::size_t register_pass_max_size = 1u * sizeof(void*);
#else
constexpr std::size_t register_pass_max_size = 2u * sizeof(void*);
#endif

//  is_register_pass_v
//
//  Whether a value may be passed in a register.
//
//  Trivially-copyable values up to register_pass_max_size in width may be
//  passed by-value in registers rather than implicitly by-reference to stack
//  copies.
//
//  Approximate. Accuracy is not promised.
template<typename T>
constexpr bool is_register_pass_v = (sizeof(T) <= register_pass_max_size) && std::is_trivially_copyable_v<T>;
template<typename T>
constexpr bool is_register_pass_v<T&> = true;
template<typename T>
constexpr bool is_register_pass_v<T&&> = true;

//  folly/lang/New.h
//  DuckStation: Only the overloads used by Function, calling ::operator new/delete directly instead of going through
//  __builtin_operator_new/__builtin_operator_delete. The builtin dispatch needs FOLLY_CREATE_QUAL_INVOKER and friends,
//  and only lets clang elide or merge allocations.

namespace detail {

#if defined(__cpp_sized_deallocation)
constexpr auto cpp_sized_deallocation_ = __cpp_sized_deallocation >= 201309L;
#else
constexpr auto cpp_sized_deallocation_ = false;
#endif

template <typename... A>
FOLLY_ERASE void do_op_del_sized_(
    void* const p, std::size_t const s, A const... a) {
  if constexpr (detail::cpp_sized_deallocation_) {
    return ::operator delete(p, s, a...);
  } else {
    return ::operator delete(p, a...);
  }
}

} // namespace detail

//  operator_new
struct operator_new_fn {
  [[nodiscard]] FOLLY_ERASE void* operator()( //
      std::size_t const s) const {
    return ::operator new(s);
  }
  [[nodiscard]] FOLLY_ERASE void* operator()( //
      std::size_t const s,
      std::align_val_t const a) const {
    return ::operator new(s, a);
  }
};
inline constexpr operator_new_fn operator_new{};

//  operator_delete
struct operator_delete_fn {
  FOLLY_ERASE void operator()( //
      void* const p,
      std::size_t const s) const noexcept {
    return detail::do_op_del_sized_(p, s);
  }
  FOLLY_ERASE void operator()( //
      void* const p,
      std::size_t const s,
      std::align_val_t const a) const noexcept {
    return detail::do_op_del_sized_(p, s, a);
  }
};
inline constexpr operator_delete_fn operator_delete{};

template <typename FunctionType>
class Function;

template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const> constCastFunction(
    Function<ReturnType(Args...)>&&) noexcept;

template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const noexcept> constCastFunction(
    Function<ReturnType(Args...) noexcept>&&) noexcept;

namespace detail {
namespace function {

enum class Op { MOVE, NUKE, HEAP };

union Data {
  struct BigTrivialLayout {
    void* big;
    std::size_t size;
    std::size_t align;
  };

  void* big;
  BigTrivialLayout bigt;
  folly::aligned_storage_t<6 * sizeof(void*)> tiny;
};

struct CoerceTag {};

template <typename T>
using FunctionNullptrTest =
    decltype(static_cast<bool>(static_cast<T const&>(T(nullptr)) == nullptr));

template <typename T>
constexpr bool IsNullptrCompatible = is_detected_v<FunctionNullptrTest, T>;

template <typename T, std::enable_if_t<!IsNullptrCompatible<T>, int> = 0>
constexpr bool isEmptyFunction(T const&) {
  return false;
}
template <typename T, std::enable_if_t<IsNullptrCompatible<T>, int> = 0>
constexpr bool isEmptyFunction(T const& t) {
  return static_cast<bool>(t == nullptr);
}

template <typename F, typename... Args>
using CallableResult = decltype(FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(Args&&)...));

template <typename F, typename... Args>
constexpr bool CallableNoexcept =
    noexcept(FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(Args&&)...));

template <
    typename From,
    typename To,
    typename = typename std::enable_if<
        !std::is_reference<To>::value || std::is_reference<From>::value>::type>
using IfSafeResultImpl = decltype(void(static_cast<To>(FOLLY_DECLVAL(From))));

#if defined(_MSC_VER)
//  Need a workaround for MSVC to avoid the inscrutable error:
//
//      folly\function.h(...) : fatal error C1001: An internal error has
//          occurred in the compiler.
//      (compiler file 'f:\dd\vctools\compiler\utc\src\p2\main.c', line 258)
//      To work around this problem, try simplifying or changing the program
//          near the locations listed above.
template <typename T>
using CallArg = T&&;
#else
template <typename T>
using CallArg = std::conditional_t<is_register_pass_v<T>, T, T&&>;
#endif

template <typename F, bool Nx, typename R, typename... A>
class FunctionTraitsSharedProxy {
  std::shared_ptr<Function<F>> sp_;

 public:
  explicit FunctionTraitsSharedProxy(std::nullptr_t) noexcept {}
  explicit FunctionTraitsSharedProxy(Function<F>&& func)
      : sp_(func ? std::make_shared<Function<F>>(std::move(func))
                 : std::shared_ptr<Function<F>>()) {}
  R operator()(A... args) const noexcept(Nx) {
    if (!sp_) {
      std::abort(); //throw_exception<std::bad_function_call>();
    }
    return (*sp_)(static_cast<A&&>(args)...);
  }

  explicit operator bool() const noexcept { return sp_ != nullptr; }

  friend bool operator==(
      FunctionTraitsSharedProxy const& proxy, std::nullptr_t) noexcept {
    return proxy.sp_ == nullptr;
  }
  friend bool operator!=(
      FunctionTraitsSharedProxy const& proxy, std::nullptr_t) noexcept {
    return proxy.sp_ != nullptr;
  }

  friend bool operator==(
      std::nullptr_t, FunctionTraitsSharedProxy const& proxy) noexcept {
    return proxy.sp_ == nullptr;
  }
  friend bool operator!=(
      std::nullptr_t, FunctionTraitsSharedProxy const& proxy) noexcept {
    return proxy.sp_ != nullptr;
  }
};

template <
    typename Fun,
    bool Small,
    bool Nx,
    typename ReturnType,
    typename... Args>
ReturnType call_(Args... args, Data& p) noexcept(Nx) {
  auto& fn = *static_cast<Fun*>(Small ? &p.tiny : p.big);
  if constexpr (std::is_void<ReturnType>::value) {
    fn(static_cast<Args&&>(args)...);
  } else {
    return fn(static_cast<Args&&>(args)...);
  }
}

template <typename FunctionType>
struct FunctionTraits;

template <typename ReturnType, typename... Args>
struct FunctionTraits<ReturnType(Args...)> {
  using Call = ReturnType (*)(CallArg<Args>..., Data&);
  using ConstSignature = ReturnType(Args...) const;
  using NonConstSignature = ReturnType(Args...);
  using OtherSignature = ConstSignature;

  // DuckStation: Upstream only checks static_cast<ReturnType>(F& invocation), which accepts explicit-only conversions
  // of the result and callables that cannot be invoked as rvalues. Additionally apply the std::move_only_function
  // constraints, so code that builds against this polyfill also builds against the standard library implementation.
  template <
      typename F,
      typename R = CallableResult<F&, Args...>,
      std::enable_if_t<
          std::is_invocable_r_v<ReturnType, F, Args...> &&
              std::is_invocable_r_v<ReturnType, F&, Args...>,
          int> = 0>
  using IfSafeResult = IfSafeResultImpl<R, ReturnType>;

  template <typename Fun, bool Small>
  static constexpr Call call =
      call_<Fun, Small, false, ReturnType, CallArg<Args>...>;

  static ReturnType uninitCall(CallArg<Args>..., Data&) {
    std::abort(); //throw_exception<std::bad_function_call>();
  }

  ReturnType operator()(Args... args) {
    auto& fn = *static_cast<Function<NonConstSignature>*>(this);
    return fn.call_(static_cast<Args&&>(args)..., fn.data_);
  }

  using SharedProxy =
      FunctionTraitsSharedProxy<NonConstSignature, false, ReturnType, Args...>;
};

template <typename ReturnType, typename... Args>
struct FunctionTraits<ReturnType(Args...) const> {
  using Call = ReturnType (*)(CallArg<Args>..., Data&);
  using ConstSignature = ReturnType(Args...) const;
  using NonConstSignature = ReturnType(Args...);
  using OtherSignature = NonConstSignature;

  // DuckStation: See the non-const specialization above.
  template <
      typename F,
      typename R = CallableResult<const F&, Args...>,
      std::enable_if_t<
          std::is_invocable_r_v<ReturnType, const F, Args...> &&
              std::is_invocable_r_v<ReturnType, const F&, Args...>,
          int> = 0>
  using IfSafeResult = IfSafeResultImpl<R, ReturnType>;

  template <typename Fun, bool Small>
  static constexpr Call call =
      call_<const Fun, Small, false, ReturnType, CallArg<Args>...>;

  static ReturnType uninitCall(CallArg<Args>..., Data&) {
    std::abort(); //throw_exception<std::bad_function_call>();
  }

  ReturnType operator()(Args... args) const {
    auto& fn = *static_cast<const Function<ConstSignature>*>(this);
    return fn.call_(static_cast<Args&&>(args)..., fn.data_);
  }

  using SharedProxy =
      FunctionTraitsSharedProxy<ConstSignature, false, ReturnType, Args...>;
};

template <typename ReturnType, typename... Args>
struct FunctionTraits<ReturnType(Args...) noexcept> {
  using Call = ReturnType (*)(CallArg<Args>..., Data&) noexcept;
  using ConstSignature = ReturnType(Args...) const noexcept;
  using NonConstSignature = ReturnType(Args...) noexcept;
  using OtherSignature = ConstSignature;

  template <
      typename F,
      typename R = CallableResult<F&, Args...>,
      // DuckStation: Upstream uses CallableNoexcept<F&, Args...>, which ignores the conversion of the result to
      // ReturnType. Use the std::move_only_function constraints instead, see the non-noexcept specialization.
      std::enable_if_t<
          std::is_nothrow_invocable_r_v<ReturnType, F, Args...> &&
              std::is_nothrow_invocable_r_v<ReturnType, F&, Args...>,
          int> = 0>
  using IfSafeResult = IfSafeResultImpl<R, ReturnType>;

  template <typename Fun, bool Small>
  static constexpr Call call =
      call_<Fun, Small, true, ReturnType, CallArg<Args>...>;

  static ReturnType uninitCall(CallArg<Args>..., Data&) noexcept {
    std::abort(); //terminate_with<std::bad_function_call>();
  }

  ReturnType operator()(Args... args) noexcept {
    auto& fn = *static_cast<Function<NonConstSignature>*>(this);
    return fn.call_(static_cast<Args&&>(args)..., fn.data_);
  }

  using SharedProxy =
      FunctionTraitsSharedProxy<NonConstSignature, true, ReturnType, Args...>;
};

template <typename ReturnType, typename... Args>
struct FunctionTraits<ReturnType(Args...) const noexcept> {
  using Call = ReturnType (*)(CallArg<Args>..., Data&) noexcept;
  using ConstSignature = ReturnType(Args...) const noexcept;
  using NonConstSignature = ReturnType(Args...) noexcept;
  using OtherSignature = NonConstSignature;

  template <
      typename F,
      typename R = CallableResult<const F&, Args...>,
      // DuckStation: See the non-const noexcept specialization above.
      std::enable_if_t<
          std::is_nothrow_invocable_r_v<ReturnType, const F, Args...> &&
              std::is_nothrow_invocable_r_v<ReturnType, const F&, Args...>,
          int> = 0>
  using IfSafeResult = IfSafeResultImpl<R, ReturnType>;

  template <typename Fun, bool Small>
  static constexpr Call call =
      call_<const Fun, Small, true, ReturnType, CallArg<Args>...>;

  static ReturnType uninitCall(CallArg<Args>..., Data&) noexcept {
    std::abort(); //terminate_with<std::bad_function_call>();
  }

  ReturnType operator()(Args... args) const noexcept {
    auto& fn = *static_cast<const Function<ConstSignature>*>(this);
    return fn.call_(static_cast<Args&&>(args)..., fn.data_);
  }

  using SharedProxy =
      FunctionTraitsSharedProxy<ConstSignature, true, ReturnType, Args...>;
};

// These are control functions. They type-erase the operations of move-
// construction, destruction, and conversion to bool.
//
// The interface operations are noexcept, so the implementations are as well.
// Having the implementations be noexcept in the type permits callers to omit
// exception-handling machinery.
//
// This is intentionally instantiated per size rather than per function in order
// to minimize the number of instantiations. It would be safe to minimize
// instantiations even more by simply having a single non-template function that
// copies sizeof(Data) bytes rather than only copying sizeof(Fun) bytes, but
// then for small function types it would be likely to cross cache lines without
// need. But it is only necessary to handle those sizes which are multiples of
// the alignof(Data), and to round up other sizes.
struct DispatchSmallTrivial {
  static constexpr bool is_in_situ = true;
  static constexpr bool is_trivial = true;

  template <std::size_t Size>
  static std::size_t exec_(Op o, Data* src, Data* dst) noexcept {
    switch (o) {
      case Op::MOVE:
        std::memcpy(static_cast<void*>(dst), static_cast<void*>(src), Size);
        break;
      case Op::NUKE:
        break;
      case Op::HEAP:
        break;
      default: /* unexpected */
        abort();
    }
    return 0U;
  }
  template <std::size_t size, std::size_t adjust = alignof(Data) - 1>
  static constexpr std::size_t size_ = (size + adjust) & ~adjust;
  template <typename Fun>
  static constexpr auto exec = exec_<size_<sizeof(Fun)>>;
};

struct DispatchBigTrivial {
  static constexpr bool is_in_situ = false;
  static constexpr bool is_trivial = true;

  template <typename Fun, typename Base>
  static constexpr auto call = Base::template callBig<Fun>;

  static constexpr bool is_align_large(size_t align) {
    return align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }

  template <bool IsAlignLarge>
  static std::size_t exec_(Op o, Data* src, Data* dst) noexcept {
    switch (o) {
      case Op::MOVE:
        dst->bigt = src->bigt;
        src->bigt = {};
        break;
      case Op::NUKE:
        IsAlignLarge
            ? operator_delete(
                  src->big, src->bigt.size, std::align_val_t(src->bigt.align))
            : operator_delete(src->big, src->bigt.size);
        break;
      case Op::HEAP:
        break;
      default: /* unexpected */
        abort();
    }
    return src->bigt.size;
  }
  template <typename T>
  static constexpr auto exec = exec_<is_align_large(alignof(T))>;

  FOLLY_ALWAYS_INLINE static void ctor(
      Data& data,
      void const* fun,
      std::size_t size,
      std::size_t align) noexcept {
    // cannot use type-specific new since type-specific new is overrideable
    // in concert with type-specific delete
    data.bigt.big = is_align_large(align)
        ? operator_new(size, std::align_val_t(align))
        : operator_new(size);
    data.bigt.size = size;
    data.bigt.align = align;
    std::memcpy(data.bigt.big, fun, size);
  }
};

struct DispatchSmall {
  static constexpr bool is_in_situ = true;
  static constexpr bool is_trivial = false;

  template <typename Fun>
  static std::size_t exec(Op o, Data* src, Data* dst) noexcept {
    switch (o) {
      case Op::MOVE:
        ::new (static_cast<void*>(&dst->tiny))
            Fun(static_cast<Fun&&>(
                *static_cast<Fun*>(static_cast<void*>(&src->tiny))));
        [[fallthrough]];
      case Op::NUKE:
        static_cast<Fun*>(static_cast<void*>(&src->tiny))->~Fun();
        break;
      case Op::HEAP:
        break;
      default: /* unexpected */
        abort();
    }
    return 0U;
  }
};

struct DispatchBig {
  static constexpr bool is_in_situ = false;
  static constexpr bool is_trivial = false;

  template <typename Fun>
  static std::size_t exec(Op o, Data* src, Data* dst) noexcept {
    switch (o) {
      case Op::MOVE:
        dst->big = src->big;
        src->big = nullptr;
        break;
      case Op::NUKE:
        delete static_cast<Fun*>(src->big);
        break;
      case Op::HEAP:
        break;
      default: /* unexpected */
        abort();
    }
    return sizeof(Fun);
  }
};

template <bool InSitu, bool IsTriv>
struct Dispatch;
template <>
struct Dispatch<true, true> : DispatchSmallTrivial {};
template <>
struct Dispatch<true, false> : DispatchSmall {};
template <>
struct Dispatch<false, true> : DispatchBigTrivial {};
template <>
struct Dispatch<false, false> : DispatchBig {};

template <
    typename Fun,
    bool InSituSize = sizeof(Fun) <= sizeof(Data),
    bool InSituAlign = alignof(Fun) <= alignof(Data),
    // DuckStation: Upstream uses noexcept(Fun(FOLLY_DECLVAL(Fun))), which is always true due to guaranteed copy
    // elision. Moves happen in noexcept functions, so a throwing move must go on the heap. Without exceptions, nothing
    // can throw, so keep everything in situ regardless of the noexcept specification.
    bool InSituNoexcept =
        !FOLLY_HAS_EXCEPTIONS || std::is_nothrow_move_constructible_v<Fun>>
using DispatchOf = Dispatch<
    InSituSize && InSituAlign && InSituNoexcept,
    std::is_trivially_copyable_v<Fun>>;

// This cannot be done inside `Function` class, because the word
// `Function` there refers to the instantiation and not the template.
template <typename T>
constexpr bool is_instantiation_of_folly_function_v =
    is_instantiation_of_v<Function, T>;

} // namespace function
} // namespace detail

template <typename FunctionType>
class Function final : private detail::function::FunctionTraits<FunctionType> {
  // These utility types are defined outside of the template to reduce
  // the number of instantiations, and then imported in the class
  // namespace for convenience.
  using Data = detail::function::Data;
  using Op = detail::function::Op;
  using CoerceTag = detail::function::CoerceTag;

  using Traits = detail::function::FunctionTraits<FunctionType>;
  using Call = typename Traits::Call;
  using Exec = std::size_t (*)(Op, Data*, Data*) noexcept;

  // The `data_` member is mutable to allow `constCastFunction` to work without
  // invoking undefined behavior. Const-correctness is only violated when
  // `FunctionType` is a const function type (e.g., `int() const`) and `*this`
  // is the result of calling `constCastFunction`.
  mutable Data data_{unsafe_default_initialized};
  Call call_{&Traits::uninitCall};
  Exec exec_{nullptr};

  std::size_t exec(Op o, Data* src, Data* dst) const {
    if (!exec_) {
      return 0U;
    }
    return exec_(o, src, dst);
  }

  friend Traits;
  friend Function<typename Traits::ConstSignature> folly::constCastFunction<>(
      Function<typename Traits::NonConstSignature>&&) noexcept;
  friend class Function<typename Traits::OtherSignature>;

  template <typename Signature>
  Function(Function<Signature>&& fun, CoerceTag) {
    using Fun = Function<Signature>;
    if (fun) {
      data_.big = new Fun(static_cast<Fun&&>(fun));
      call_ = Traits::template call<Fun, false>;
      exec_ = Exec(detail::function::DispatchBig::exec<Fun>);
    }
  }

  Function(Function<typename Traits::OtherSignature>&& that, CoerceTag) noexcept
      : call_(that.call_), exec_(that.exec_) {
    that.call_ = &Traits::uninitCall;
    that.exec_ = nullptr;
    exec(Op::MOVE, &that.data_, &data_);
  }

 public:
  /**
   * Default constructor. Constructs an empty Function.
   */
  constexpr Function() = default;

  // not copyable
  Function(const Function&) = delete;

#ifdef __OBJC__
  // Make sure Objective C blocks are copied
  template <class ReturnType, class... Args>
  /*implicit*/ Function(ReturnType (^objCBlock)(Args... args))
      : Function([blockCopy = (ReturnType(^)(Args...))[objCBlock copy]](
                     Args... args) { return blockCopy(args...); }) {}
#endif

  /**
   * Move constructor
   */
  Function(Function&& that) noexcept : call_(that.call_), exec_(that.exec_) {
    // that must be uninitialized before exec() call in the case of self move
    that.call_ = &Traits::uninitCall;
    that.exec_ = nullptr;
    exec(Op::MOVE, &that.data_, &data_);
  }

  /**
   * Constructs an empty `Function`.
   */
  /* implicit */ constexpr Function(std::nullptr_t) noexcept {}

  /**
   * Constructs a new `Function` from any callable object that is _not_ a
   * `folly::Function`.
   *
   * \note `typename Traits::template IfSafeResult<Fun>` prevents this overload
   * from being selected by overload resolution when `fun` is not a compatible
   * function.
   *
   * \note The noexcept requires some explanation. `is_in_situ` is true when the
   * decayed type fits within the internal buffer and is noexcept-movable. But
   * this ctor might copy, not move. What we need here, if this ctor does a
   * copy, is that this ctor be noexcept when the copy is noexcept. That is not
   * checked in `is_in_situ`, and shouldn't be, because once the `Function` is
   * constructed, the contained object is never copied. This check is for this
   * ctor only, in the case that this ctor does a copy.
   *
   * @param fun function pointers, pointers to static
   *     member functions, `std::reference_wrapper` objects, `std::function`
   *     objects, and arbitrary objects that implement `operator()` if the
   * parameter signature matches (i.e. it returns an object convertible to `R`
   * when called with `Args...`).
   */
  template <
      typename Fun,
      typename = std::enable_if_t<
          !detail::function::is_instantiation_of_folly_function_v<Fun>>,
      typename = typename Traits::template IfSafeResult<Fun>>
  /* implicit */ constexpr Function(Fun fun) noexcept(
      detail::function::DispatchOf<Fun>::is_in_situ) {
    using Dispatch = detail::function::DispatchOf<Fun>;
    if constexpr (detail::function::IsNullptrCompatible<Fun>) {
      if (detail::function::isEmptyFunction(fun)) {
        return;
      }
    }
    if constexpr (Dispatch::is_in_situ) {
      if constexpr (
          !std::is_empty<Fun>::value || !std::is_trivially_copyable_v<Fun>) {
        ::new (&data_.tiny) Fun(static_cast<Fun&&>(fun));
      }
    } else {
      if constexpr (Dispatch::is_trivial) {
        Dispatch::ctor(data_, &fun, sizeof(Fun), alignof(Fun));
      } else {
        data_.big = new Fun(static_cast<Fun&&>(fun));
      }
    }
    call_ = Traits::template call<Fun, Dispatch::is_in_situ>;
    exec_ = Exec(Dispatch::template exec<Fun>);
  }

  /**
   * For move-constructing from a `folly::Function<X(Ys...) [const?]>`.
   *
   * For a `Function` with a `const` function type, the object must be
   * callable from a `const`-reference, i.e. implement `operator() const`.
   * For a `Function` with a non-`const` function type, the object will
   * be called from a non-const reference, which means that it will execute
   * a non-const `operator()` if it is defined, and falls back to
   * `operator() const` otherwise.
   */
  template <
      typename Signature,
      typename Fun = Function<Signature>,
      // prevent gcc from making this a better match than move-ctor
      typename = std::enable_if_t<!std::is_same<Function, Fun>::value>,
      typename = typename Traits::template IfSafeResult<Fun>>
  Function(Function<Signature>&& that) noexcept(
      noexcept(Function(std::move(that), CoerceTag{})))
      : Function(std::move(that), CoerceTag{}) {}

  /**
   * If `ptr` is null, constructs an empty `Function`. Otherwise,
   * this constructor is equivalent to `Function(std::mem_fn(ptr))`.
   */
  template <
      typename Member,
      typename Class,
      // Prevent this overload from being selected when `ptr` is not a
      // compatible member function pointer.
      typename = decltype(Function(std::mem_fn((Member Class::*)0)))>
  /* implicit */ Function(Member Class::* ptr) noexcept {
    if (ptr) {
      *this = std::mem_fn(ptr);
    }
  }

  ~Function() { exec(Op::NUKE, &data_, nullptr); }

  Function& operator=(const Function&) = delete;

#ifdef __OBJC__
  // Make sure Objective C blocks are copied
  template <class ReturnType, class... Args>
  /* implicit */ Function& operator=(ReturnType (^objCBlock)(Args... args)) {
    (*this) =
        [blockCopy = (ReturnType(^)(Args...))[objCBlock copy]](Args... args) {
          return blockCopy(args...);
        };
    return *this;
  }
#endif

  /**
   * Move assignment operator
   *
   * \note Leaves `that` in a valid but unspecified state. If `&that == this`
   * then `*this` is left in a valid but unspecified state.
   */
  Function& operator=(Function&& that) noexcept {
    exec(Op::NUKE, &data_, nullptr);
    if (this != &that) [[likely]] {
      that.exec(Op::MOVE, &that.data_, &data_);
      exec_ = that.exec_;
      call_ = that.call_;
    }
    that.exec_ = nullptr;
    that.call_ = &Traits::uninitCall;
    return *this;
  }

  /**
   * Assigns a callable object to this `Function`. If the operation fails,
   * `*this` is left unmodified.
   *
   * \note `typename = decltype(Function(FOLLY_DECLVAL(Fun&&)))` prevents this
   * overload from being selected by overload resolution when `fun` is not a
   * compatible function.
   */
  template <
      typename Fun,
      typename...,
      bool Nx = noexcept(Function(FOLLY_DECLVAL(Fun&&)))>
  Function& operator=(Fun fun) noexcept(Nx) {
    // Doing this in place is more efficient when we can do so safely.
    if (Nx) {
      // Q: Why is is safe to destroy and reconstruct this object in place?
      // A: See the explanation in the move assignment operator.
      this->~Function();
      ::new (this) Function(static_cast<Fun&&>(fun));
    } else {
      // Construct a temporary and (nothrow) swap.
      Function(static_cast<Fun&&>(fun)).swap(*this);
    }
    return *this;
  }

  /**
   * For assigning from a `Function<X(Ys..) [const?]>`.
   */
  template <
      typename Signature,
      typename...,
      typename = typename Traits::template IfSafeResult<Function<Signature>>>
  Function& operator=(Function<Signature>&& that) noexcept(
      noexcept(Function(std::move(that)))) {
    return (*this = Function(std::move(that)));
  }

  /**
   * Clears this `Function`.
   */
  Function& operator=(std::nullptr_t) noexcept { return (*this = Function()); }

  /**
   * If `ptr` is null, clears this `Function`. Otherwise, this assignment
   * operator is equivalent to `*this = std::mem_fn(ptr)`.
   */
  template <typename Member, typename Class>
  auto operator=(Member Class::* ptr) noexcept
      // Prevent this overload from being selected when `ptr` is not a
      // compatible member function pointer.
      -> decltype(operator=(std::mem_fn(ptr))) {
    return ptr ? (*this = std::mem_fn(ptr)) : (*this = Function());
  }

  /**
   * Call the wrapped callable object with the specified arguments.
   */
  using Traits::operator();

  /**
   * Exchanges the callable objects of `*this` and `that`.
   *
   * @param that a folly::Function ref
   */
  void swap(Function& that) noexcept { std::swap(*this, that); }

  /**
   * Returns `true` if this `Function` contains a callable, i.e. is
   * non-empty.
   */
  explicit operator bool() const noexcept { return exec_ != nullptr; }

  /**
   * Returns the size of the allocation made to store the callable on the
   * heap. If `0` is returned, there has been no additional memory
   * allocation because the callable is stored within the `Function` object.
   */
  std::size_t heapAllocatedMemory() const noexcept {
    return exec(Op::HEAP, &data_, nullptr);
  }

  using typename Traits::SharedProxy;

  /**
   * Move this `Function` into a copyable callable object, of which all copies
   * share the state.
   */
  SharedProxy asSharedProxy() && { return SharedProxy{std::move(*this)}; }

  /**
   * Construct a `std::function` by moving in the contents of this `Function`.
   * Note that the returned `std::function` will share its state (i.e. captured
   * data) across all copies you make of it, so be very careful when copying.
   */
  std::function<typename Traits::NonConstSignature> asStdFunction() && {
    return std::move(*this).asSharedProxy();
  }
};

template <typename FunctionType>
void swap(Function<FunctionType>& lhs, Function<FunctionType>& rhs) noexcept {
  lhs.swap(rhs);
}

template <typename FunctionType>
bool operator==(const Function<FunctionType>& fn, std::nullptr_t) {
  return !fn;
}

template <typename FunctionType>
bool operator==(std::nullptr_t, const Function<FunctionType>& fn) {
  return !fn;
}

template <typename FunctionType>
bool operator!=(const Function<FunctionType>& fn, std::nullptr_t) {
  return !(fn == nullptr);
}

template <typename FunctionType>
bool operator!=(std::nullptr_t, const Function<FunctionType>& fn) {
  return !(nullptr == fn);
}

/**
 * Casts a `folly::Function` from non-const to a const signature.
 *
 * NOTE: The name of `constCastFunction` should warn you that something
 * potentially dangerous is happening. As a matter of fact, using
 * `std::function` always involves this potentially dangerous aspect, which
 * is why it is not considered fully const-safe or even const-correct.
 * However, in most of the cases you will not need the dangerous aspect at all.
 * Either you do not require invocation of the function from a const context,
 * in which case you do not need to use `constCastFunction` and just
 * use a non-const `folly::Function`. Or, you may need invocation from const,
 * but the callable you are wrapping does not mutate its state (e.g. it is a
 * class object and implements `operator() const`, or it is a normal,
 * non-mutable lambda), in which case you can wrap the callable in a const
 * `folly::Function` directly, without using `constCastFunction`.
 * Only if you require invocation from a const context of a callable that
 * may mutate itself when invoked you have to go through the above procedure.
 * However, in that case what you do is potentially dangerous and requires
 * the equivalent of a `const_cast`, hence you need to call
 * `constCastFunction`.
 *
 * @param that a non-const folly::Function.
 */
template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const> constCastFunction(
    Function<ReturnType(Args...)>&& that) noexcept {
  return Function<ReturnType(Args...) const>{
      std::move(that), detail::function::CoerceTag{}};
}

template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const> constCastFunction(
    Function<ReturnType(Args...) const>&& that) noexcept {
  return std::move(that);
}

template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const noexcept> constCastFunction(
    Function<ReturnType(Args...) noexcept>&& that) noexcept {
  return Function<ReturnType(Args...) const noexcept>{
      std::move(that), detail::function::CoerceTag{}};
}

template <typename ReturnType, typename... Args>
Function<ReturnType(Args...) const noexcept> constCastFunction(
    Function<ReturnType(Args...) const noexcept>&& that) noexcept {
  return std::move(that);
}

namespace detail {

template <typename Void, typename>
struct function_ctor_deduce_;

template <typename P>
struct function_ctor_deduce_<
    std::enable_if_t<std::is_function<std::remove_pointer_t<P>>::value>,
    P> {
  using type = std::remove_pointer_t<P>;
};

template <typename F>
struct function_ctor_deduce_<std::void_t<decltype(&F::operator())>, F> {
  using type =
      typename member_pointer_traits<decltype(&F::operator())>::member_type;
};

template <typename F>
using function_ctor_deduce_t = typename function_ctor_deduce_<void, F>::type;

} // namespace detail

template <typename F>
Function(F) -> Function<detail::function_ctor_deduce_t<F>>;

/**
 * @class folly::FunctionRef
 *
 * A reference wrapper for callable objects
 *
 * FunctionRef is similar to std::reference_wrapper, but the template parameter
 * is the function signature type rather than the type of the referenced object.
 * A folly::FunctionRef is cheap to construct as it contains only a pointer to
 * the referenced callable and a pointer to a function which invokes the
 * callable.
 *
 * The user of FunctionRef must be aware of the reference semantics: storing a
 * copy of a FunctionRef is potentially dangerous and should be avoided unless
 * the referenced object definitely outlives the FunctionRef object. Thus any
 * function that accepts a FunctionRef parameter should only use it to invoke
 * the referenced function and not store a copy of it. Knowing that FunctionRef
 * itself has reference semantics, it is generally okay to use it to reference
 * lambdas that capture by reference.
 */

template <typename FunctionType>
class FunctionRef;

template <typename ReturnType, typename... Args>
class FunctionRef<ReturnType(Args...)> final {
  template <typename Arg>
  using CallArg = detail::function::CallArg<Arg>;

  using Call = ReturnType (*)(CallArg<Args>..., void*);

  static ReturnType uninitCall(CallArg<Args>..., void*) {
    std::abort(); //throw_exception<std::bad_function_call>();
  }

  template <
      typename Fun,
      std::enable_if_t<!std::is_pointer<Fun>::value, int> = 0>
  static ReturnType call(CallArg<Args>... args, void* object) {
    using Pointer = std::add_pointer_t<Fun>;
    return static_cast<ReturnType>(std::invoke(
        static_cast<Fun&&>(*static_cast<Pointer>(object)),
        static_cast<Args&&>(args)...));
  }
  template <
      typename Fun,
      std::enable_if_t<std::is_pointer<Fun>::value, int> = 0>
  static ReturnType call(CallArg<Args>... args, void* object) {
    return static_cast<ReturnType>(std::invoke(
        reinterpret_cast<Fun>(object), static_cast<Args&&>(args)...));
  }

  void* object_{nullptr};
  Call call_{&FunctionRef::uninitCall};

 public:
  /**
   * Default constructor. Constructs an empty FunctionRef.
   *
   * Invoking it will throw std::bad_function_call.
   */
  constexpr FunctionRef() = default;

  /**
   * Like default constructor. Constructs an empty FunctionRef.
   *
   * Invoking it will throw std::bad_function_call.
   */
  constexpr explicit FunctionRef(std::nullptr_t) noexcept {}

  /**
   * Construct a FunctionRef from a reference to a callable object. If the
   * callable is considered to be an empty callable, the FunctionRef will be
   * empty.
   */
  template <
      typename Fun,
      std::enable_if_t<
          std::conjunction_v<
              std::negation<std::is_same<FunctionRef, std::decay_t<Fun>>>,
              std::is_invocable_r<ReturnType, Fun&&, Args&&...>>,
          int> = 0>
  // DuckStation: [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] on fun removed, since folly/CppAttributes.h is not ported.
  constexpr /* implicit */ FunctionRef(Fun&& fun) noexcept {
    // `Fun` may be a const type, in which case we have to do a const_cast
    // to store the address in a `void*`. This is safe because the `void*`
    // will be cast back to `Fun*` (which is a const pointer whenever `Fun`
    // is a const type) inside `FunctionRef::call`
    auto& ref = fun; // work around forwarding lint advice
    if constexpr ( //
        detail::function::IsNullptrCompatible<std::decay_t<Fun>>) {
      if (detail::function::isEmptyFunction(fun)) {
        return;
      }
    }
    auto ptr = std::addressof(ref);
    object_ = const_cast<void*>(static_cast<void const*>(ptr));
    call_ = &FunctionRef::template call<Fun>;
  }

  /**
   * Constructs a FunctionRef from a pointer to a function. If the
   * pointer is nullptr, the FunctionRef will be empty.
   */
  template <
      typename Fun,
      std::enable_if_t<std::is_function<Fun>::value, int> = 0,
      std::enable_if_t<std::is_invocable_r_v<ReturnType, Fun&, Args&&...>, int> = 0>
  constexpr /* implicit */ FunctionRef(Fun* fun) noexcept {
    if (fun) {
      object_ = const_cast<void*>(reinterpret_cast<void const*>(fun));
      call_ = &FunctionRef::template call<Fun*>;
    }
  }

  // DuckStation: FunctionRef(nontype_t<Fn>) constructor removed, since it needs folly's nontype_t. FunctionRef is
  // unused here.

  ReturnType operator()(Args... args) const {
    return call_(static_cast<Args&&>(args)..., object_);
  }

  constexpr explicit operator bool() const noexcept { return object_; }

  constexpr friend bool operator==(
      FunctionRef<ReturnType(Args...)> ref, std::nullptr_t) noexcept {
    return ref.object_ == nullptr;
  }
  constexpr friend bool operator!=(
      FunctionRef<ReturnType(Args...)> ref, std::nullptr_t) noexcept {
    return ref.object_ != nullptr;
  }

  constexpr friend bool operator==(
      std::nullptr_t, FunctionRef<ReturnType(Args...)> ref) noexcept {
    return ref.object_ == nullptr;
  }
  constexpr friend bool operator!=(
      std::nullptr_t, FunctionRef<ReturnType(Args...)> ref) noexcept {
    return ref.object_ != nullptr;
  }
};

} // namespace folly
