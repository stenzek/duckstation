// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistrefreshthread.h"
#include "qtutils.h"

#include "core/game_list.h"

#include "common/log.h"
#include "common/progress_callback.h"
#include "common/timer.h"

#include <QtWidgets/QMessageBox>

AsyncRefreshProgressCallback::AsyncRefreshProgressCallback(GameListRefreshThread* parent) : m_parent(parent)
{
}

float AsyncRefreshProgressCallback::timeSinceStart() const
{
  return m_start_time.GetTimeSeconds();
}

void AsyncRefreshProgressCallback::Cancel()
{
  // Not atomic, but we don't need to cancel immediately.
  m_cancelled = true;
}

void AsyncRefreshProgressCallback::PushState()
{
  ProgressCallback::PushState();
}

void AsyncRefreshProgressCallback::PopState()
{
  ProgressCallback::PopState();

  if (static_cast<int>(m_progress_range) == m_last_range && static_cast<int>(m_progress_value) == m_last_value)
    return;

  m_last_range = static_cast<int>(m_progress_range);
  m_last_value = static_cast<int>(m_progress_value);
  fireUpdate();
}

void AsyncRefreshProgressCallback::SetStatusText(const std::string_view text)
{
  const QString new_text = QtUtils::StringViewToQString(text);
  if (new_text == m_status_text)
    return;

  m_status_text = new_text;
  fireUpdate();
}

void AsyncRefreshProgressCallback::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);
  if (static_cast<int>(m_progress_range) == m_last_range)
    return;

  m_last_range = static_cast<int>(m_progress_range);
  fireUpdate();
}

void AsyncRefreshProgressCallback::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);
  if (static_cast<int>(m_progress_value) == m_last_value)
    return;

  m_last_value = static_cast<int>(m_progress_value);
  fireUpdate();
}

void AsyncRefreshProgressCallback::ModalError(const std::string_view message)
{
  QMessageBox::critical(nullptr, QStringLiteral("Error"), QtUtils::StringViewToQString(message));
}

bool AsyncRefreshProgressCallback::ModalConfirmation(const std::string_view message)
{
  return QMessageBox::question(nullptr, QStringLiteral("Question"), QtUtils::StringViewToQString(message)) ==
         QMessageBox::Yes;
}

void AsyncRefreshProgressCallback::ModalInformation(const std::string_view message)
{
  QMessageBox::information(nullptr, QStringLiteral("Information"), QtUtils::StringViewToQString(message));
}

void AsyncRefreshProgressCallback::fireUpdate()
{
  m_parent->refreshProgress(m_status_text, m_last_value, m_last_range, m_start_time.GetTimeSeconds());
}

GameListRefreshThread::GameListRefreshThread(bool invalidate_cache)
  : QThread(), m_progress(this), m_invalidate_cache(invalidate_cache)
{
}

GameListRefreshThread::~GameListRefreshThread() = default;

float GameListRefreshThread::timeSinceStart() const
{
  return m_progress.timeSinceStart();
}

void GameListRefreshThread::cancel()
{
  m_progress.Cancel();
}

void GameListRefreshThread::run()
{
  GameList::Refresh(m_invalidate_cache, false, &m_progress);
  emit refreshComplete();
}
