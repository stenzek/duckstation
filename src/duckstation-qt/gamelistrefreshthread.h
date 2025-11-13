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

private:
  void PushState() override;
  void PopState() override;

  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

  void fireUpdate();

  Timer m_start_time;
  QString m_status_text;
  int m_last_range = 1;
  int m_last_value = 0;
  bool m_invalidate_cache;
};
