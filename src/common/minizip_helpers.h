// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "unzip.h"

namespace MinizipHelpers {

unzFile OpenUnzMemoryFile(const void* memory, size_t memory_size);
unzFile OpenUnzFile(const char* filename);

} // namespace MinizipHelpers