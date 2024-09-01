// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/progress_callback.h"
#include "common/timer.h"

class HostInterfaceProgressCallback final : public ProgressCallback
{
public:
  HostInterfaceProgressCallback();

  ALWAYS_INLINE void SetOpenDelay(float delay) { m_open_delay = delay; }

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;

private:
  void Redraw(bool force);

  Common::Timer m_open_time;
  float m_open_delay = 1.0f;
  int m_last_progress_percent = -1;
};
