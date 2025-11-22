// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/progress_callback.h"

class UpdaterProgressCallback : public ProgressCallback
{
public:
  UpdaterProgressCallback() = default;
  virtual ~UpdaterProgressCallback() override = default;

  virtual void DisplayError(const std::string_view message) = 0;
  virtual void DisplayWarning(const std::string_view message) = 0;
  virtual void DisplayInformation(const std::string_view message) = 0;
  virtual void DisplayDebugMessage(const std::string_view message) = 0;

  virtual void ModalError(const std::string_view message) = 0;
  virtual bool ModalConfirmation(const std::string_view message) = 0;
  virtual void ModalInformation(const std::string_view message) = 0;

#define MAKE_PROGRESS_CALLBACK_FORWARDER(from, to)                                                                     \
  template<typename... T>                                                                                              \
  void from(fmt::format_string<T...> fmt, T&&... args)                                                                 \
  {                                                                                                                    \
    TinyString str;                                                                                                    \
    fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));                                     \
    to(str.view());                                                                                                    \
  }

  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatError, DisplayError);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatWarning, DisplayWarning);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatInformation, DisplayInformation);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatDebugMessage, DisplayDebugMessage);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalError, ModalError);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalConfirmation, ModalConfirmation);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalInformation, ModalInformation);

#undef MAKE_PROGRESS_CALLBACK_FORWARDER
};
