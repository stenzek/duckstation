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

  static QtAsyncTaskWithProgress* create(QWidget* parent, std::string_view initial_title,
                                         std::string_view initial_status_text, bool cancellable, int range, int value,
                                         float show_delay, WorkCallback callback);
  static QtAsyncTaskWithProgress* create(QWidget* parent, float show_delay, WorkCallback callback);

  /// Asynchronously cancel the task. Should only be called from the UI thread.
  /// There is no guarantee when the cancel will go through.
  void cancel();

Q_SIGNALS:
  void completed(QtAsyncTaskWithProgress* self);

private:
  // can't use QProgressDialog, it starts an event in setValue()...
  class ProgressDialog final : public QDialog
  {
    friend QtAsyncTaskWithProgress;

  public:
    ProgressDialog(const QString& initial_title, const QString& initial_status_text, bool cancellable, int range,
                   int value, QtAsyncTaskWithProgress& task, QWidget* parent);
    ~ProgressDialog() override;

    void setCancellable(bool cancellable);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    static constexpr int MINIMUM_WIDTH = 500;
    static constexpr int MINIMUM_HEIGHT_WITHOUT_CANCEL = 70;
    static constexpr int MINIMUM_HEIGHT_WITH_CANCEL = 100;

    void cancelled();

    QtAsyncTaskWithProgress& m_task;
    QLabel* m_status_label = nullptr;
    QProgressBar* m_progress_bar = nullptr;
    QDialogButtonBox* m_button_box = nullptr;
  };

  friend ProgressDialog;

  // constructor hidden, clients should not be creating this directly
  QtAsyncTaskWithProgress(const QString& initial_title, const QString& initial_status_text, bool cancellable, int range,
                          int value, float show_delay, QWidget* dialog_parent, WorkCallback callback);
  ~QtAsyncTaskWithProgress();

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
