// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace Path {
/// Converts any forward slashes to backslashes on Win32.
std::string ToNativePath(std::string_view path);
void ToNativePath(std::string* path);

/// Builds a path relative to the specified file
std::string BuildRelativePath(std::string_view filename, std::string_view new_filename);

/// Joins path components together, producing a new path.
std::string Combine(std::string_view base, std::string_view next);

/// Removes all .. and . components from a path.
std::string Canonicalize(std::string_view path);
void Canonicalize(std::string* path);

/// Sanitizes a filename for use in a filesystem.
std::string SanitizeFileName(std::string_view str, bool strip_slashes = true);
void SanitizeFileName(std::string* str, bool strip_slashes = true);

/// Mutates the path to remove any MAX_PATH limits (for Windows).
std::string RemoveLengthLimits(std::string_view str);
void RemoveLengthLimits(std::string* path);

/// Returns true if the specified path is an absolute path (C:\Path on Windows or /path on Unix).
bool IsAbsolute(std::string_view path);

/// Resolves any symbolic links in the specified path.
std::string RealPath(std::string_view path);

/// Makes the specified path relative to another (e.g. /a/b/c, /a/b -> ../c).
/// Both paths must be relative, otherwise this function will just return the input path.
std::string MakeRelative(std::string_view path, std::string_view relative_to);

/// Returns a view of the extension of a filename.
std::string_view GetExtension(std::string_view path);

/// Removes the extension of a filename.
std::string_view StripExtension(std::string_view path);

/// Replaces the extension of a filename with another.
std::string ReplaceExtension(std::string_view path, std::string_view new_extension);

/// Returns the directory component of a filename.
std::string_view GetDirectory(std::string_view path);

/// Returns the filename component of a filename.
std::string_view GetFileName(std::string_view path);

/// Returns the file title (less the extension and path) from a filename.
std::string_view GetFileTitle(std::string_view path);

/// Changes the filename in a path.
std::string ChangeFileName(std::string_view path, std::string_view new_filename);
void ChangeFileName(std::string* path, std::string_view new_filename);

/// Appends a directory to a path.
std::string AppendDirectory(std::string_view path, std::string_view new_dir);
void AppendDirectory(std::string* path, std::string_view new_dir);

/// Splits a path into its components, handling both Windows and Unix separators.
std::vector<std::string_view> SplitWindowsPath(std::string_view path);
std::string JoinWindowsPath(const std::vector<std::string_view>& components);

/// Splits a path into its components, only handling native separators.
std::vector<std::string_view> SplitNativePath(std::string_view path);
std::string JoinNativePath(const std::vector<std::string_view>& components);

/// URL encodes the specified string.
std::string URLEncode(std::string_view str);

/// Decodes the specified escaped string.
std::string URLDecode(std::string_view str);

/// Returns a URL for a given path. The path should be absolute.
std::string CreateFileURL(std::string_view path);
} // namespace Path
