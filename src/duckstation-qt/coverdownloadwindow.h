// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_coverdownloadwindow.h"

#include "common/error.h"
#include "common/progress_callback.h"
#include "common/timer.h"
#include "common/types.h"

#include <QtWidgets/QWidget>

#include <array>
#include <memory>
#include <string>

class CoverDownloadThread;

class CoverDownloadWindow final : public QWidget
{
  Q_OBJECT

public:
  CoverDownloadWindow();
  ~CoverDownloadWindow();

Q_SIGNALS:
  void closed();
  void coverRefreshRequested();

protected:
  void closeEvent(QCloseEvent* ev) override;

private:
  void startThread();
  void cancelThread();

  void onDownloadStatus(const QString& text);
  void onDownloadProgress(int value, int range);
  void onDownloadComplete();
  void onStartClicked();
  void onCloseClicked();
  void updateEnabled();

  Ui::CoverDownloadWindow m_ui;
  CoverDownloadThread* m_thread = nullptr;
  Timer m_last_refresh_time;
};

class CoverDownloadThread final : public QThread, private ProgressCallback
{
  Q_OBJECT

public:
  CoverDownloadThread(const QString& urls, bool use_serials);
  ~CoverDownloadThread();

  ALWAYS_INLINE const Error& getError() const { return m_error; }
  ALWAYS_INLINE bool getResult() const { return m_result; }

Q_SIGNALS:
  void titleUpdated(const QString& title);
  void statusUpdated(const QString& status);
  void progressUpdated(int value, int range);
  void threadFinished();

protected:
  void run() override;

  bool IsCancelled() const override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

private:
  std::vector<std::string> m_urls;
  Error m_error;
  bool m_use_serials = false;
  bool m_result = false;
};