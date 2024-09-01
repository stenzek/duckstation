// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtCore/QSemaphore>
#include <QtCore/QThread>

#include "common/progress_callback.h"
#include "common/timer.h"

class GameListRefreshThread;

class AsyncRefreshProgressCallback : public ProgressCallback
{
public:
  AsyncRefreshProgressCallback(GameListRefreshThread* parent);

  float timeSinceStart() const;

  void Cancel();

  void PushState() override;
  void PopState() override;

  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

private:
  void fireUpdate();

  GameListRefreshThread* m_parent;
  Common::Timer m_start_time;
  QString m_status_text;
  int m_last_range = 1;
  int m_last_value = 0;
};

class GameListRefreshThread final : public QThread
{
  Q_OBJECT

public:
  GameListRefreshThread(bool invalidate_cache);
  ~GameListRefreshThread();

  float timeSinceStart() const;

  void cancel();

Q_SIGNALS:
  void refreshProgress(const QString& status, int current, int total, float time);
  void refreshComplete();

protected:
  void run();

private:
  AsyncRefreshProgressCallback m_progress;
  bool m_invalidate_cache;
};
