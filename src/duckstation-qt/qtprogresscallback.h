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
public:
  explicit QtModalProgressCallback(QWidget* parent_widget, float show_delay = 0.0f);
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

  void MakeVisible();

private:
  static constexpr int MINIMUM_WIDTH = 500;
  static constexpr int MINIMUM_HEIGHT_WITHOUT_CANCEL = 70;
  static constexpr int MINIMUM_HEIGHT_WITH_CANCEL = 100;

  void checkForDelayedShow();

  void dialogCancelled();

  QProgressDialog m_dialog;
  Timer m_show_timer;
  float m_show_delay;
};

class QtAsyncProgressThread : public QThread, public ProgressCallback
{
  Q_OBJECT

public:
  explicit QtAsyncProgressThread(QWidget* parent);
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

  void start();
  void join();

Q_SIGNALS:
  void titleUpdated(const QString& title);
  void statusUpdated(const QString& status);
  void progressUpdated(int value, int range);
  void threadStarting();
  void threadFinished();

protected:
  virtual void runAsync() = 0;
  void run() final;

private:
  QWidget* parentWidget() const;

  QSemaphore m_start_semaphore;
  QThread* m_starting_thread = nullptr;
};
