#pragma once
#include "progress_callback.h"
#include "types.h"
#include <array>
#include <optional>
#include <string>

class CDImage;

namespace CDImageHasher {

using Hash = std::array<u8, 16>;
std::string HashToString(const Hash& hash);
std::optional<Hash> HashFromString(const std::string_view& str);

bool GetImageHash(CDImage* image, Hash* out_hash,
                  ProgressCallback* progress_callback = ProgressCallback::NullProgressCallback);
bool GetTrackHash(CDImage* image, u8 track, Hash* out_hash,
                  ProgressCallback* progress_callback = ProgressCallback::NullProgressCallback);

} // namespace CDImageHasher