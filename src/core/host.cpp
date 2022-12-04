// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "host.h"
#include "common/string_util.h"
#include <cstdarg>

void Host::ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message(StringUtil::StdStringFromFormatV(format, ap));
  va_end(ap);
  ReportErrorAsync(title, message);
}

bool Host::ConfirmFormattedMessage(const std::string_view& title, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  return ConfirmMessage(title, message);
}

void Host::ReportFormattedDebuggerMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  ReportDebuggerMessage(message);
}
