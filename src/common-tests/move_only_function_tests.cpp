// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Define MOVE_ONLY_FUNCTION_TEST_DIAGNOSTIC_SCOPE and pass -Wunused-variable -Xclang -verify to clang++ to run the
// compile-only check that the header restores its caller's diagnostic scopes.
#if defined(MOVE_ONLY_FUNCTION_TEST_DIAGNOSTIC_SCOPE)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "common/thirdparty/FollyFunction.h"
#pragma GCC diagnostic pop

void CheckFollyDiagnosticScope()
{
  int should_warn; // expected-warning {{unused variable 'should_warn'}}
}

#else

#include "common/move_only_function.h"
#include "common/thirdparty/FollyFunction.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

// Also compile this file with exceptions and sized deallocation disabled to cover the fallback build configurations.

namespace {

enum class Enum
{
  A
};

struct ExplicitInt
{
  explicit operator int() const { return 1; }
};

struct ImplicitInt
{
  operator int() const { return 1; }
};

struct NothrowInt
{
  operator int() const noexcept { return 1; }
};

struct Base
{
};

struct Derived : Base
{
};

struct Immovable
{
  Immovable() = default;
  Immovable(Immovable&&) = delete;
  int value = 3;
};

struct ReturnsEnum
{
  Enum operator()() const { return Enum::A; }
};

struct ReturnsExplicitInt
{
  ExplicitInt operator()() const { return {}; }
};

struct ReturnsImplicitInt
{
  ImplicitInt operator()() const { return {}; }
};

struct ReturnsInt
{
  int operator()() const { return 1; }
};

struct ReturnsVoid
{
  void operator()() const {}
};

struct ReturnsString
{
  const char* operator()() const { return "x"; }
};

struct ReturnsDerivedPtr
{
  std::unique_ptr<Derived> operator()() const { return std::make_unique<Derived>(); }
};

struct ReturnsDerivedRef
{
  const Derived& operator()() const
  {
    static Derived d;
    return d;
  }
};

struct TakesLong
{
  long operator()(long x) const { return x; }
};

struct NonConstCall
{
  int operator()() noexcept { return 2; }
};

struct NothrowCallThrowingConversion
{
  ImplicitInt operator()() const noexcept { return {}; }
};

struct NothrowCallNothrowConversion
{
  NothrowInt operator()() const noexcept { return {}; }
};

struct NothrowExplicitInt
{
  ExplicitInt operator()() const noexcept { return {}; }
};

struct ReturnsImmovable
{
  Immovable operator()() const { return Immovable{}; }
};

struct NothrowReturnsImmovable
{
  Immovable operator()() const noexcept { return Immovable{}; }
};

struct LvalueOnly
{
  void operator()() & {}
};

struct ConstLvalueOnly
{
  void operator()() const& {}
};

struct RvalueOnly
{
  void operator()() && {}
};

struct LvalueAndRvalue
{
  void operator()() & {}
  void operator()() && {}
};

struct NothrowLvalueOnly
{
  void operator()() & noexcept {}
};

struct NothrowReturnsInt
{
  int operator()() const noexcept { return 1; }
};

struct DeletedConstRvalueCall
{
  void operator()() const& {}
  void operator()() const&& = delete;
};

struct DifferentRvalueResult
{
  int operator()() & { return 1; }
  void operator()() && {}
};

struct DifferentRvalueNoexcept
{
  int operator()() & noexcept { return 1; }
  int operator()() && noexcept(false) { return 1; }
};

struct ReturnsReference
{
  int& operator()() noexcept { return value; }
  const int& operator()() const noexcept { return value; }

  int value = 1;
};

// Checks MoveOnlyFunction, folly::Function and std::move_only_function (if available) against the expected
// std::move_only_function behaviour, so code that builds with one implementation builds with the others. The
// folly::Function fallback is only used when the standard library lacks std::move_only_function, so it is tested
// directly as well.
template<typename Signature, typename F>
constexpr bool IsConstructible(bool expected)
{
  return (std::is_constructible_v<MoveOnlyFunction<Signature>, F> == expected) &&
         (std::is_constructible_v<folly::Function<Signature>, F> == expected)
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
         && (std::is_constructible_v<std::move_only_function<Signature>, F> == expected)
#endif
    ;
}

#define CHECK_CONSTRUCTIBLE(expected, Signature, F)                                                                    \
  static_assert(IsConstructible<Signature, F>(expected), #Signature " from " #F)

// Result conversion must be implicit.
CHECK_CONSTRUCTIBLE(false, int(), ReturnsEnum);
CHECK_CONSTRUCTIBLE(false, int(), ReturnsExplicitInt);
CHECK_CONSTRUCTIBLE(true, int(), ReturnsImplicitInt);
CHECK_CONSTRUCTIBLE(true, long(), ReturnsInt);
CHECK_CONSTRUCTIBLE(true, double(), ReturnsInt);
CHECK_CONSTRUCTIBLE(true, std::string(), ReturnsString);
CHECK_CONSTRUCTIBLE(true, std::unique_ptr<Base>(), ReturnsDerivedPtr);
CHECK_CONSTRUCTIBLE(true, const Base&(), ReturnsDerivedRef);
CHECK_CONSTRUCTIBLE(false, const int&(), ReturnsInt);
CHECK_CONSTRUCTIBLE(true, int(int), TakesLong);

// Void signatures discard the result, no conversion needed.
CHECK_CONSTRUCTIBLE(true, void(), ReturnsInt);
CHECK_CONSTRUCTIBLE(true, void(), ReturnsEnum);
CHECK_CONSTRUCTIBLE(true, void(), ReturnsVoid);
CHECK_CONSTRUCTIBLE(false, int(), ReturnsVoid);

// Const signatures need a const call operator.
CHECK_CONSTRUCTIBLE(true, int() const, ReturnsInt);
CHECK_CONSTRUCTIBLE(true, int() noexcept, NonConstCall);
CHECK_CONSTRUCTIBLE(false, int() const noexcept, NonConstCall);

// Noexcept signatures need both the call and the result conversion to be noexcept.
CHECK_CONSTRUCTIBLE(false, int() noexcept, ReturnsInt);
CHECK_CONSTRUCTIBLE(false, int() noexcept, NothrowCallThrowingConversion);
CHECK_CONSTRUCTIBLE(false, int() const noexcept, NothrowCallThrowingConversion);
CHECK_CONSTRUCTIBLE(true, void() noexcept, NothrowCallThrowingConversion);
CHECK_CONSTRUCTIBLE(true, int() noexcept, NothrowCallNothrowConversion);
CHECK_CONSTRUCTIBLE(true, int() const noexcept, NothrowCallNothrowConversion);
CHECK_CONSTRUCTIBLE(true, void() noexcept, NothrowExplicitInt);

// Immovable results are returned through guaranteed copy elision.
CHECK_CONSTRUCTIBLE(true, Immovable(), ReturnsImmovable);
CHECK_CONSTRUCTIBLE(true, Immovable() const, ReturnsImmovable);
CHECK_CONSTRUCTIBLE(true, Immovable() noexcept, NothrowReturnsImmovable);
CHECK_CONSTRUCTIBLE(true, Immovable() const noexcept, NothrowReturnsImmovable);

// Unqualified signatures need the target to be invocable as both an lvalue and an rvalue.
CHECK_CONSTRUCTIBLE(false, void(), LvalueOnly);
CHECK_CONSTRUCTIBLE(false, void() const, LvalueOnly);
CHECK_CONSTRUCTIBLE(true, void(), ConstLvalueOnly);
CHECK_CONSTRUCTIBLE(true, void() const, ConstLvalueOnly);
CHECK_CONSTRUCTIBLE(false, void(), RvalueOnly);
CHECK_CONSTRUCTIBLE(true, void(), LvalueAndRvalue);
CHECK_CONSTRUCTIBLE(false, void() noexcept, NothrowLvalueOnly);
CHECK_CONSTRUCTIBLE(false, void() const noexcept, NothrowLvalueOnly);
CHECK_CONSTRUCTIBLE(false, void(), DeletedConstRvalueCall);
CHECK_CONSTRUCTIBLE(false, void() const, DeletedConstRvalueCall);
CHECK_CONSTRUCTIBLE(false, int(), DifferentRvalueResult);
CHECK_CONSTRUCTIBLE(true, void(), DifferentRvalueResult);
CHECK_CONSTRUCTIBLE(true, int(), DifferentRvalueNoexcept);
CHECK_CONSTRUCTIBLE(false, int() noexcept, DifferentRvalueNoexcept);

// Reference results must refer to the target's existing storage.
CHECK_CONSTRUCTIBLE(true, int&(), ReturnsReference);
CHECK_CONSTRUCTIBLE(true, int&() noexcept, ReturnsReference);
CHECK_CONSTRUCTIBLE(true, const int&() const, ReturnsReference);
CHECK_CONSTRUCTIBLE(true, const int&() const noexcept, ReturnsReference);

// Wrapping another function object.
CHECK_CONSTRUCTIBLE(true, int(), MoveOnlyFunction<int() const>);
CHECK_CONSTRUCTIBLE(true, void(), MoveOnlyFunction<int()>);
CHECK_CONSTRUCTIBLE(false, int(), MoveOnlyFunction<Enum()>);

#undef CHECK_CONSTRUCTIBLE

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

template<typename... T>
struct TypeList
{
};

using ConstructionSignatures =
  TypeList<void(), void() const, void() noexcept, void() const noexcept, int(), int() const, int() noexcept,
           int() const noexcept, Immovable(), Immovable() const, Immovable() noexcept, Immovable() const noexcept,
           const Immovable(), const Immovable() const, const Immovable() noexcept, const Immovable() const noexcept>;
using ConstructionTargets =
  TypeList<NothrowReturnsImmovable, NonConstCall, NothrowReturnsInt, ReturnsInt, LvalueOnly, DeletedConstRvalueCall,
           RvalueOnly, DifferentRvalueResult, DifferentRvalueNoexcept, NothrowCallNothrowConversion, NothrowExplicitInt,
           NothrowCallThrowingConversion, ReturnsReference>;
using ConversionSignatures =
  TypeList<int(), int() const, int() noexcept, int() const noexcept, void(), void() const, void() noexcept,
           void() const noexcept, long(), long() const, long() noexcept, long() const noexcept>;

// Compare acceptance with the native implementation and instantiate the constructor and invocation bodies as well.
template<template<typename> class Function, typename Signature, typename Target>
void TestConstructionCombination()
{
  constexpr bool expected = std::is_constructible_v<std::move_only_function<Signature>, Target>;
  static_assert(std::is_constructible_v<Function<Signature>, Target> == expected);
  static_assert(!std::is_copy_constructible_v<Function<Signature>>);
  static_assert(!std::is_copy_assignable_v<Function<Signature>>);
  static_assert(std::is_nothrow_move_constructible_v<Function<Signature>>);
  // The standard doesn't require std::move_only_function's move assignment to be noexcept (MSVC's isn't), only check
  // the fallback.
  static_assert(std::is_nothrow_move_assignable_v<folly::Function<Signature>>);

  if constexpr (expected)
  {
    Function<Signature> f = Target{};
    ASSERT_TRUE(f);
    (void)f();

    Function<Signature> moved = std::move(f);
    ASSERT_TRUE(moved);
    (void)moved();

    f = std::move(moved);
    ASSERT_TRUE(f);
    (void)f();

    Function<Signature> empty;
    f.swap(empty);
    EXPECT_FALSE(f);
    ASSERT_TRUE(empty);
    (void)empty();

    using std::swap;
    swap(f, empty);
    ASSERT_TRUE(f);
    EXPECT_FALSE(empty);
    (void)f();

    f = nullptr;
    EXPECT_FALSE(f);
  }
}

template<template<typename> class Function, typename Signature, typename... Targets>
void TestConstructionSignature(TypeList<Targets...>)
{
  (TestConstructionCombination<Function, Signature, Targets>(), ...);
}

template<template<typename> class Function, typename... Signatures>
void TestConstructionMatrix(TypeList<Signatures...>)
{
  (TestConstructionSignature<Function, Signatures>(ConstructionTargets{}), ...);
  TestConstructionCombination<Function, int&(), ReturnsReference>();
  TestConstructionCombination<Function, int&() noexcept, ReturnsReference>();
  TestConstructionCombination<Function, const int&() const, ReturnsReference>();
  TestConstructionCombination<Function, const int&() const noexcept, ReturnsReference>();
}

template<template<typename> class Function, typename To, typename From>
void TestConversionCombination()
{
  constexpr bool expected = std::is_constructible_v<std::move_only_function<To>, std::move_only_function<From>&&>;
  static_assert(std::is_constructible_v<Function<To>, Function<From>&&> == expected);
  static_assert(std::is_assignable_v<Function<To>&, Function<From>&&> ==
                std::is_assignable_v<std::move_only_function<To>&, std::move_only_function<From>&&>);

  if constexpr (expected)
  {
    const auto invoke = [](Function<To>& f) {
      ASSERT_TRUE(f);
      if constexpr (std::is_void_v<std::invoke_result_t<Function<To>&>>)
        f();
      else
        EXPECT_EQ(f(), 1);
    };

    Function<From> source = NothrowReturnsInt{};
    Function<To> converted = std::move(source);
    invoke(converted);

    source = NothrowReturnsInt{};
    converted = std::move(source);
    invoke(converted);

    Function<From> empty;
    converted = std::move(empty);
    EXPECT_FALSE(converted);
  }
}

template<template<typename> class Function, typename To, typename... From>
void TestConversionSignature(TypeList<From...>)
{
  (TestConversionCombination<Function, To, From>(), ...);
}

template<template<typename> class Function, typename... To>
void TestConversionMatrix(TypeList<To...> signatures)
{
  (TestConversionSignature<Function, To>(signatures), ...);
}

#endif

// Counts live instances, to check that moves don't leak or double-destroy the target.
template<size_t Size, size_t Align = alignof(void*)>
struct alignas(Align) Counted
{
  static inline int live = 0;

  Counted(int value_) : value(value_) { live++; }
  Counted(Counted&& other) noexcept : value(other.value) { live++; }
  ~Counted() { live--; }

  int operator()() const { return value; }

  int value;
  char padding[Size];
};

template<template<typename> class Function, typename T>
void TestCountedLifetime()
{
  ASSERT_EQ(T::live, 0);
  {
    Function<int() const> f = T(5);
    EXPECT_EQ(T::live, 1);
    EXPECT_EQ(f(), 5);

    Function<int() const> g = std::move(f);
    EXPECT_EQ(T::live, 1);
    EXPECT_EQ(g(), 5);

    f = std::move(g);
    EXPECT_EQ(T::live, 1);
    EXPECT_EQ(f(), 5);

    f = nullptr;
    EXPECT_FALSE(f);
    EXPECT_EQ(T::live, 0);

    g = T(6);
    EXPECT_EQ(T::live, 1);
    EXPECT_EQ(g(), 6);
  }
  EXPECT_EQ(T::live, 0);
}

} // namespace

// Runs each test against both MoveOnlyFunction and folly::Function.
#define FUNCTION_TEST(name)                                                                                            \
  template<template<typename> class Function>                                                                          \
  static void Test##name();                                                                                            \
  TEST(MoveOnlyFunction, name)                                                                                         \
  {                                                                                                                    \
    Test##name<MoveOnlyFunction>();                                                                                    \
  }                                                                                                                    \
  TEST(FollyFunction, name)                                                                                            \
  {                                                                                                                    \
    Test##name<folly::Function>();                                                                                     \
  }                                                                                                                    \
  template<template<typename> class Function>                                                                          \
  static void Test##name()

FUNCTION_TEST(Empty)
{
  Function<void()> f;
  EXPECT_FALSE(f);
  f = [] {};
  EXPECT_TRUE(f);
  f = nullptr;
  EXPECT_FALSE(f);
}

FUNCTION_TEST(MoveOnlyCapture)
{
  Function<int()> f = [p = std::make_unique<int>(5)] { return *p; };
  EXPECT_EQ(f(), 5);

  Function<int()> g = std::move(f);
  EXPECT_EQ(g(), 5);
}

FUNCTION_TEST(ConstAndNoexcept)
{
  const Function<int(int) const noexcept> f = [](int value) noexcept { return value * 2; };
  static_assert(std::is_nothrow_invocable_r_v<int, const decltype(f)&, int>);
  EXPECT_EQ(f(3), 6);

  Function<int() noexcept> g = NothrowCallNothrowConversion{};
  static_assert(noexcept(g()));
  EXPECT_EQ(g(), 1);
}

FUNCTION_TEST(FunctionAndMemberPointers)
{
  Function<int(int) noexcept> f = +[](int value) noexcept { return value * 2; };
  EXPECT_EQ(f(3), 6);

  int (*null_function)(int) noexcept = nullptr;
  Function<int(int) noexcept> empty = null_function;
  EXPECT_FALSE(empty);
  EXPECT_TRUE(empty == nullptr);
  EXPECT_TRUE(nullptr == empty);
  EXPECT_FALSE(empty != nullptr);
  EXPECT_FALSE(nullptr != empty);

  struct Object
  {
    int Add(int delta) const noexcept { return value + delta; }

    int value = 5;
  } object;

  Function<int(const Object&, int) noexcept> method = &Object::Add;
  EXPECT_EQ(method(object, 2), 7);

  Function<const int&(const Object&)> member = &Object::value;
  EXPECT_EQ(&member(object), &object.value);
  object.value = 9;
  EXPECT_EQ(member(object), 9);

  int (Object::*null_method)(int) const noexcept = nullptr;
  Function<int(const Object&, int) noexcept> empty_method = null_method;
  EXPECT_FALSE(empty_method);

  int Object::* null_member = nullptr;
  Function<const int&(const Object&)> empty_member = null_member;
  EXPECT_FALSE(empty_member);
}

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

FUNCTION_TEST(ConstructorCompatibilityMatrix)
{
  TestConstructionMatrix<Function>(ConstructionSignatures{});
}

FUNCTION_TEST(WrapperConversionMatrix)
{
  TestConversionMatrix<Function>(ConversionSignatures{});
}

#endif

FUNCTION_TEST(Arguments)
{
  Function<long(int)> f = [](int x) { return x * 2; };
  EXPECT_EQ(f(2), 4);

  Function<std::string(std::string, const std::string&)> g = [](std::string a, const std::string& b) { return a + b; };
  EXPECT_EQ(g("a", "b"), "ab");

  Function<void(std::unique_ptr<int>&&)> h = [](std::unique_ptr<int>&& p) { *p = 7; };
  auto p = std::make_unique<int>(0);
  h(std::move(p));
  ASSERT_TRUE(p);
  EXPECT_EQ(*p, 7);
}

FUNCTION_TEST(Mutable)
{
  Function<int()> f = [i = 0]() mutable { return ++i; };
  EXPECT_EQ(f(), 1);
  EXPECT_EQ(f(), 2);
}

FUNCTION_TEST(ResultConversion)
{
  Function<std::unique_ptr<Base>()> f = ReturnsDerivedPtr();
  EXPECT_TRUE(f());

  Function<void()> g = ReturnsInt();
  g();

  Function<Immovable()> h = ReturnsImmovable();
  Immovable a = h();
  EXPECT_EQ(a.value, 3);

  Function<Immovable() const noexcept> i = NothrowReturnsImmovable();
  Immovable b = i();
  EXPECT_EQ(b.value, 3);

  const Function<Immovable() const> const_result = ReturnsImmovable();
  Immovable c = const_result();
  EXPECT_EQ(c.value, 3);

  Function<Immovable() noexcept> nothrow_result = NothrowReturnsImmovable();
  Immovable d = nothrow_result();
  EXPECT_EQ(d.value, 3);

  Function<void() const> j = ConstLvalueOnly();
  j();
}

FUNCTION_TEST(SmallLifetime)
{
  TestCountedLifetime<Function, Counted<1>>();
}

FUNCTION_TEST(LargeLifetime)
{
  TestCountedLifetime<Function, Counted<256>>();
}

FUNCTION_TEST(OverAlignedLifetime)
{
  TestCountedLifetime<Function, Counted<1, 64>>();
  TestCountedLifetime<Function, Counted<256, 64>>();
}

FUNCTION_TEST(LargeTrivial)
{
  struct Large
  {
    char data[256];
  } large = {{3}};

  Function<int()> f = [large] { return large.data[0]; };
  Function<int()> g = std::move(f);
  EXPECT_EQ(g(), 3);

  struct alignas(64) LargeAligned
  {
    char data[256];
    int operator()() const { return data[0]; }
  };

  Function<int() const> h = LargeAligned{{7}};
  Function<int() const> i = std::move(h);
  EXPECT_EQ(i(), 7);
}

FUNCTION_TEST(SmallOverAlignedTrivial)
{
  struct alignas(128) Aligned
  {
    int operator()() const noexcept { return value; }

    int value;
  };
  static_assert(std::is_trivially_copyable_v<Aligned>);

  Function<int() const noexcept> f = Aligned{11};
  Function<int() const noexcept> g = std::move(f);
  EXPECT_EQ(g(), 11);
  f = std::move(g);
  EXPECT_EQ(f(), 11);
  f = nullptr;
  EXPECT_FALSE(f);
}

#if FOLLY_HAS_EXCEPTIONS

FUNCTION_TEST(WrapperMoveDoesNotCallThrowingTargetMove)
{
  struct Target
  {
    explicit Target(bool* flag) : throw_on_move(flag) {}
    Target(Target&& other) : throw_on_move(other.throw_on_move)
    {
      if (*throw_on_move)
        throw 1;
    }
    void operator()() const {}

    bool* throw_on_move;
  };

  bool throw_on_move = false;
  Function<void()> f = Target(&throw_on_move);
  throw_on_move = true;
  EXPECT_NO_THROW({
    Function<void()> g = std::move(f);
    g();
    f = std::move(g);
    f();
  });
}

#endif

#undef FUNCTION_TEST

namespace {

struct ThrowingMove
{
  ThrowingMove() = default;
  ThrowingMove(ThrowingMove&&) {}
  void operator()() {}
  int x = 1;
};

struct NothrowMove
{
  NothrowMove() = default;
  NothrowMove(NothrowMove&&) noexcept {}
  void operator()() {}
  int x = 1;
};

template<typename F>
constexpr bool IsInSitu = folly::detail::function::DispatchOf<F>::is_in_situ;

// Moves of in-situ targets happen in noexcept functions, so a throwing move must go on the heap. Without exceptions,
// nothing can throw, so it should stay in situ.
static_assert(IsInSitu<ThrowingMove> == !FOLLY_HAS_EXCEPTIONS);
static_assert(IsInSitu<NothrowMove>);
static_assert(IsInSitu<Counted<1>>);
static_assert(!IsInSitu<Counted<256>>);
static_assert(!IsInSitu<Counted<1, 64>>);

} // namespace

TEST(FollyFunction, ThrowingMove)
{
  folly::Function<void()> f = ThrowingMove();
  folly::Function<void()> g = std::move(f);
  g();
}

#endif // MOVE_ONLY_FUNCTION_TEST_DIAGNOSTIC_SCOPE
