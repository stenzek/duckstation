// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "qtprogresscallback.h"
#include "ui_coverdownloadwindow.h"

#include "common/timer.h"
#include "common/types.h"

#include <QtWidgets/QWidget>

#include <array>
#include <memory>
#include <string>

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
  void closeEvent(QCloseEvent* ev);

private Q_SLOTS:
  void onDownloadStatus(const QString& text);
  void onDownloadProgress(int value, int range);
  void onDownloadComplete();
  void onStartClicked();
  void onCloseClicked();
  void updateEnabled();

private:
  class CoverDownloadThread : public QtAsyncProgressThread
  {
  public:
    CoverDownloadThread(QWidget* parent, const QString& urls, bool use_serials);
    ~CoverDownloadThread();

  protected:
    void runAsync() override;

  private:
    std::vector<std::string> m_urls;
    bool m_use_serials;
  };

  void startThread();
  void cancelThread();

  Ui::CoverDownloadWindow m_ui;
  std::unique_ptr<CoverDownloadThread> m_thread;
  Timer m_last_refresh_time;
};
