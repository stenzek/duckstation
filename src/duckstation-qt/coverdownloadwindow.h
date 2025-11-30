// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_coverdownloadwindow.h"

#include "common/timer.h"
#include "common/types.h"

#include <QtWidgets/QWidget>

#include <array>
#include <memory>
#include <string>

class Error;

class QtAsyncTaskWithProgress;

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
  void onStartClicked();
  void downloadComplete(bool result, const Error& error);
  void updateEnabled();

  Ui::CoverDownloadWindow m_ui;
  QtAsyncTaskWithProgress* m_task = nullptr;
  Timer m_last_refresh_time;
};
