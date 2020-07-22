#include "common/file_system.h"
#include <gtest/gtest.h>

TEST(FileSystem, IsAbsolutePath)
{
#ifdef WIN32
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:\\"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:\\Path"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:\\Path\\Subdirectory"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:/"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:/Path"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("C:/Path/Subdirectory"));
  ASSERT_FALSE(FileSystem::IsAbsolutePath(""));
  ASSERT_FALSE(FileSystem::IsAbsolutePath("C:"));
  ASSERT_FALSE(FileSystem::IsAbsolutePath("Path"));
  ASSERT_FALSE(FileSystem::IsAbsolutePath("Path/Subdirectory"));
#else
  ASSERT_TRUE(FileSystem::IsAbsolutePath("/"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("/path"));
  ASSERT_TRUE(FileSystem::IsAbsolutePath("/path/subdirectory"));
  ASSERT_FALSE(FileSystem::IsAbsolutePath(""));
  ASSERT_FALSE(FileSystem::IsAbsolutePath("path"));
  ASSERT_FALSE(FileSystem::IsAbsolutePath("path/subdirectory"));
#endif
}
