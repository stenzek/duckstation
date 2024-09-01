// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"

#include <optional>
#include <span>

class Error;

namespace CompressHelpers {
enum class CompressType
{
  Uncompressed,
  Zstandard,
  Count
};

using ByteBuffer = DynamicHeapArray<u8>;
using OptionalByteBuffer = std::optional<ByteBuffer>;

OptionalByteBuffer DecompressBuffer(CompressType type, std::span<const u8> data,
                                    std::optional<size_t> decompressed_size = std::nullopt, Error* error = nullptr);
OptionalByteBuffer DecompressBuffer(CompressType type, OptionalByteBuffer data,
                                    std::optional<size_t> decompressed_size = std::nullopt, Error* error = nullptr);
OptionalByteBuffer DecompressFile(std::string_view path, std::span<const u8> data,
                                  std::optional<size_t> decompressed_size = std::nullopt, Error* error = nullptr);
OptionalByteBuffer DecompressFile(std::string_view path, OptionalByteBuffer data,
                                  std::optional<size_t> decompressed_size = std::nullopt, Error* error = nullptr);
OptionalByteBuffer DecompressFile(const char* path, std::optional<size_t> decompressed_size = std::nullopt,
                                  Error* error = nullptr);
OptionalByteBuffer DecompressFile(CompressType type, const char* path,
                                  std::optional<size_t> decompressed_size = std::nullopt, Error* error = nullptr);

OptionalByteBuffer CompressToBuffer(CompressType type, const void* data, size_t data_size, int clevel = -1,
                                    Error* error = nullptr);
OptionalByteBuffer CompressToBuffer(CompressType type, std::span<const u8> data, int clevel = -1,
                                    Error* error = nullptr);
OptionalByteBuffer CompressToBuffer(CompressType type, OptionalByteBuffer data, int clevel = -1,
                                    Error* error = nullptr);
bool CompressToFile(const char* path, std::span<const u8> data, int clevel = -1, bool atomic_write = true,
                    Error* error = nullptr);
bool CompressToFile(CompressType type, const char* path, std::span<const u8> data, int clevel = -1,
                    bool atomic_write = true, Error* error = nullptr);
} // namespace CompressHelpers
