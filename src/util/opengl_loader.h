// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR PolyForm-Strict-1.0.0)

#pragma once

// Fix glad.h including windows.h
#ifdef _WIN32
#include "common/windows_headers.h"
#endif

#include "glad/gl.h"