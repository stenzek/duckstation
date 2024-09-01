// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/progress_callback.h"
#include "common/timer.h"

#include <QtCore/QSemaphore>
#include <QtCore/QThread>
#include <QtWidgets/QProgressDialog>
#include <atomic>

class QtModalProgressCallback final : public QObject, public ProgressCallback
{
  Q_OBJECT

public:
  QtModalProgressCallback(QWidget* parent_widget, float show_delay = 0.0f);
  ~QtModalProgressCallback();

  QProgressDialog& GetDialog() { return m_dialog; }

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

private Q_SLOTS:
  void dialogCancelled();

private:
  void checkForDelayedShow();

  QProgressDialog m_dialog;
  Common::Timer m_show_timer;
  float m_show_delay;
};

class QtAsyncProgressThread : public QThread, public ProgressCallback
{
  Q_OBJECT

public:
  QtAsyncProgressThread(QWidget* parent);
  ~QtAsyncProgressThread();

  bool IsCancelled() const override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

Q_SIGNALS:
  void titleUpdated(const QString& title);
  void statusUpdated(const QString& status);
  void progressUpdated(int value, int range);
  void threadStarting();
  void threadFinished();

public Q_SLOTS:
  void start();
  void join();

protected:
  virtual void runAsync() = 0;
  void run() final;

private:
  QWidget* parentWidget() const;

  QSemaphore m_start_semaphore;
  QThread* m_starting_thread = nullptr;
};
