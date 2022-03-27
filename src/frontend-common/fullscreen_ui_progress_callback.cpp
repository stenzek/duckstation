#include "fullscreen_ui_progress_callback.h"
#include "common/log.h"
#include "core/host_interface.h"
#include "core/imgui_fullscreen.h"
Log_SetChannel(ProgressCallback);

namespace FullscreenUI {

ProgressCallback::ProgressCallback(String name) : BaseProgressCallback(), m_name(std::move(name))
{
  ImGuiFullscreen::OpenBackgroundProgressDialog(m_name, "", 0, 100, 0);
}

ProgressCallback::~ProgressCallback()
{
  ImGuiFullscreen::CloseBackgroundProgressDialog(m_name);
}

void ProgressCallback::PushState()
{
  BaseProgressCallback::PushState();
}

void ProgressCallback::PopState()
{
  BaseProgressCallback::PopState();
  Redraw(true);
}

void ProgressCallback::SetCancellable(bool cancellable)
{
  BaseProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void ProgressCallback::SetTitle(const char* title)
{
  // todo?
}

void ProgressCallback::SetStatusText(const char* text)
{
  BaseProgressCallback::SetStatusText(text);
  Redraw(true);
}

void ProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  BaseProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void ProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  BaseProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(false);
}

void ProgressCallback::Redraw(bool force)
{
  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  if (percent == m_last_progress_percent && !force)
    return;

  m_last_progress_percent = percent;
  ImGuiFullscreen::UpdateBackgroundProgressDialog(
    m_name, m_status_text.GetCharArray(), 0, 100, percent);
}

void ProgressCallback::DisplayError(const char* message)
{
  Log_ErrorPrint(message);
}

void ProgressCallback::DisplayWarning(const char* message)
{
  Log_WarningPrint(message);
}

void ProgressCallback::DisplayInformation(const char* message)
{
  Log_InfoPrint(message);
}

void ProgressCallback::DisplayDebugMessage(const char* message)
{
  Log_DevPrint(message);
}

void ProgressCallback::ModalError(const char* message)
{
  Log_ErrorPrint(message);
  g_host_interface->ReportError(message);
}

bool ProgressCallback::ModalConfirmation(const char* message)
{
  Log_InfoPrint(message);
  return g_host_interface->ConfirmMessage(message);
}

void ProgressCallback::ModalInformation(const char* message)
{
  Log_InfoPrint(message);
  g_host_interface->ReportMessage(message);
}

} // namespace FullscreenUI