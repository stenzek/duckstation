// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/path.h"
#include "common/types.h"
#include <gtest/gtest.h>

TEST(Path, ToNativePath)
{
  ASSERT_EQ(Path::ToNativePath(""), "");

#ifdef _WIN32
  ASSERT_EQ(Path::ToNativePath("foo"), "foo");
  ASSERT_EQ(Path::ToNativePath("foo\\"), "foo");
  ASSERT_EQ(Path::ToNativePath("foo\\\\bar"), "foo\\bar");
  ASSERT_EQ(Path::ToNativePath("foo\\bar"), "foo\\bar");
  ASSERT_EQ(Path::ToNativePath("foo\\bar\\baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::ToNativePath("foo\\bar/baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::ToNativePath("foo/bar/baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::ToNativePath("foo/🙃bar/b🙃az"), "foo\\🙃bar\\b🙃az");
  ASSERT_EQ(Path::ToNativePath("\\\\foo\\bar\\baz"), "\\\\foo\\bar\\baz");
#else
  ASSERT_EQ(Path::ToNativePath("foo"), "foo");
  ASSERT_EQ(Path::ToNativePath("foo/"), "foo");
  ASSERT_EQ(Path::ToNativePath("foo//bar"), "foo/bar");
  ASSERT_EQ(Path::ToNativePath("foo/bar"), "foo/bar");
  ASSERT_EQ(Path::ToNativePath("foo/bar/baz"), "foo/bar/baz");
  ASSERT_EQ(Path::ToNativePath("/foo/bar/baz"), "/foo/bar/baz");
#endif
}

TEST(Path, IsAbsolute)
{
  ASSERT_FALSE(Path::IsAbsolute(""));
  ASSERT_FALSE(Path::IsAbsolute("foo"));
  ASSERT_FALSE(Path::IsAbsolute("foo/bar"));
  ASSERT_FALSE(Path::IsAbsolute("foo/b🙃ar"));
#ifdef _WIN32
  ASSERT_TRUE(Path::IsAbsolute("C:\\foo/bar"));
  ASSERT_TRUE(Path::IsAbsolute("C://foo\\bar"));
  ASSERT_FALSE(Path::IsAbsolute("\\foo/bar"));
  ASSERT_TRUE(Path::IsAbsolute("\\\\foo\\bar\\baz"));
  ASSERT_TRUE(Path::IsAbsolute("C:\\"));
  ASSERT_TRUE(Path::IsAbsolute("C:\\Path"));
  ASSERT_TRUE(Path::IsAbsolute("C:\\Path\\Subdirectory"));
  ASSERT_TRUE(Path::IsAbsolute("C:/"));
  ASSERT_TRUE(Path::IsAbsolute("C:/Path"));
  ASSERT_TRUE(Path::IsAbsolute("C:/Path/Subdirectory"));
  ASSERT_FALSE(Path::IsAbsolute(""));
  ASSERT_FALSE(Path::IsAbsolute("C:"));
  ASSERT_FALSE(Path::IsAbsolute("Path"));
  ASSERT_FALSE(Path::IsAbsolute("Path/Subdirectory"));
#else
  ASSERT_TRUE(Path::IsAbsolute("/foo/bar"));
  ASSERT_TRUE(Path::IsAbsolute("/"));
  ASSERT_TRUE(Path::IsAbsolute("/path"));
  ASSERT_TRUE(Path::IsAbsolute("/path/subdirectory"));
  ASSERT_FALSE(Path::IsAbsolute(""));
  ASSERT_FALSE(Path::IsAbsolute("path"));
  ASSERT_FALSE(Path::IsAbsolute("path/subdirectory"));
#endif
}

TEST(Path, Canonicalize)
{
  ASSERT_EQ(Path::Canonicalize(""), Path::ToNativePath(""));
  ASSERT_EQ(Path::Canonicalize("foo/bar/../baz"), Path::ToNativePath("foo/baz"));
  ASSERT_EQ(Path::Canonicalize("foo/bar/./baz"), Path::ToNativePath("foo/bar/baz"));
  ASSERT_EQ(Path::Canonicalize("foo/./bar/./baz"), Path::ToNativePath("foo/bar/baz"));
  ASSERT_EQ(Path::Canonicalize("foo/bar/../baz/../foo"), Path::ToNativePath("foo/foo"));
  ASSERT_EQ(Path::Canonicalize("foo/bar/../baz/./foo"), Path::ToNativePath("foo/baz/foo"));
  ASSERT_EQ(Path::Canonicalize("./foo"), Path::ToNativePath("foo"));
  ASSERT_EQ(Path::Canonicalize("../foo"), Path::ToNativePath("../foo"));
  ASSERT_EQ(Path::Canonicalize("foo/b🙃ar/../b🙃az/./foo"), Path::ToNativePath("foo/b🙃az/foo"));
  ASSERT_EQ(Path::Canonicalize("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃az/../foℹ︎o"),
            Path::ToNativePath("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/foℹ︎o"));
#ifdef _WIN32
  ASSERT_EQ(Path::Canonicalize("C:\\foo\\bar\\..\\baz\\.\\foo"), "C:\\foo\\baz\\foo");
  ASSERT_EQ(Path::Canonicalize("C:/foo\\bar\\..\\baz\\.\\foo"), "C:\\foo\\baz\\foo");
  ASSERT_EQ(Path::Canonicalize("foo\\bar\\..\\baz\\.\\foo"), "foo\\baz\\foo");
  ASSERT_EQ(Path::Canonicalize("foo\\bar/..\\baz/.\\foo"), "foo\\baz\\foo");
  ASSERT_EQ(Path::Canonicalize("\\\\foo\\bar\\baz/..\\foo"), "\\\\foo\\bar\\foo");
#else
  ASSERT_EQ(Path::Canonicalize("/foo/bar/../baz/./foo"), "/foo/baz/foo");
#endif
}

TEST(Path, Combine)
{
  ASSERT_EQ(Path::Combine("", ""), Path::ToNativePath(""));
  ASSERT_EQ(Path::Combine("foo", "bar"), Path::ToNativePath("foo/bar"));
  ASSERT_EQ(Path::Combine("foo/bar", "baz"), Path::ToNativePath("foo/bar/baz"));
  ASSERT_EQ(Path::Combine("foo/bar", "../baz"), Path::ToNativePath("foo/bar/../baz"));
  ASSERT_EQ(Path::Combine("foo/bar/", "/baz/"), Path::ToNativePath("foo/bar/baz"));
  ASSERT_EQ(Path::Combine("foo//bar", "baz/"), Path::ToNativePath("foo/bar/baz"));
  ASSERT_EQ(Path::Combine("foo//ba🙃r", "b🙃az/"), Path::ToNativePath("foo/ba🙃r/b🙃az"));
#ifdef _WIN32
  ASSERT_EQ(Path::Combine("C:\\foo\\bar", "baz"), "C:\\foo\\bar\\baz");
  ASSERT_EQ(Path::Combine("\\\\server\\foo\\bar", "baz"), "\\\\server\\foo\\bar\\baz");
  ASSERT_EQ(Path::Combine("foo\\bar", "baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::Combine("foo\\bar\\", "baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::Combine("foo/bar\\", "\\baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::Combine("\\\\foo\\bar", "baz"), "\\\\foo\\bar\\baz");
#else
  ASSERT_EQ(Path::Combine("/foo/bar", "baz"), "/foo/bar/baz");
#endif
}

TEST(Path, AppendDirectory)
{
  ASSERT_EQ(Path::AppendDirectory("foo/bar", "baz"), Path::ToNativePath("foo/baz/bar"));
  ASSERT_EQ(Path::AppendDirectory("", "baz"), Path::ToNativePath("baz"));
  ASSERT_EQ(Path::AppendDirectory("", ""), Path::ToNativePath(""));
  ASSERT_EQ(Path::AppendDirectory("foo/bar", "🙃"), Path::ToNativePath("foo/🙃/bar"));
#ifdef _WIN32
  ASSERT_EQ(Path::AppendDirectory("foo\\bar", "baz"), "foo\\baz\\bar");
  ASSERT_EQ(Path::AppendDirectory("\\\\foo\\bar", "baz"), "\\\\foo\\baz\\bar");
#else
  ASSERT_EQ(Path::AppendDirectory("/foo/bar", "baz"), "/foo/baz/bar");
#endif
}

TEST(Path, MakeRelative)
{
  ASSERT_EQ(Path::MakeRelative("", ""), Path::ToNativePath(""));
  ASSERT_EQ(Path::MakeRelative("foo", ""), Path::ToNativePath("foo"));
  ASSERT_EQ(Path::MakeRelative("", "foo"), Path::ToNativePath(""));
  ASSERT_EQ(Path::MakeRelative("foo", "bar"), Path::ToNativePath("foo"));

#ifdef _WIN32
#define A "C:\\"
#else
#define A "/"
#endif

  ASSERT_EQ(Path::MakeRelative(A "foo", A "bar"), Path::ToNativePath("../foo"));
  ASSERT_EQ(Path::MakeRelative(A "foo/bar", A "foo"), Path::ToNativePath("bar"));
  ASSERT_EQ(Path::MakeRelative(A "foo/bar", A "foo/baz"), Path::ToNativePath("../bar"));
  ASSERT_EQ(Path::MakeRelative(A "foo/b🙃ar", A "foo/b🙃az"), Path::ToNativePath("../b🙃ar"));
  ASSERT_EQ(Path::MakeRelative(A "f🙃oo/b🙃ar", A "f🙃oo/b🙃az"), Path::ToNativePath("../b🙃ar"));
  ASSERT_EQ(
    Path::MakeRelative(A "ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃ar", A "ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃az"),
    Path::ToNativePath("../b🙃ar"));

#undef A

#ifdef _WIN32
  ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\baz\\foo", "\\\\foo\\bar\\baz"), "foo");
  ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\foo", "\\\\foo\\bar\\baz"), "..\\foo");
  ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\foo", "\\\\other\\bar\\foo"), "\\\\foo\\bar\\foo");
#endif
}

TEST(Path, GetExtension)
{
  ASSERT_EQ(Path::GetExtension("foo"), "");
  ASSERT_EQ(Path::GetExtension("foo.txt"), "txt");
  ASSERT_EQ(Path::GetExtension("foo.t🙃t"), "t🙃t");
  ASSERT_EQ(Path::GetExtension("foo."), "");
  ASSERT_EQ(Path::GetExtension("a/b/foo.txt"), "txt");
  ASSERT_EQ(Path::GetExtension("a/b/foo"), "");
}

TEST(Path, GetFileName)
{
  ASSERT_EQ(Path::GetFileName(""), "");
  ASSERT_EQ(Path::GetFileName("foo"), "foo");
  ASSERT_EQ(Path::GetFileName("foo.txt"), "foo.txt");
  ASSERT_EQ(Path::GetFileName("foo"), "foo");
  ASSERT_EQ(Path::GetFileName("foo/bar/."), ".");
  ASSERT_EQ(Path::GetFileName("foo/bar/baz"), "baz");
  ASSERT_EQ(Path::GetFileName("foo/bar/baz.txt"), "baz.txt");
#ifdef _WIN32
  ASSERT_EQ(Path::GetFileName("foo/bar\\baz"), "baz");
  ASSERT_EQ(Path::GetFileName("foo\\bar\\baz.txt"), "baz.txt");
#endif
}

TEST(Path, GetFileTitle)
{
  ASSERT_EQ(Path::GetFileTitle(""), "");
  ASSERT_EQ(Path::GetFileTitle("foo"), "foo");
  ASSERT_EQ(Path::GetFileTitle("foo.txt"), "foo");
  ASSERT_EQ(Path::GetFileTitle("foo/bar/."), "");
  ASSERT_EQ(Path::GetFileTitle("foo/bar/baz"), "baz");
  ASSERT_EQ(Path::GetFileTitle("foo/bar/baz.txt"), "baz");
#ifdef _WIN32
  ASSERT_EQ(Path::GetFileTitle("foo/bar\\baz"), "baz");
  ASSERT_EQ(Path::GetFileTitle("foo\\bar\\baz.txt"), "baz");
#endif
}

TEST(Path, GetDirectory)
{
  ASSERT_EQ(Path::GetDirectory(""), "");
  ASSERT_EQ(Path::GetDirectory("foo"), "");
  ASSERT_EQ(Path::GetDirectory("foo.txt"), "");
  ASSERT_EQ(Path::GetDirectory("foo/bar/."), "foo/bar");
  ASSERT_EQ(Path::GetDirectory("foo/bar/baz"), "foo/bar");
  ASSERT_EQ(Path::GetDirectory("foo/bar/baz.txt"), "foo/bar");
#ifdef _WIN32
  ASSERT_EQ(Path::GetDirectory("foo\\bar\\baz"), "foo\\bar");
  ASSERT_EQ(Path::GetDirectory("foo\\bar/baz.txt"), "foo\\bar");
#endif
}

TEST(Path, ChangeFileName)
{
  ASSERT_EQ(Path::ChangeFileName("", ""), Path::ToNativePath(""));
  ASSERT_EQ(Path::ChangeFileName("", "bar"), Path::ToNativePath("bar"));
  ASSERT_EQ(Path::ChangeFileName("bar", ""), Path::ToNativePath(""));
  ASSERT_EQ(Path::ChangeFileName("foo/bar", ""), Path::ToNativePath("foo"));
  ASSERT_EQ(Path::ChangeFileName("foo/", "bar"), Path::ToNativePath("foo/bar"));
  ASSERT_EQ(Path::ChangeFileName("foo/bar", "baz"), Path::ToNativePath("foo/baz"));
  ASSERT_EQ(Path::ChangeFileName("foo//bar", "baz"), Path::ToNativePath("foo/baz"));
  ASSERT_EQ(Path::ChangeFileName("foo//bar.txt", "baz.txt"), Path::ToNativePath("foo/baz.txt"));
  ASSERT_EQ(Path::ChangeFileName("foo//ba🙃r.txt", "ba🙃z.txt"), Path::ToNativePath("foo/ba🙃z.txt"));
#ifdef _WIN32
  ASSERT_EQ(Path::ChangeFileName("foo/bar", "baz"), "foo\\baz");
  ASSERT_EQ(Path::ChangeFileName("foo//bar\\foo", "baz"), "foo\\bar\\baz");
  ASSERT_EQ(Path::ChangeFileName("\\\\foo\\bar\\foo", "baz"), "\\\\foo\\bar\\baz");
#else
  ASSERT_EQ(Path::ChangeFileName("/foo/bar", "baz"), "/foo/baz");
#endif
}

TEST(Path, SanitizeFileName)
{
  ASSERT_EQ(Path::SanitizeFileName("foo"), "foo");
  ASSERT_EQ(Path::SanitizeFileName("foo/bar"), "foo_bar");
  ASSERT_EQ(Path::SanitizeFileName("f🙃o"), "f🙃o");
  ASSERT_EQ(Path::SanitizeFileName("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱"), "ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱");
  ASSERT_EQ(Path::SanitizeFileName("abcdefghijlkmnopqrstuvwxyz-0123456789+&=_[]{}"),
            "abcdefghijlkmnopqrstuvwxyz-0123456789+&=_[]{}");
  ASSERT_EQ(Path::SanitizeFileName("some*path**with*asterisks"), "some_path__with_asterisks");
#ifdef _WIN32
  ASSERT_EQ(Path::SanitizeFileName("foo:"), "foo_");
  ASSERT_EQ(Path::SanitizeFileName("foo:bar."), "foo_bar_");
  ASSERT_EQ(Path::SanitizeFileName("foo\\bar"), "foo_bar");
  ASSERT_EQ(Path::SanitizeFileName("foo>bar"), "foo_bar");
  ASSERT_EQ(Path::SanitizeFileName("foo\\bar", false), "foo\\bar");
#endif
  ASSERT_EQ(Path::SanitizeFileName("foo/bar", false), "foo/bar");
}

TEST(Path, RemoveLengthLimits)
{
#ifdef _WIN32
  ASSERT_EQ(Path::RemoveLengthLimits("C:\\foo"), "\\\\?\\C:\\foo");
  ASSERT_EQ(Path::RemoveLengthLimits("\\\\foo\\bar\\baz"), "\\\\?\\UNC\\foo\\bar\\baz");
#else
  ASSERT_EQ(Path::RemoveLengthLimits("/foo/bar/baz"), "/foo/bar/baz");
#endif
}

#if 0

// Relies on presence of files.
TEST(Path, RealPath)
{
#ifdef _WIN32
  ASSERT_EQ(Path::RealPath("C:\\Users\\Me\\Desktop\\foo\\baz"), "C:\\Users\\Me\\Desktop\\foo\\bar\\baz");
#else
  ASSERT_EQ(Path::RealPath("/lib/foo/bar"), "/usr/lib/foo/bar");
#endif
}

#endif

TEST(Path, CreateFileURL)
{
#ifdef _WIN32
  ASSERT_EQ(Path::CreateFileURL("C:\\foo\\bar"), "file:///C:/foo/bar");
  ASSERT_EQ(Path::CreateFileURL("\\\\server\\share\\file.txt"), "file://server/share/file.txt");
#else
  ASSERT_EQ(Path::CreateFileURL("/foo/bar"), "file:///foo/bar");
#endif
}
