// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/progress_callback.h"
#include "common/timer.h"

class HostInterfaceProgressCallback final : public BaseProgressCallback
{
public:
  HostInterfaceProgressCallback();

  ALWAYS_INLINE void SetOpenDelay(float delay) { m_open_delay = delay; }

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const char* title) override;
  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;

  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  void ModalInformation(const char* message) override;

private:
  void Redraw(bool force);

  Common::Timer m_open_time;
  float m_open_delay = 1.0f;
  int m_last_progress_percent = -1;
};
