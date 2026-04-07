// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtCore/QThread>

#include "common/progress_callback.h"
#include "common/timer.h"

class GameListRefreshThread final : public QThread, public ProgressCallback
{
  Q_OBJECT

public:
  explicit GameListRefreshThread(bool invalidate_cache);
  ~GameListRefreshThread();

  void cancel();

Q_SIGNALS:
  void refreshProgress(const QString& status, int current, int total, int entry_count, float time);
  void refreshComplete();

protected:
  void run() final;
  void StateChanged(StateChange changed) override;

private:
  Timer m_start_time;
  QString m_qstatus_text;
  bool m_invalidate_cache;
};
