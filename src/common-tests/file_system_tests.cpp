// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/file_system.h"
#include "common/path.h"
#include "common/scoped_guard.h"

#include <gtest/gtest.h>

#ifdef _WIN32

TEST(FileSystem, GetWin32Path)
{
  ASSERT_EQ(FileSystem::GetWin32Path("test.txt"), L"test.txt");
  ASSERT_EQ(FileSystem::GetWin32Path("D:\\test.txt"), L"\\\\?\\D:\\test.txt");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\foo"), L"\\\\?\\C:\\foo");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\foo\\bar\\..\\baz"), L"\\\\?\\C:\\foo\\baz");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\baz"), L"\\\\?\\UNC\\foo\\bar\\baz");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\baz\\sub\\.."), L"\\\\?\\UNC\\foo\\bar\\baz");
  ASSERT_EQ(FileSystem::GetWin32Path("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"), L"ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"),
            L"\\\\?\\C:\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"),
            L"\\\\?\\UNC\\foo\\bar\\ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("C:\\ŻąłóРстуぬね\\のはen🍪\\⟑η∏☉ⴤ\\..\\ℹ︎∩₲ ₱⟑♰⫳🐱"),
            L"\\\\?\\C:\\ŻąłóРстуぬね\\のはen🍪\\ℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(FileSystem::GetWin32Path("\\\\foo\\bar\\ŻąłóРстуぬねのはen🍪\\⟑η∏☉ⴤ\\..\\ℹ︎∩₲ ₱⟑♰⫳🐱"),
            L"\\\\?\\UNC\\foo\\bar\\ŻąłóРстуぬねのはen🍪\\ℹ︎∩₲ ₱⟑♰⫳🐱");
}

#endif

static std::string GetTestTempBasePath(const char* name)
{
  return Path::Combine(FileSystem::GetWorkingDirectory(), name);
}

TEST(FileSystem, OpenTemporaryCFile)
{
  const std::string base_path = GetTestTempBasePath("duckstation_test_temp");
  std::string temp_path;
  std::FILE* fp = FileSystem::OpenTemporaryCFile(base_path, &temp_path);
  ASSERT_NE(fp, nullptr);

  ScopedGuard cleanup([&temp_path, fp]() {
    std::fclose(fp);
    FileSystem::DeleteFile(temp_path.c_str());
  });

  EXPECT_FALSE(temp_path.empty());
  EXPECT_TRUE(temp_path.starts_with(base_path + "."));
  EXPECT_FALSE(temp_path.ends_with("XXXXXX"));
  EXPECT_TRUE(FileSystem::FileExists(temp_path.c_str()));

  const char test_data[] = "hello";
  EXPECT_EQ(std::fwrite(test_data, 1, sizeof(test_data), fp), sizeof(test_data));
}

TEST(FileSystem, OpenTemporaryManagedCFile)
{
  const std::string base_path = GetTestTempBasePath("duckstation_test_temp_managed");
  std::string temp_path;
  auto fp = FileSystem::OpenTemporaryManagedCFile(base_path, &temp_path);
  ASSERT_NE(fp, nullptr);

  ScopedGuard cleanup([&temp_path]() {
    FileSystem::DeleteFile(temp_path.c_str());
  });

  EXPECT_FALSE(temp_path.empty());
  EXPECT_TRUE(temp_path.starts_with(base_path + "."));
  EXPECT_FALSE(temp_path.ends_with("XXXXXX"));
  EXPECT_TRUE(FileSystem::FileExists(temp_path.c_str()));
}

TEST(FileSystem, OpenTemporaryCFileNullOutPath)
{
  const std::string base_path = GetTestTempBasePath("duckstation_test_temp_null");
  std::FILE* fp = FileSystem::OpenTemporaryCFile(base_path, nullptr);
  ASSERT_NE(fp, nullptr);

  // Without out_path we can't reliably clean up the file, so just close it.
  std::fclose(fp);
}
