// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "progress_callback.h"
#include "assert.h"
#include "log.h"

#include <cmath>
#include <cstdio>
#include <limits>

Log_SetChannel(ProgressCallback);

static ProgressCallback s_nullProgressCallbacks;
ProgressCallback* ProgressCallback::NullProgressCallback = &s_nullProgressCallbacks;

ProgressCallback::~ProgressCallback()
{
  std::unique_ptr<State> pNextState = std::move(m_saved_state);
  while (pNextState)
    pNextState = std::move(pNextState->next_saved_state);
}

void ProgressCallback::PushState()
{
  std::unique_ptr<State> pNewState = std::make_unique<State>();
  pNewState->cancellable = m_cancellable;
  pNewState->status_text = m_status_text;
  pNewState->progress_range = m_progress_range;
  pNewState->progress_value = m_progress_value;
  pNewState->base_progress_value = m_base_progress_value;
  pNewState->next_saved_state = std::move(m_saved_state);
  m_saved_state = std::move(pNewState);
}

void ProgressCallback::PopState()
{
  DebugAssert(m_saved_state);

  // impose the current position into the previous range
  const u32 new_progress_value =
    (m_progress_range != 0) ?
      static_cast<u32>(((float)m_progress_value / (float)m_progress_range) * (float)m_saved_state->progress_range) :
      m_saved_state->progress_value;

  m_cancellable = m_saved_state->cancellable;
  m_status_text = std::move(m_saved_state->status_text);
  m_progress_range = m_saved_state->progress_range;
  m_progress_value = new_progress_value;

  m_base_progress_value = m_saved_state->base_progress_value;
  m_saved_state = std::move(m_saved_state->next_saved_state);
}

bool ProgressCallback::IsCancellable() const
{
  return m_cancellable;
}

bool ProgressCallback::IsCancelled() const
{
  return m_cancelled;
}

void ProgressCallback::SetTitle(const std::string_view title)
{
}

void ProgressCallback::SetStatusText(const std::string_view text)
{
  m_status_text.assign(text);
}

void ProgressCallback::SetCancellable(bool cancellable)
{
  m_cancellable = cancellable;
}

void ProgressCallback::SetProgressValue(u32 value)
{
  m_progress_value = m_base_progress_value + value;
}

void ProgressCallback::SetProgressRange(u32 range)
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

void ProgressCallback::IncrementProgressValue()
{
  SetProgressValue((m_progress_value - m_base_progress_value) + 1);
}

void ProgressCallback::DisplayError(const std::string_view message)
{
  ERROR_LOG(message);
}

void ProgressCallback::DisplayWarning(const std::string_view message)
{
  WARNING_LOG(message);
}

void ProgressCallback::DisplayInformation(const std::string_view message)
{
  INFO_LOG(message);
}

void ProgressCallback::DisplayDebugMessage(const std::string_view message)
{
  DEV_LOG(message);
}

void ProgressCallback::ModalError(const std::string_view message)
{
  ERROR_LOG(message);
}

bool ProgressCallback::ModalConfirmation(const std::string_view message)
{
  INFO_LOG(message);
  return false;
}

void ProgressCallback::ModalInformation(const std::string_view message)
{
  INFO_LOG(message);
}
