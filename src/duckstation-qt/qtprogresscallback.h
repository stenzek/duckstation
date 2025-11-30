// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/progress_callback.h"
#include "common/timer.h"

#include <QtWidgets/QDialog>
#include <atomic>

class QAbstractButton;
class QDialogButtonBox;
class QLabel;
class QProgressBar;
class QPushButton;

class QtProgressCallback final : public QObject, public ProgressCallback
{
  Q_OBJECT

public:
  explicit QtProgressCallback(QObject* parent = nullptr);
  ~QtProgressCallback() override;

  bool IsCancelled() const override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void connectWidgets(QLabel* const status_label, QProgressBar* const progress_bar,
                      QAbstractButton* const cancel_button);

Q_SIGNALS:
  void titleUpdated(const QString& title);
  void statusTextUpdated(const QString& status);
  void progressRangeUpdated(int min, int max);
  void progressValueUpdated(int value);

private:
  std::atomic_bool m_ts_cancelled{false};
};

class QtAsyncTaskWithProgress final : public QObject, private ProgressCallback
{
  Q_OBJECT

public:
  using CompletionCallback = std::function<void()>;
  using WorkCallback = std::function<CompletionCallback(ProgressCallback*)>;

  ~QtAsyncTaskWithProgress() override;

  /// Creates a task, the task is still in the pending state until start() is called.
  static QtAsyncTaskWithProgress* create(QWidget* const callback_parent, WorkCallback callback);

  /// Starts the task asynchronously. The pointer is only guaranteed to be valid until the
  /// completion callback has executed.
  void start();

  /// Cancel the task asynchronously. If the callback_parent that was set in start() is no
  /// longer valid, the completion handler will not execute.
  void cancel();

  /// Connects progress updates to a typical set of widgets.
  void connectWidgets(QLabel* const status_label, QProgressBar* const progress_bar,
                      QAbstractButton* const cancel_button);

Q_SIGNALS:
  void titleUpdated(const QString& title);
  void statusTextUpdated(const QString& status);
  void progressRangeUpdated(int min, int max);
  void progressValueUpdated(int value);
  void completed();

protected:
  bool IsCancelled() const override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

private:
  QtAsyncTaskWithProgress();

  std::variant<WorkCallback, CompletionCallback> m_callback;
  std::atomic_bool m_ts_cancelled{false};
};

class QtAsyncTaskWithProgressDialog final : public QObject, private ProgressCallback
{
  Q_OBJECT

public:
  using CompletionCallback = std::function<void()>;
  using WorkCallback = std::function<CompletionCallback(ProgressCallback*)>;

  static QtAsyncTaskWithProgressDialog* create(QWidget* parent, std::string_view initial_title,
                                               std::string_view initial_status_text, bool cancellable, int range,
                                               int value, float show_delay, WorkCallback callback);
  static QtAsyncTaskWithProgressDialog* create(QWidget* parent, float show_delay, WorkCallback callback);

  /// Asynchronously cancel the task. Should only be called from the UI thread.
  /// There is no guarantee when the cancel will go through.
  void cancel();

Q_SIGNALS:
  void completed(QtAsyncTaskWithProgressDialog* self);

private:
  // can't use QProgressDialog, it starts an event in setValue()...
  class ProgressDialog final : public QDialog
  {
    friend QtAsyncTaskWithProgressDialog;

  public:
    ProgressDialog(const QString& initial_title, const QString& initial_status_text, bool cancellable, int range,
                   int value, QtAsyncTaskWithProgressDialog& task, QWidget* parent);
    ~ProgressDialog() override;

    void setCancellable(bool cancellable);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    static constexpr int MINIMUM_WIDTH = 500;
    static constexpr int MINIMUM_HEIGHT_WITHOUT_CANCEL = 70;
    static constexpr int MINIMUM_HEIGHT_WITH_CANCEL = 100;

    void cancelled();

    QtAsyncTaskWithProgressDialog& m_task;
    QLabel* m_status_label = nullptr;
    QProgressBar* m_progress_bar = nullptr;
    QDialogButtonBox* m_button_box = nullptr;
  };

  friend ProgressDialog;

  // constructor hidden, clients should not be creating this directly
  QtAsyncTaskWithProgressDialog(const QString& initial_title, const QString& initial_status_text, bool cancellable,
                                int range, int value, float show_delay, QWidget* dialog_parent, WorkCallback callback);
  ~QtAsyncTaskWithProgressDialog();

  // progress callback overrides
  bool IsCancelled() const override;
  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void CheckForDelayedShow();

  std::variant<WorkCallback, CompletionCallback> m_callback;
  ProgressDialog* m_dialog = nullptr;

  Timer m_show_timer;
  float m_show_delay;
  std::atomic_bool m_ts_cancelled{false};
  bool m_shown = false;
};
