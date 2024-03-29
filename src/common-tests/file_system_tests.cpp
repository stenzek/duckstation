// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/file_system.h"
#include <gtest/gtest.h>

#ifdef _WIN32

TEST(FileSystem, GetWin32Path)
{
  ASSERT_EQ(FileSystem::GetWin32Path("test.txt"), L"test.txt");
  ASSERT_EQ(FileSystem::GetWin32Path("D:\\test.txt"), L"\\\\?\\D:\\test.txt");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\foo"), L"\\\\?\\C:\\foo");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\baz"), L"\\\\?\\UNC\\foo\\bar\\baz");
  ASSERT_EQ(FileSystem::GetWin32Path("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"), L"ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"), L"\\\\?\\C:\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"), L"\\\\?\\UNC\\foo\\bar\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
}

#endif
