// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistrefreshthread.h"
#include "qtutils.h"

#include "core/game_list.h"

#include "common/log.h"
#include "common/progress_callback.h"
#include "common/timer.h"

#include "moc_gamelistrefreshthread.cpp"

GameListRefreshThread::GameListRefreshThread(bool invalidate_cache) : QThread(), m_invalidate_cache(invalidate_cache)
{
}

GameListRefreshThread::~GameListRefreshThread() = default;

void GameListRefreshThread::cancel()
{
  // Not atomic, but we don't need to cancel immediately.
  m_cancelled = true;
}

void GameListRefreshThread::run()
{
  GameList::Refresh(m_invalidate_cache, false, this);
  emit refreshComplete();
}

void GameListRefreshThread::PushState()
{
  ProgressCallback::PushState();
}

void GameListRefreshThread::PopState()
{
  ProgressCallback::PopState();

  if (static_cast<int>(m_progress_range) == m_last_range && static_cast<int>(m_progress_value) == m_last_value)
    return;

  m_last_range = static_cast<int>(m_progress_range);
  m_last_value = static_cast<int>(m_progress_value);
  fireUpdate();
}

void GameListRefreshThread::SetStatusText(const std::string_view text)
{
  const QString new_text = QtUtils::StringViewToQString(text);
  if (new_text == m_status_text)
    return;

  m_status_text = new_text;
  fireUpdate();
}

void GameListRefreshThread::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);
  if (static_cast<int>(m_progress_range) == m_last_range)
    return;

  m_last_range = static_cast<int>(m_progress_range);
  fireUpdate();
}

void GameListRefreshThread::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);
  if (static_cast<int>(m_progress_value) == m_last_value)
    return;

  m_last_value = static_cast<int>(m_progress_value);
  fireUpdate();
}

void GameListRefreshThread::ModalError(const std::string_view message)
{
  QtUtils::MessageBoxCritical(nullptr, QStringLiteral("Error"), QtUtils::StringViewToQString(message));
}

bool GameListRefreshThread::ModalConfirmation(const std::string_view message)
{
  return QtUtils::MessageBoxQuestion(nullptr, QStringLiteral("Question"), QtUtils::StringViewToQString(message)) ==
         QMessageBox::Yes;
}

void GameListRefreshThread::ModalInformation(const std::string_view message)
{
  QtUtils::MessageBoxInformation(nullptr, QStringLiteral("Information"), QtUtils::StringViewToQString(message));
}

void GameListRefreshThread::fireUpdate()
{
  emit refreshProgress(m_status_text, m_last_value, m_last_range, static_cast<int>(GameList::GetEntryCount()),
                       static_cast<float>(m_start_time.GetTimeSeconds()));
}
