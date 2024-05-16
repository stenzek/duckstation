// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "platform_misc.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <algorithm>
#include <cinttypes>
#include <memory>

#include "common/windows_headers.h"
#include <mmsystem.h>

Log_SetChannel(PlatformMisc);

static bool SetScreensaverInhibitWin32(bool inhibit)
{
  if (SetThreadExecutionState(ES_CONTINUOUS | (inhibit ? (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : 0)) == NULL)
  {
    Log_ErrorPrintf("SetThreadExecutionState() failed: %d", GetLastError());
    return false;
  }

  return true;
}

static bool s_screensaver_suspended;

void PlatformMisc::SuspendScreensaver()
{
  if (s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(true))
  {
    Log_ErrorPrintf("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

size_t PlatformMisc::GetRuntimePageSize()
{
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  return si.dwPageSize;
}

size_t PlatformMisc::GetRuntimeCacheLineSize()
{
  DWORD size = 0;
  if (!GetLogicalProcessorInformation(nullptr, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return 0;

  std::unique_ptr<SYSTEM_LOGICAL_PROCESSOR_INFORMATION[]> lpi =
    std::make_unique<SYSTEM_LOGICAL_PROCESSOR_INFORMATION[]>(
      (size + (sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1)) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (!GetLogicalProcessorInformation(lpi.get(), &size))
    return 0;

  u32 max_line_size = 0;
  for (u32 i = 0; i < size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++)
  {
    if (lpi[i].Relationship == RelationCache)
      max_line_size = std::max<u32>(max_line_size, lpi[i].Cache.LineSize);
  }

  return max_line_size;
}

bool PlatformMisc::PlaySoundAsync(const char* path)
{
  const std::wstring wpath(FileSystem::GetWin32Path(path));
  return PlaySoundW(wpath.c_str(), NULL, SND_ASYNC | SND_NODEFAULT);
}
