#include "progress_callback.h"
#include "assert.h"
#include "byte_stream.h"
#include "log.h"
#include <cmath>
#include <cstdio>
#include <limits>
Log_SetChannel(ProgressCallback);

ProgressCallback::~ProgressCallback() {}

void ProgressCallback::SetFormattedStatusText(const char* Format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, Format);
  str.FormatVA(Format, ap);
  va_end(ap);

  SetStatusText(str);
}

void ProgressCallback::DisplayFormattedError(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayError(str);
}

void ProgressCallback::DisplayFormattedWarning(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayWarning(str);
}

void ProgressCallback::DisplayFormattedInformation(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayInformation(str);
}

void ProgressCallback::DisplayFormattedDebugMessage(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayDebugMessage(str);
}

void ProgressCallback::DisplayFormattedModalError(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  ModalError(str);
}

bool ProgressCallback::DisplayFormattedModalConfirmation(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  return ModalConfirmation(str);
}

void ProgressCallback::DisplayFormattedModalInformation(const char* format, ...)
{
  SmallString str;
  va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  ModalInformation(str);
}

void ProgressCallback::UpdateProgressFromStream(ByteStream* pStream)
{
  u32 streamSize = (u32)pStream->GetSize();
  u32 streamPosition = (u32)pStream->GetPosition();

  SetProgressRange(streamSize);
  SetProgressValue(streamPosition);
}

class NullProgressCallbacks final : public ProgressCallback
{
public:
  void PushState() override {}
  void PopState() override {}

  bool IsCancelled() const override { return false; }
  bool IsCancellable() const override { return false; }

  void SetCancellable(bool cancellable) override {}
  void SetTitle(const char* title) override {}
  void SetStatusText(const char* statusText) override {}
  void SetProgressRange(u32 range) override {}
  void SetProgressValue(u32 value) override {}
  void IncrementProgressValue() override {}

  void DisplayError(const char* message) override { Log_ErrorPrint(message); }
  void DisplayWarning(const char* message) override { Log_WarningPrint(message); }
  void DisplayInformation(const char* message) override { Log_InfoPrint(message); }
  void DisplayDebugMessage(const char* message) override { Log_DevPrint(message); }

  void ModalError(const char* message) override { Log_ErrorPrint(message); }
  bool ModalConfirmation(const char* message) override
  {
    Log_InfoPrint(message);
    return false;
  }
  void ModalInformation(const char* message) override { Log_InfoPrint(message); }
};

static NullProgressCallbacks s_nullProgressCallbacks;
ProgressCallback* ProgressCallback::NullProgressCallback = &s_nullProgressCallbacks;

BaseProgressCallback::BaseProgressCallback()
  : m_cancellable(false), m_cancelled(false), m_progress_range(1), m_progress_value(0), m_base_progress_value(0),
    m_saved_state(NULL)
{
}

BaseProgressCallback::~BaseProgressCallback()
{
  State* pNextState = m_saved_state;
  while (pNextState != NULL)
  {
    State* pCurrentState = pNextState;
    pNextState = pCurrentState->next_saved_state;
    delete pCurrentState;
  }
}

void BaseProgressCallback::PushState()
{
  State* pNewState = new State;
  pNewState->cancellable = m_cancellable;
  pNewState->status_text = m_status_text;
  pNewState->progress_range = m_progress_range;
  pNewState->progress_value = m_progress_value;
  pNewState->base_progress_value = m_base_progress_value;
  pNewState->next_saved_state = m_saved_state;
  m_saved_state = pNewState;
}

void BaseProgressCallback::PopState()
{
  DebugAssert(m_saved_state);
  State* state = m_saved_state;
  m_saved_state = nullptr;

  // impose the current position into the previous range
  const u32 new_progress_value =
    (m_progress_range != 0) ?
      static_cast<u32>(((float)m_progress_value / (float)m_progress_range) * (float)state->progress_range) :
      state->progress_value;

  m_cancellable = state->cancellable;
  m_status_text = std::move(state->status_text);
  m_progress_range = state->progress_range;
  m_progress_value = new_progress_value;

  m_base_progress_value = state->base_progress_value;
  m_saved_state = state->next_saved_state;
  delete state;
}

bool BaseProgressCallback::IsCancelled() const
{
  return m_cancelled;
}

bool BaseProgressCallback::IsCancellable() const
{
  return m_cancellable;
}

void BaseProgressCallback::SetCancellable(bool cancellable)
{
  m_cancellable = cancellable;
}

void BaseProgressCallback::SetStatusText(const char* text)
{
  m_status_text = text;
}

void BaseProgressCallback::SetProgressRange(u32 range)
{
  if (m_saved_state)
  {
    // impose the previous range on this range
    m_progress_range = m_saved_state->progress_range * range;
    m_base_progress_value = m_progress_value = m_saved_state->progress_value * range;
  }
  else
  {
    m_progress_range = range;
    m_progress_value = 0;
    m_base_progress_value = 0;
  }
}

void BaseProgressCallback::SetProgressValue(u32 value)
{
  m_progress_value = m_base_progress_value + value;
}

void BaseProgressCallback::IncrementProgressValue()
{
  SetProgressValue((m_progress_value - m_base_progress_value) + 1);
}

ConsoleProgressCallback::ConsoleProgressCallback()
  : BaseProgressCallback(), m_last_percent_complete(std::numeric_limits<float>::infinity()),
    m_last_bar_length(0xFFFFFFFF)
{
}

ConsoleProgressCallback::~ConsoleProgressCallback()
{
  Clear();
}

void ConsoleProgressCallback::PushState()
{
  BaseProgressCallback::PushState();
}

void ConsoleProgressCallback::PopState()
{
  BaseProgressCallback::PopState();
  Redraw(false);
}

void ConsoleProgressCallback::SetCancellable(bool cancellable)
{
  BaseProgressCallback::SetCancellable(cancellable);
  Redraw(false);
}

void ConsoleProgressCallback::SetTitle(const char* title)
{
  Clear();
  std::fprintf(stdout, "== %s ==\n", title);
  Redraw(false);
}

void ConsoleProgressCallback::SetStatusText(const char* text)
{
  BaseProgressCallback::SetStatusText(text);
  Redraw(false);
}

void ConsoleProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  BaseProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void ConsoleProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  BaseProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(true);
}

void ConsoleProgressCallback::Clear()
{
  SmallString message;
  for (u32 i = 0; i < COLUMNS; i++)
    message.AppendCharacter(' ');
  message.AppendCharacter('\r');

  std::fwrite(message.GetCharArray(), message.GetLength(), 1, stderr);
  std::fflush(stderr);
}

void ConsoleProgressCallback::Redraw(bool update_value_only)
{
  float percent_complete = (m_progress_range > 0) ? ((float)m_progress_value / (float)m_progress_range) * 100.0f : 0.0f;
  if (percent_complete > 100.0f)
    percent_complete = 100.0f;

  const u32 current_length = m_status_text.GetLength() + 14;
  const u32 max_bar_length = (current_length < COLUMNS) ? COLUMNS - current_length : 0;
  const u32 current_bar_length =
    (max_bar_length > 0) ? (static_cast<u32>(percent_complete / 100.0f * (float)max_bar_length)) : 0;

  if (update_value_only && (current_bar_length == m_last_bar_length) &&
      std::abs(percent_complete - m_last_percent_complete) < 0.01f)
  {
    return;
  }

  m_last_bar_length = current_bar_length;
  m_last_percent_complete = percent_complete;

  SmallString message;
  message.AppendString(m_status_text);
  message.AppendFormattedString(" [%.2f%%]", percent_complete);

  if (max_bar_length > 0)
  {
    message.AppendString(" |");

    u32 i;
    for (i = 0; i < current_bar_length; i++)
      message.AppendCharacter('=');
    for (; i < max_bar_length; i++)
      message.AppendCharacter(' ');

    message.AppendString("|");
  }

  message.AppendCharacter('\r');

  std::fwrite(message.GetCharArray(), message.GetLength(), 1, stderr);
  std::fflush(stderr);
}

void ConsoleProgressCallback::DisplayError(const char* message)
{
  Clear();
  Log_ErrorPrint(message);
  Redraw(false);
}

void ConsoleProgressCallback::DisplayWarning(const char* message)
{
  Clear();
  Log_WarningPrint(message);
  Redraw(false);
}

void ConsoleProgressCallback::DisplayInformation(const char* message)
{
  Clear();
  Log_InfoPrint(message);
  Redraw(false);
}

void ConsoleProgressCallback::DisplayDebugMessage(const char* message)
{
  Clear();
  Log_DevPrint(message);
  Redraw(false);
}

void ConsoleProgressCallback::ModalError(const char* message)
{
  Clear();
  Log_ErrorPrint(message);
  Redraw(false);
}

bool ConsoleProgressCallback::ModalConfirmation(const char* message)
{
  Clear();
  Log_InfoPrint(message);
  Redraw(false);
  return false;
}

void ConsoleProgressCallback::ModalInformation(const char* message)
{
  Clear();
  Log_InfoPrint(message);
  Redraw(false);
}
