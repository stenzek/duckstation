// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistrefreshthread.h"
#include "qtutils.h"

#include "core/game_list.h"

#include "common/log.h"
#include "common/progress_callback.h"
#include "common/timer.h"

#include "moc_gamelistrefreshthread.cpp"

LOG_CHANNEL(Host);

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

void GameListRefreshThread::StateChanged(StateChange changed)
{
  if (changed & STATE_CHANGE_STATUS_TEXT)
    m_qstatus_text = QtUtils::StringViewToQString(m_status_text);
  else if (!(changed & (STATE_CHANGE_PROGRESS | STATE_CHANGE_STATUS_TEXT)))
    return;

  emit refreshProgress(m_qstatus_text, static_cast<int>(m_progress_value), static_cast<int>(m_progress_range),
                       static_cast<int>(GameList::GetEntryCount()), static_cast<float>(m_start_time.GetTimeSeconds()));
}
