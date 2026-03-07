// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#if defined(CPU_ARCH_ARM64)
inline constexpr const char* INSTALLER_PROGRAM_FILENAME = "duckstation-qt-ARM64-ReleaseLTCG.exe";
#else
inline constexpr const char* INSTALLER_PROGRAM_FILENAME = "duckstation-qt-x64-ReleaseLTCG.exe";
#endif

inline constexpr const char* INSTALLER_UNINSTALLER_FILENAME = "uninstaller.exe";
inline constexpr const char* INSTALLER_SHORTCUT_FILENAME = "DuckStation.lnk";
inline constexpr const wchar_t* INSTALLER_UNINSTALL_REG_KEY =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DuckStation";