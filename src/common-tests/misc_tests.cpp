// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/locked_ptr.h"
#include "common/optional_with_status.h"

#include <gtest/gtest.h>

#include <mutex>
#include <optional>
#include <string>

// ============================================================================
// OptionalWithStatus Tests
// ============================================================================

enum class TestStatus
{
  Ok,
  Missing,
  Failed,
};

TEST(OptionalWithStatus, DefaultConstruction)
{
  OptionalWithStatus<int, TestStatus> obj;
  EXPECT_FALSE(obj.has_value());
  EXPECT_FALSE(static_cast<bool>(obj));
  EXPECT_EQ(obj.status(), TestStatus::Ok);
}

TEST(OptionalWithStatus, StatusOnlyConstruction)
{
  OptionalWithStatus<int, TestStatus> obj(TestStatus::Missing);
  EXPECT_FALSE(obj.has_value());
  EXPECT_FALSE(static_cast<bool>(obj));
  EXPECT_EQ(obj.status(), TestStatus::Missing);
}

TEST(OptionalWithStatus, StatusOnlyConstructionError)
{
  OptionalWithStatus<int, TestStatus> obj(TestStatus::Failed);
  EXPECT_FALSE(obj.has_value());
  EXPECT_EQ(obj.status(), TestStatus::Failed);
}

TEST(OptionalWithStatus, CopyValueConstruction)
{
  const int value = 42;
  OptionalWithStatus<int, TestStatus> obj(TestStatus::Ok, value);
  EXPECT_TRUE(obj.has_value());
  EXPECT_TRUE(static_cast<bool>(obj));
  EXPECT_EQ(obj.status(), TestStatus::Ok);
  EXPECT_EQ(obj.value(), 42);
  EXPECT_EQ(*obj, 42);
}

TEST(OptionalWithStatus, MoveValueConstruction)
{
  std::string s = "hello";
  OptionalWithStatus<std::string, TestStatus> obj(TestStatus::Ok, std::move(s));
  EXPECT_TRUE(obj.has_value());
  EXPECT_EQ(obj.status(), TestStatus::Ok);
  EXPECT_EQ(obj.value(), "hello");
}

TEST(OptionalWithStatus, ArrowOperator)
{
  OptionalWithStatus<std::string, TestStatus> obj(TestStatus::Ok, std::string("world"));
  EXPECT_EQ(obj->size(), 5u);
}

TEST(OptionalWithStatus, ValueOr)
{
  OptionalWithStatus<int, TestStatus> present(TestStatus::Ok, 10);
  OptionalWithStatus<int, TestStatus> absent(TestStatus::Missing);

  EXPECT_EQ(present.value_or(99), 10);
  EXPECT_EQ(absent.value_or(99), 99);
}

TEST(OptionalWithStatus, CopyConstruction)
{
  OptionalWithStatus<int, TestStatus> original(TestStatus::Ok, 7);
  OptionalWithStatus<int, TestStatus> copy(original);
  EXPECT_TRUE(copy.has_value());
  EXPECT_EQ(copy.status(), TestStatus::Ok);
  EXPECT_EQ(copy.value(), 7);
}

TEST(OptionalWithStatus, MoveConstruction)
{
  OptionalWithStatus<std::string, TestStatus> original(TestStatus::Ok, std::string("move-me"));
  OptionalWithStatus<std::string, TestStatus> moved(std::move(original));
  EXPECT_TRUE(moved.has_value());
  EXPECT_EQ(moved.status(), TestStatus::Ok);
  EXPECT_EQ(moved.value(), "move-me");
  // source must be emptied by the manual move
  EXPECT_FALSE(original.has_value());
}

TEST(OptionalWithStatus, CopyAssignment)
{
  OptionalWithStatus<int, TestStatus> a(TestStatus::Ok, 5);
  OptionalWithStatus<int, TestStatus> b(TestStatus::Missing);
  b = a;
  EXPECT_TRUE(b.has_value());
  EXPECT_EQ(b.status(), TestStatus::Ok);
  EXPECT_EQ(b.value(), 5);
}

TEST(OptionalWithStatus, MoveAssignment)
{
  OptionalWithStatus<std::string, TestStatus> a(TestStatus::Ok, std::string("assigned"));
  OptionalWithStatus<std::string, TestStatus> b(TestStatus::Missing);
  b = std::move(a);
  EXPECT_TRUE(b.has_value());
  EXPECT_EQ(b.status(), TestStatus::Ok);
  EXPECT_EQ(b.value(), "assigned");
  // source must be emptied by the manual move
  EXPECT_FALSE(a.has_value());
}

TEST(OptionalWithStatus, MutableValueAccess)
{
  OptionalWithStatus<int, TestStatus> obj(TestStatus::Ok, 1);
  obj.value() = 100;
  EXPECT_EQ(*obj, 100);
}

TEST(OptionalWithStatus, DistinctMissingVsError)
{
  OptionalWithStatus<int, TestStatus> miss(TestStatus::Missing);
  OptionalWithStatus<int, TestStatus> error(TestStatus::Failed);

  EXPECT_EQ(miss.status(), TestStatus::Missing);
  EXPECT_EQ(error.status(), TestStatus::Failed);
  EXPECT_NE(miss.status(), error.status());
  EXPECT_FALSE(miss.has_value());
  EXPECT_FALSE(error.has_value());
}

TEST(OptionalWithStatus, SmallerThanEmbeddedOptional)
{
  // The manual storage packs the has-value bool next to the status field, saving
  // the padding that an embedded std::optional<T> member would add.
  //
  // For any T where alignof(T) > 1, sizeof(std::optional<T>) includes internal
  // padding after its bool, so embedding it as a member wastes space.
  // OptionalWithStatus avoids this by sharing that padding region with the status.
  struct LargeAlign { double d; int i; };
  static_assert(sizeof(OptionalWithStatus<LargeAlign, TestStatus>) <
                sizeof(std::optional<LargeAlign>) + sizeof(TestStatus),
                "OptionalWithStatus should be smaller than std::optional<T> + S");
}

// ============================================================================
// LockedPtr Tests
// ============================================================================

TEST(LockedPtr, DefaultConstruction)
{
  LockedPtr<int, std::mutex> ptr;
  EXPECT_FALSE(static_cast<bool>(ptr));
  EXPECT_EQ(ptr.get_ptr(), nullptr);
  EXPECT_EQ(ptr.get_mutex(), nullptr);
}

TEST(LockedPtr, LocksOnConstruction)
{
  std::mutex mtx;
  int value = 42;

  {
    LockedPtr<int, std::mutex> ptr(mtx, &value);
    EXPECT_TRUE(static_cast<bool>(ptr));
    EXPECT_EQ(ptr.get_ptr(), &value);
    EXPECT_EQ(*ptr, 42);
    // mutex is held; try_lock should fail
    EXPECT_FALSE(mtx.try_lock());
  }

  // destructor released the lock; try_lock should succeed now
  EXPECT_TRUE(mtx.try_lock());
  mtx.unlock();
}

TEST(LockedPtr, AdoptLock)
{
  std::mutex mtx;
  mtx.lock();
  int value = 7;

  {
    LockedPtr<int, std::mutex> ptr(mtx, &value, std::adopt_lock);
    EXPECT_TRUE(static_cast<bool>(ptr));
    EXPECT_EQ(*ptr, 7);
    // lock was adopted; try_lock should still fail (ptr holds it)
    EXPECT_FALSE(mtx.try_lock());
  }

  // released on destruction
  EXPECT_TRUE(mtx.try_lock());
  mtx.unlock();
}

TEST(LockedPtr, UniqueLockConstruction)
{
  std::mutex mtx;
  int value = 99;

  std::unique_lock<std::mutex> lock(mtx);
  EXPECT_FALSE(mtx.try_lock()); // lock held

  LockedPtr<int, std::mutex> ptr(std::move(lock), &value);
  EXPECT_TRUE(static_cast<bool>(ptr));
  EXPECT_EQ(*ptr, 99);
  EXPECT_FALSE(mtx.try_lock()); // still held by ptr

  ptr = LockedPtr<int, std::mutex>{}; // release
  EXPECT_TRUE(mtx.try_lock());
  mtx.unlock();
}

TEST(LockedPtr, MoveConstruction)
{
  std::mutex mtx;
  int value = 3;

  LockedPtr<int, std::mutex> first(mtx, &value);
  EXPECT_FALSE(mtx.try_lock());

  LockedPtr<int, std::mutex> second(std::move(first));
  EXPECT_EQ(first.get_ptr(), nullptr);
  EXPECT_EQ(first.get_mutex(), nullptr);
  EXPECT_EQ(second.get_ptr(), &value);
  EXPECT_FALSE(mtx.try_lock()); // second still holds the lock
}

TEST(LockedPtr, MoveAssignment)
{
  std::mutex mtx1;
  std::mutex mtx2;
  int a = 1;
  int b = 2;

  LockedPtr<int, std::mutex> ptr1(mtx1, &a);
  LockedPtr<int, std::mutex> ptr2(mtx2, &b);

  // After move-assign, ptr2 should release mtx2 and take ownership of mtx1/ptr1
  ptr2 = std::move(ptr1);

  EXPECT_EQ(ptr2.get_ptr(), &a);
  EXPECT_FALSE(mtx1.try_lock()); // mtx1 still locked via ptr2
  EXPECT_TRUE(mtx2.try_lock());  // mtx2 was released
  mtx2.unlock();
}

TEST(LockedPtr, ArrowOperator)
{
  std::mutex mtx;
  std::string value = "test";

  LockedPtr<std::string, std::mutex> ptr(mtx, &value);
  EXPECT_EQ(ptr->size(), 4u);
}

TEST(LockedPtr, RecursiveMutex)
{
  std::recursive_mutex rmtx;
  int value = 55;

  LockedPtr<int, std::recursive_mutex> ptr(rmtx, &value);
  EXPECT_TRUE(static_cast<bool>(ptr));

  // recursive_mutex allows re-locking on the same thread
  rmtx.lock();
  rmtx.unlock();

  EXPECT_EQ(*ptr, 55);
}
