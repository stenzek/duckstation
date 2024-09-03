// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "small_string.h"
#include "types.h"

#include "fmt/base.h"

#include <memory>
#include <string>

class ProgressCallback
{
public:
  virtual ~ProgressCallback();

  virtual void PushState();
  virtual void PopState();

  virtual bool IsCancelled() const;
  virtual bool IsCancellable() const;

  virtual void SetCancellable(bool cancellable);

  virtual void SetTitle(const std::string_view title);
  virtual void SetStatusText(const std::string_view text);
  virtual void SetProgressRange(u32 range);
  virtual void SetProgressValue(u32 value);
  virtual void IncrementProgressValue();

  virtual void DisplayError(const std::string_view message);
  virtual void DisplayWarning(const std::string_view message);
  virtual void DisplayInformation(const std::string_view message);
  virtual void DisplayDebugMessage(const std::string_view message);

  virtual void ModalError(const std::string_view message);
  virtual bool ModalConfirmation(const std::string_view message);
  virtual void ModalInformation(const std::string_view message);

#define MAKE_PROGRESS_CALLBACK_FORWARDER(from, to)                                                                     \
  template<typename... T>                                                                                              \
  void from(fmt::format_string<T...> fmt, T&&... args)                                                                 \
  {                                                                                                                    \
    TinyString str;                                                                                                    \
    fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));                                     \
    to(str.view());                                                                                                    \
  }

  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatStatusText, SetStatusText);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatError, DisplayError);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatWarning, DisplayWarning);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatInformation, DisplayInformation);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatDebugMessage, DisplayDebugMessage);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalError, ModalError);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalConfirmation, ModalConfirmation);
  MAKE_PROGRESS_CALLBACK_FORWARDER(FormatModalInformation, ModalInformation);

#undef MAKE_PROGRESS_CALLBACK_FORWARDER

protected:
  struct State
  {
    std::unique_ptr<State> next_saved_state;
    std::string status_text;
    u32 progress_range;
    u32 progress_value;
    u32 base_progress_value;
    bool cancellable;
  };

  bool m_cancellable = false;
  bool m_cancelled = false;
  std::string m_status_text;
  u32 m_progress_range = 1;
  u32 m_progress_value = 0;

  u32 m_base_progress_value = 0;

  std::unique_ptr<State> m_saved_state;

public:
  static ProgressCallback* NullProgressCallback;
};
