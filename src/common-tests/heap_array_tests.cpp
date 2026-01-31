// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/heap_array.h"

#include <gtest/gtest.h>

// ============================================================================
// FixedHeapArray Tests
// ============================================================================

TEST(FixedHeapArray, DefaultConstruction)
{
  FixedHeapArray<int, 10> arr;
  EXPECT_EQ(arr.size(), 10u);
  EXPECT_EQ(arr.capacity(), 10u);
  EXPECT_EQ(arr.size_bytes(), 10u * sizeof(int));
  EXPECT_FALSE(arr.empty());
  EXPECT_NE(arr.data(), nullptr);
}

TEST(FixedHeapArray, CopyConstruction)
{
  FixedHeapArray<int, 5> arr1;
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 10);

  FixedHeapArray<int, 5> arr2(arr1);
  EXPECT_EQ(arr2.size(), arr1.size());
  for (size_t i = 0; i < arr1.size(); ++i)
    EXPECT_EQ(arr2[i], arr1[i]);

  // Ensure they have separate storage
  EXPECT_NE(arr1.data(), arr2.data());
}

TEST(FixedHeapArray, MoveConstruction)
{
  FixedHeapArray<int, 5> arr1;
  int* original_data = arr1.data();
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 10);

  FixedHeapArray<int, 5> arr2(std::move(arr1));
  EXPECT_EQ(arr2.data(), original_data);
  EXPECT_EQ(arr2.size(), 5u);

  for (size_t i = 0; i < arr2.size(); ++i)
    EXPECT_EQ(arr2[i], static_cast<int>(i * 10));
}

TEST(FixedHeapArray, ElementAccess)
{
  FixedHeapArray<int, 5> arr;
  arr[0] = 100;
  arr[1] = 200;
  arr[2] = 300;
  arr[3] = 400;
  arr[4] = 500;

  EXPECT_EQ(arr[0], 100);
  EXPECT_EQ(arr[1], 200);
  EXPECT_EQ(arr[2], 300);
  EXPECT_EQ(arr[3], 400);
  EXPECT_EQ(arr[4], 500);

  const auto& carr = arr;
  EXPECT_EQ(carr[0], 100);
  EXPECT_EQ(carr[4], 500);
}

TEST(FixedHeapArray, FrontBack)
{
  FixedHeapArray<int, 5> arr;
  arr[0] = 10;
  arr[4] = 50;

  EXPECT_EQ(arr.front(), 10);
  EXPECT_EQ(arr.back(), 50);

  arr.front() = 15;
  arr.back() = 55;
  EXPECT_EQ(arr[0], 15);
  EXPECT_EQ(arr[4], 55);

  const auto& carr = arr;
  EXPECT_EQ(carr.front(), 15);
  EXPECT_EQ(carr.back(), 55);
}

TEST(FixedHeapArray, Iterators)
{
  FixedHeapArray<int, 5> arr;
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  int expected = 0;
  for (auto it = arr.begin(); it != arr.end(); ++it)
  {
    EXPECT_EQ(*it, expected);
    ++expected;
  }
  EXPECT_EQ(expected, 5);

  expected = 0;
  for (auto it = arr.cbegin(); it != arr.cend(); ++it)
  {
    EXPECT_EQ(*it, expected);
    ++expected;
  }
}

TEST(FixedHeapArray, Fill)
{
  FixedHeapArray<int, 10> arr;
  arr.fill(42);

  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], 42);
}

TEST(FixedHeapArray, Swap)
{
  FixedHeapArray<int, 5> arr1;
  FixedHeapArray<int, 5> arr2;
  arr1.fill(10);
  arr2.fill(20);

  int* arr1_data = arr1.data();
  int* arr2_data = arr2.data();

  arr1.swap(arr2);

  EXPECT_EQ(arr1.data(), arr2_data);
  EXPECT_EQ(arr2.data(), arr1_data);

  for (size_t i = 0; i < arr1.size(); ++i)
  {
    EXPECT_EQ(arr1[i], 20);
    EXPECT_EQ(arr2[i], 10);
  }
}

TEST(FixedHeapArray, Span)
{
  FixedHeapArray<int, 5> arr;
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i * 2);

  std::span<int, 5> sp = arr.span();
  EXPECT_EQ(sp.size(), 5u);
  EXPECT_EQ(sp.data(), arr.data());

  for (size_t i = 0; i < sp.size(); ++i)
    EXPECT_EQ(sp[i], static_cast<int>(i * 2));

  const auto& carr = arr;
  std::span<const int, 5> csp = carr.cspan();
  EXPECT_EQ(csp.size(), 5u);
  EXPECT_EQ(csp.data(), carr.data());
}

TEST(FixedHeapArray, MoveAssignment)
{
  FixedHeapArray<int, 5> arr1;
  arr1.fill(42);
  int* original_data = arr1.data();

  FixedHeapArray<int, 5> arr2;
  arr2.fill(0);

  arr2 = std::move(arr1);
  EXPECT_EQ(arr2.data(), original_data);
  for (size_t i = 0; i < arr2.size(); ++i)
    EXPECT_EQ(arr2[i], 42);
}

TEST(FixedHeapArray, AlignedAllocation)
{
  constexpr size_t alignment = 64;
  FixedHeapArray<int, 16, alignment> arr;

  uintptr_t addr = reinterpret_cast<uintptr_t>(arr.data());
  EXPECT_EQ(addr % alignment, 0u);
}

TEST(FixedHeapArray, DifferentTypes)
{
  FixedHeapArray<double, 3> arr;
  arr[0] = 1.5;
  arr[1] = 2.5;
  arr[2] = 3.5;

  EXPECT_DOUBLE_EQ(arr[0], 1.5);
  EXPECT_DOUBLE_EQ(arr[1], 2.5);
  EXPECT_DOUBLE_EQ(arr[2], 3.5);
  EXPECT_EQ(arr.size_bytes(), 3u * sizeof(double));
}

// ============================================================================
// DynamicHeapArray Tests
// ============================================================================

TEST(DynamicHeapArray, DefaultConstruction)
{
  DynamicHeapArray<int> arr;
  EXPECT_EQ(arr.size(), 0u);
  EXPECT_EQ(arr.capacity(), 0u);
  EXPECT_EQ(arr.size_bytes(), 0u);
  EXPECT_TRUE(arr.empty());
  EXPECT_EQ(arr.data(), nullptr);
}

TEST(DynamicHeapArray, SizeConstruction)
{
  DynamicHeapArray<int> arr(10);
  EXPECT_EQ(arr.size(), 10u);
  EXPECT_EQ(arr.capacity(), 10u);
  EXPECT_EQ(arr.size_bytes(), 10u * sizeof(int));
  EXPECT_FALSE(arr.empty());
  EXPECT_NE(arr.data(), nullptr);
}

TEST(DynamicHeapArray, RangeConstructionBeginEnd)
{
  int source[] = {1, 2, 3, 4, 5};
  DynamicHeapArray<int> arr(std::begin(source), std::end(source));

  EXPECT_EQ(arr.size(), 5u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, RangeConstructionBeginCount)
{
  int source[] = {10, 20, 30, 40, 50};
  DynamicHeapArray<int> arr(source, 5);

  EXPECT_EQ(arr.size(), 5u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, SpanConstruction)
{
  int source[] = {1, 2, 3, 4, 5};
  std::span<const int> sp(source);
  DynamicHeapArray<int> arr(sp);

  EXPECT_EQ(arr.size(), 5u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, SpanConstructionEmpty)
{
  std::span<const int> sp;
  DynamicHeapArray<int> arr(sp);

  EXPECT_EQ(arr.size(), 0u);
  EXPECT_TRUE(arr.empty());
}

TEST(DynamicHeapArray, CopyConstruction)
{
  DynamicHeapArray<int> arr1(5);
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 10);

  DynamicHeapArray<int> arr2(arr1);
  EXPECT_EQ(arr2.size(), arr1.size());
  for (size_t i = 0; i < arr1.size(); ++i)
    EXPECT_EQ(arr2[i], arr1[i]);

  // Ensure they have separate storage
  EXPECT_NE(arr1.data(), arr2.data());
}

TEST(DynamicHeapArray, CopyConstructionEmpty)
{
  DynamicHeapArray<int> arr1;
  DynamicHeapArray<int> arr2(arr1);

  EXPECT_EQ(arr2.size(), 0u);
  EXPECT_TRUE(arr2.empty());
}

TEST(DynamicHeapArray, MoveConstruction)
{
  DynamicHeapArray<int> arr1(5);
  int* original_data = arr1.data();
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 10);

  DynamicHeapArray<int> arr2(std::move(arr1));

  EXPECT_EQ(arr2.data(), original_data);
  EXPECT_EQ(arr2.size(), 5u);
  EXPECT_EQ(arr1.size(), 0u);
  EXPECT_EQ(arr1.data(), nullptr);

  for (size_t i = 0; i < arr2.size(); ++i)
    EXPECT_EQ(arr2[i], static_cast<int>(i * 10));
}

TEST(DynamicHeapArray, ElementAccess)
{
  DynamicHeapArray<int> arr(5);
  arr[0] = 100;
  arr[1] = 200;
  arr[2] = 300;
  arr[3] = 400;
  arr[4] = 500;

  EXPECT_EQ(arr[0], 100);
  EXPECT_EQ(arr[1], 200);
  EXPECT_EQ(arr[2], 300);
  EXPECT_EQ(arr[3], 400);
  EXPECT_EQ(arr[4], 500);

  const auto& carr = arr;
  EXPECT_EQ(carr[0], 100);
  EXPECT_EQ(carr[4], 500);
}

TEST(DynamicHeapArray, FrontBack)
{
  DynamicHeapArray<int> arr(5);
  arr[0] = 10;
  arr[4] = 50;

  EXPECT_EQ(arr.front(), 10);
  EXPECT_EQ(arr.back(), 50);

  arr.front() = 15;
  arr.back() = 55;
  EXPECT_EQ(arr[0], 15);
  EXPECT_EQ(arr[4], 55);

  const auto& carr = arr;
  EXPECT_EQ(carr.front(), 15);
  EXPECT_EQ(carr.back(), 55);
}

TEST(DynamicHeapArray, Iterators)
{
  DynamicHeapArray<int> arr(5);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  int expected = 0;
  for (auto it = arr.begin(); it != arr.end(); ++it)
  {
    EXPECT_EQ(*it, expected);
    ++expected;
  }
  EXPECT_EQ(expected, 5);

  expected = 0;
  for (auto it = arr.cbegin(); it != arr.cend(); ++it)
  {
    EXPECT_EQ(*it, expected);
    ++expected;
  }
}

TEST(DynamicHeapArray, Fill)
{
  DynamicHeapArray<int> arr(10);
  arr.fill(42);

  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], 42);
}

TEST(DynamicHeapArray, Swap)
{
  DynamicHeapArray<int> arr1(5);
  DynamicHeapArray<int> arr2(3);
  arr1.fill(10);
  arr2.fill(20);

  int* arr1_data = arr1.data();
  int* arr2_data = arr2.data();

  arr1.swap(arr2);

  EXPECT_EQ(arr1.data(), arr2_data);
  EXPECT_EQ(arr2.data(), arr1_data);
  EXPECT_EQ(arr1.size(), 3u);
  EXPECT_EQ(arr2.size(), 5u);

  for (size_t i = 0; i < arr1.size(); ++i)
    EXPECT_EQ(arr1[i], 20);
  for (size_t i = 0; i < arr2.size(); ++i)
    EXPECT_EQ(arr2[i], 10);
}

TEST(DynamicHeapArray, ResizeGrow)
{
  DynamicHeapArray<int> arr(5);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  arr.resize(10);
  EXPECT_EQ(arr.size(), 10u);

  // Original data should be preserved
  for (size_t i = 0; i < 5; ++i)
    EXPECT_EQ(arr[i], static_cast<int>(i));
}

TEST(DynamicHeapArray, ResizeShrink)
{
  DynamicHeapArray<int> arr(10);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  arr.resize(5);
  EXPECT_EQ(arr.size(), 5u);

  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], static_cast<int>(i));
}

TEST(DynamicHeapArray, ResizeSameSize)
{
  DynamicHeapArray<int> arr(5);
  int* original_data = arr.data();
  arr.fill(42);

  arr.resize(5);
  EXPECT_EQ(arr.size(), 5u);
  EXPECT_EQ(arr.data(), original_data);

  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], 42);
}

TEST(DynamicHeapArray, Deallocate)
{
  DynamicHeapArray<int> arr(10);
  EXPECT_FALSE(arr.empty());

  arr.deallocate();
  EXPECT_TRUE(arr.empty());
  EXPECT_EQ(arr.size(), 0u);
  EXPECT_EQ(arr.data(), nullptr);
}

TEST(DynamicHeapArray, AssignSpan)
{
  DynamicHeapArray<int> arr;
  int source[] = {1, 2, 3, 4, 5};
  std::span<const int> sp(source);

  arr.assign(sp);
  EXPECT_EQ(arr.size(), 5u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, AssignBeginEnd)
{
  DynamicHeapArray<int> arr;
  int source[] = {10, 20, 30};

  arr.assign(std::begin(source), std::end(source));
  EXPECT_EQ(arr.size(), 3u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, AssignBeginCount)
{
  DynamicHeapArray<int> arr;
  int source[] = {100, 200, 300, 400};

  arr.assign(source, 4);
  EXPECT_EQ(arr.size(), 4u);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, AssignCopy)
{
  DynamicHeapArray<int> arr1(5);
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 3);

  DynamicHeapArray<int> arr2;
  arr2.assign(arr1);

  EXPECT_EQ(arr2.size(), arr1.size());
  for (size_t i = 0; i < arr1.size(); ++i)
    EXPECT_EQ(arr2[i], arr1[i]);
  EXPECT_NE(arr1.data(), arr2.data());
}

TEST(DynamicHeapArray, AssignMove)
{
  DynamicHeapArray<int> arr1(5);
  int* original_data = arr1.data();
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 3);

  DynamicHeapArray<int> arr2;
  arr2.assign(std::move(arr1));

  EXPECT_EQ(arr2.size(), 5u);
  EXPECT_EQ(arr2.data(), original_data);
  EXPECT_EQ(arr1.size(), 0u);
  EXPECT_EQ(arr1.data(), nullptr);
}

TEST(DynamicHeapArray, AssignEmpty)
{
  DynamicHeapArray<int> arr(10);
  arr.fill(42);

  int* empty = nullptr;
  arr.assign(empty, static_cast<size_t>(0));

  EXPECT_TRUE(arr.empty());
  EXPECT_EQ(arr.size(), 0u);
}

TEST(DynamicHeapArray, AssignSameSize)
{
  DynamicHeapArray<int> arr(5);
  arr.fill(10);
  int* original_data = arr.data();

  int source[] = {1, 2, 3, 4, 5};
  arr.assign(source, 5);

  // Should reuse existing buffer
  EXPECT_EQ(arr.data(), original_data);
  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], source[i]);
}

TEST(DynamicHeapArray, Span)
{
  DynamicHeapArray<int> arr(5);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i * 2);

  std::span<int> sp = arr.span();
  EXPECT_EQ(sp.size(), 5u);
  EXPECT_EQ(sp.data(), arr.data());

  for (size_t i = 0; i < sp.size(); ++i)
    EXPECT_EQ(sp[i], static_cast<int>(i * 2));
}

TEST(DynamicHeapArray, SpanWithOffset)
{
  DynamicHeapArray<int> arr(10);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  std::span<int> sp = arr.span(3, 4);
  EXPECT_EQ(sp.size(), 4u);
  EXPECT_EQ(sp.data(), arr.data() + 3);
  EXPECT_EQ(sp[0], 3);
  EXPECT_EQ(sp[3], 6);
}

TEST(DynamicHeapArray, SpanWithOffsetClamp)
{
  DynamicHeapArray<int> arr(10);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  // Request more than available
  std::span<int> sp = arr.span(7, 100);
  EXPECT_EQ(sp.size(), 3u);
  EXPECT_EQ(sp[0], 7);
  EXPECT_EQ(sp[2], 9);
}

TEST(DynamicHeapArray, SpanWithOffsetOutOfRange)
{
  DynamicHeapArray<int> arr(5);
  std::span<int> sp = arr.span(10);
  EXPECT_TRUE(sp.empty());
}

TEST(DynamicHeapArray, CSpan)
{
  DynamicHeapArray<int> arr(5);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i * 2);

  const auto& carr = arr;
  std::span<const int> csp = carr.cspan();
  EXPECT_EQ(csp.size(), 5u);
  EXPECT_EQ(csp.data(), carr.data());
}

TEST(DynamicHeapArray, CSpanWithOffset)
{
  DynamicHeapArray<int> arr(10);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<int>(i);

  const auto& carr = arr;
  std::span<const int> csp = carr.cspan(2, 3);
  EXPECT_EQ(csp.size(), 3u);
  EXPECT_EQ(csp[0], 2);
  EXPECT_EQ(csp[2], 4);
}

TEST(DynamicHeapArray, CopyAssignment)
{
  DynamicHeapArray<int> arr1(5);
  for (size_t i = 0; i < arr1.size(); ++i)
    arr1[i] = static_cast<int>(i * 2);

  DynamicHeapArray<int> arr2;
  arr2 = arr1;

  EXPECT_EQ(arr2.size(), arr1.size());
  EXPECT_NE(arr2.data(), arr1.data());
  for (size_t i = 0; i < arr1.size(); ++i)
    EXPECT_EQ(arr2[i], arr1[i]);
}

TEST(DynamicHeapArray, MoveAssignment)
{
  DynamicHeapArray<int> arr1(5);
  int* original_data = arr1.data();
  arr1.fill(42);

  DynamicHeapArray<int> arr2;
  arr2 = std::move(arr1);

  EXPECT_EQ(arr2.data(), original_data);
  EXPECT_EQ(arr2.size(), 5u);
  EXPECT_EQ(arr1.size(), 0u);
  EXPECT_EQ(arr1.data(), nullptr);

  for (size_t i = 0; i < arr2.size(); ++i)
    EXPECT_EQ(arr2[i], 42);
}

TEST(DynamicHeapArray, AlignedAllocation)
{
  constexpr size_t alignment = 64;
  DynamicHeapArray<int, alignment> arr(16);

  uintptr_t addr = reinterpret_cast<uintptr_t>(arr.data());
  EXPECT_EQ(addr % alignment, 0u);
}

TEST(DynamicHeapArray, AlignedResize)
{
  constexpr size_t alignment = 64;
  DynamicHeapArray<int, alignment> arr(8);
  arr.fill(42);

  arr.resize(32);

  uintptr_t addr = reinterpret_cast<uintptr_t>(arr.data());
  EXPECT_EQ(addr % alignment, 0u);

  // Original data preserved
  for (size_t i = 0; i < 8; ++i)
    EXPECT_EQ(arr[i], 42);
}

TEST(DynamicHeapArray, DifferentTypes)
{
  DynamicHeapArray<double> arr(3);
  arr[0] = 1.5;
  arr[1] = 2.5;
  arr[2] = 3.5;

  EXPECT_DOUBLE_EQ(arr[0], 1.5);
  EXPECT_DOUBLE_EQ(arr[1], 2.5);
  EXPECT_DOUBLE_EQ(arr[2], 3.5);
  EXPECT_EQ(arr.size_bytes(), 3u * sizeof(double));
}

TEST(DynamicHeapArray, RangeBasedFor)
{
  DynamicHeapArray<int> arr(5);
  int val = 0;
  for (auto& elem : arr)
    elem = val++;

  val = 0;
  for (const auto& elem : arr)
  {
    EXPECT_EQ(elem, val);
    ++val;
  }
}

TEST(DynamicHeapArray, ByteArray)
{
  DynamicHeapArray<std::byte> arr(16);
  for (size_t i = 0; i < arr.size(); ++i)
    arr[i] = static_cast<std::byte>(i);

  EXPECT_EQ(arr.size(), 16u);
  EXPECT_EQ(arr.size_bytes(), 16u);

  for (size_t i = 0; i < arr.size(); ++i)
    EXPECT_EQ(arr[i], static_cast<std::byte>(i));
}

TEST(DynamicHeapArray, EmptyRangeConstruction)
{
  int* empty = nullptr;
  DynamicHeapArray<int> arr(empty, empty);

  EXPECT_TRUE(arr.empty());
  EXPECT_EQ(arr.size(), 0u);
  EXPECT_EQ(arr.data(), nullptr);
}

TEST(DynamicHeapArray, EmptyCountConstruction)
{
  int* ptr = nullptr;
  DynamicHeapArray<int> arr(ptr, static_cast<size_t>(0));

  EXPECT_TRUE(arr.empty());
  EXPECT_EQ(arr.size(), 0u);
  EXPECT_EQ(arr.data(), nullptr);
}

TEST(FixedHeapArray, CopyAssignmentCopiesWrongDirection)
{
  FixedHeapArray<int, 4> src;
  FixedHeapArray<int, 4> dst;

  src[0] = 1;
  src[1] = 2;
  src[2] = 3;
  src[3] = 4;
  dst[0] = 0;
  dst[1] = 0;
  dst[2] = 0;
  dst[3] = 0;

  dst = src; // Should copy src -> dst

  // After assignment, dst should have src's values
  EXPECT_EQ(dst[0], 1);
  EXPECT_EQ(dst[1], 2);
  EXPECT_EQ(dst[2], 3);
  EXPECT_EQ(dst[3], 4);
}

TEST(FixedHeapArray, EqualityOperatorMissingReturnTrue)
{
  FixedHeapArray<int, 4> a;
  FixedHeapArray<int, 4> b;

  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  b[0] = 1;
  b[1] = 2;
  b[2] = 3;
  b[3] = 4;

  // Both arrays have identical content, should return true
  EXPECT_TRUE(a == b);
}

TEST(FixedHeapArray, InequalityOperatorReturnTrue)
{
  FixedHeapArray<int, 4> a;
  FixedHeapArray<int, 4> b;

  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  b[0] = 1;
  b[1] = 2;
  b[2] = 3;
  b[3] = 5; // Last element differs

  // Arrays differ, inequality should return true
  EXPECT_TRUE(a != b);
}

TEST(DynamicHeapArray, EqualityOperatorReturnTrue)
{
  DynamicHeapArray<int> a(4);
  DynamicHeapArray<int> b(4);

  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  b[0] = 1;
  b[1] = 2;
  b[2] = 3;
  b[3] = 4;

  // Both arrays have identical content, should return true
  EXPECT_TRUE(a == b);
}

TEST(DynamicHeapArray, EqualityOperatorWhenSizesDiffer)
{
  DynamicHeapArray<int> a(4);
  DynamicHeapArray<int> b(8);

  // Different sizes should NOT be equal
  EXPECT_FALSE(a == b);
}

TEST(DynamicHeapArray, InequalityOperatorWhenSizesDiffer)
{
  DynamicHeapArray<int> a(4);
  DynamicHeapArray<int> b(8);

  // Different sizes should be not-equal
  EXPECT_TRUE(a != b);
}
