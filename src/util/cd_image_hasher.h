// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/progress_callback.h"
#include "common/types.h"
#include <array>
#include <optional>
#include <string>

class CDImage;

namespace CDImageHasher {

using Hash = std::array<u8, 16>;
std::string HashToString(const Hash& hash);
std::optional<Hash> HashFromString(std::string_view str);

bool GetImageHash(CDImage* image, Hash* out_hash,
                  ProgressCallback* progress_callback = ProgressCallback::NullProgressCallback);
bool GetTrackHash(CDImage* image, u8 track, Hash* out_hash,
                  ProgressCallback* progress_callback = ProgressCallback::NullProgressCallback);

} // namespace CDImageHasher