// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <array>
#include <optional>
#include <string>

class CDImage;
class Error;
class ProgressCallback;

namespace CDImageHasher {

using Hash = std::array<u8, 16>;
std::string HashToString(const Hash& hash);
std::optional<Hash> HashFromString(std::string_view str);

bool GetImageHash(CDImage* image, Hash* out_hash, ProgressCallback* progress_callback, Error* error);
bool GetTrackHash(CDImage* image, u8 track, Hash* out_hash, ProgressCallback* progress_callback, Error* error);

} // namespace CDImageHasher