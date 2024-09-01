// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "host_interface_progress_callback.h"
#include "host.h"

#include "common/log.h"

Log_SetChannel(HostInterfaceProgressCallback);

HostInterfaceProgressCallback::HostInterfaceProgressCallback() : ProgressCallback()
{
}

void HostInterfaceProgressCallback::PushState()
{
  ProgressCallback::PushState();
}

void HostInterfaceProgressCallback::PopState()
{
  ProgressCallback::PopState();
  Redraw(true);
}

void HostInterfaceProgressCallback::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void HostInterfaceProgressCallback::SetTitle(const std::string_view title)
{
  // todo?
}

void HostInterfaceProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  Redraw(true);
}

void HostInterfaceProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  ProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void HostInterfaceProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  ProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(false);
}

void HostInterfaceProgressCallback::Redraw(bool force)
{
  if (m_last_progress_percent < 0 && m_open_time.GetTimeSeconds() < m_open_delay)
    return;

  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  if (percent == m_last_progress_percent && !force)
    return;

  m_last_progress_percent = percent;
  Host::DisplayLoadingScreen(m_status_text.c_str(), 0, static_cast<int>(m_progress_range),
                             static_cast<int>(m_progress_value));
}

void HostInterfaceProgressCallback::ModalError(const std::string_view message)
{
  ERROR_LOG(message);
  Host::ReportErrorAsync("Error", message);
}

bool HostInterfaceProgressCallback::ModalConfirmation(const std::string_view message)
{
  INFO_LOG(message);
  return Host::ConfirmMessage("Confirm", message);
}
