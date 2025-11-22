// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtprogresscallback.h"
#include "qtutils.h"

#include "common/assert.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <array>

#include "moc_qtprogresscallback.cpp"

QtModalProgressCallback::QtModalProgressCallback(QWidget* parent_widget, float show_delay)
  : QObject(parent_widget), m_dialog(QString(), QString(), 0, 1, parent_widget), m_show_delay(show_delay)
{
  m_dialog.setWindowTitle(tr("DuckStation"));
  m_dialog.setMinimumSize(MINIMUM_WIDTH, MINIMUM_HEIGHT_WITHOUT_CANCEL);
  if (parent_widget)
  {
    m_dialog.setWindowModality(Qt::WindowModal);
  }
  else
  {
    m_dialog.setModal(true);
    m_dialog.setWindowModality(Qt::ApplicationModal);
  }
  m_dialog.setAutoClose(false);
  m_dialog.setAutoReset(false);
  m_dialog.setWindowFlag(Qt::CustomizeWindowHint, true);
  m_dialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
  connect(&m_dialog, &QProgressDialog::canceled, this, &QtModalProgressCallback::dialogCancelled);
  checkForDelayedShow();
}

QtModalProgressCallback::~QtModalProgressCallback() = default;

void QtModalProgressCallback::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  ProgressCallback::SetCancellable(cancellable);
  m_dialog.setWindowFlag(Qt::WindowCloseButtonHint, cancellable);
  m_dialog.setMinimumHeight(cancellable ? MINIMUM_HEIGHT_WITH_CANCEL : MINIMUM_HEIGHT_WITHOUT_CANCEL);
  m_dialog.setCancelButtonText(cancellable ? tr("Cancel") : QString());
}

void QtModalProgressCallback::SetTitle(const std::string_view title)
{
  m_dialog.setWindowTitle(QtUtils::StringViewToQString(title));
}

void QtModalProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  checkForDelayedShow();
  m_dialog.setLabelText(QtUtils::StringViewToQString(text));
}

void QtModalProgressCallback::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);
  checkForDelayedShow();

  if (m_dialog.isVisible())
    m_dialog.setRange(0, m_progress_range);
}

void QtModalProgressCallback::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);
  checkForDelayedShow();

  if (m_dialog.isVisible() && static_cast<u32>(m_dialog.value()) != m_progress_range)
    m_dialog.setValue(m_progress_value);

  QCoreApplication::processEvents();
}

void QtModalProgressCallback::dialogCancelled()
{
  m_cancelled = true;
}

void QtModalProgressCallback::checkForDelayedShow()
{
  if (m_dialog.isVisible())
    return;

  if (m_show_timer.GetTimeSeconds() >= m_show_delay)
    MakeVisible();
}
void QtModalProgressCallback::MakeVisible()
{
  m_dialog.setRange(0, m_progress_range);
  m_dialog.setValue(m_progress_value);
  if (m_dialog.parent())
    m_dialog.open();
  else
    m_dialog.show();
}

// NOTE: We deliberately don't set the thread parent, because otherwise we can't move it.
QtAsyncProgressThread::QtAsyncProgressThread(QWidget* parent) : QThread()
{
}

QtAsyncProgressThread::~QtAsyncProgressThread() = default;

bool QtAsyncProgressThread::IsCancelled() const
{
  return isInterruptionRequested();
}

void QtAsyncProgressThread::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  ProgressCallback::SetCancellable(cancellable);
}

void QtAsyncProgressThread::SetTitle(const std::string_view title)
{
  emit titleUpdated(QtUtils::StringViewToQString(title));
}

void QtAsyncProgressThread::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  emit statusUpdated(QtUtils::StringViewToQString(text));
}

void QtAsyncProgressThread::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);
  emit progressUpdated(static_cast<int>(m_progress_value), static_cast<int>(m_progress_range));
}

void QtAsyncProgressThread::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);
  emit progressUpdated(static_cast<int>(m_progress_value), static_cast<int>(m_progress_range));
}

void QtAsyncProgressThread::start()
{
  Assert(!isRunning());

  QThread::start();
  moveToThread(this);
  m_starting_thread = QThread::currentThread();
  m_start_semaphore.release();
}

void QtAsyncProgressThread::join()
{
  if (isRunning())
    QThread::wait();
}

void QtAsyncProgressThread::run()
{
  m_start_semaphore.acquire();
  emit threadStarting();
  runAsync();
  emit threadFinished();
  moveToThread(m_starting_thread);
}

QWidget* QtAsyncProgressThread::parentWidget() const
{
  return qobject_cast<QWidget*>(parent());
}

QtAsyncTaskWithProgress::QtAsyncTaskWithProgress(const QString& initial_title, const QString& initial_status_text,
                                                 bool cancellable, int range, int value, float show_delay,
                                                 QWidget* dialog_parent, WorkCallback callback)
  : m_callback(std::move(callback)), m_show_delay(show_delay)
{
  m_dialog = new ProgressDialog(initial_title, initial_status_text, cancellable, range, value, *this, dialog_parent);

  if (show_delay <= 0.0f)
  {
    m_shown = true;
    m_dialog->open();
  }
}

QtAsyncTaskWithProgress::~QtAsyncTaskWithProgress()
{
  if (m_dialog)
  {
    // should null out itself
    delete m_dialog;
    DebugAssert(!m_dialog);
  }
}

QtAsyncTaskWithProgress::ProgressDialog::ProgressDialog(const QString& initial_title,
                                                        const QString& initial_status_text, bool cancellable, int range,
                                                        int value, QtAsyncTaskWithProgress& task, QWidget* parent)
  : QDialog(parent), m_task(task)
{
  if (!initial_title.isEmpty())
    setWindowTitle(initial_title);
  else
    setWindowTitle(QStringLiteral("DuckStation"));

  setWindowFlag(Qt::CustomizeWindowHint, true);
  setWindowFlag(Qt::WindowCloseButtonHint, cancellable);
  setWindowModality(Qt::WindowModal);
  setMinimumSize(MINIMUM_WIDTH, cancellable ? MINIMUM_HEIGHT_WITH_CANCEL : MINIMUM_HEIGHT_WITHOUT_CANCEL);

  m_progress_bar = new QProgressBar(this);
  m_progress_bar->setRange(0, range);
  m_progress_bar->setValue(value);

  m_status_label = new QLabel(this);
  m_status_label->setAlignment(Qt::AlignCenter);
  if (!initial_status_text.isEmpty())
    m_status_label->setText(initial_status_text);

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::close);
  m_button_box->setVisible(cancellable);

  QVBoxLayout* const layout = new QVBoxLayout(this);
  layout->setSpacing(8);
  layout->addWidget(m_status_label);
  layout->addWidget(m_progress_bar);
  layout->addWidget(m_button_box);
}

QtAsyncTaskWithProgress::ProgressDialog::~ProgressDialog()
{
  DebugAssert(m_task.m_dialog == this);
  m_task.m_dialog = nullptr;
}

void QtAsyncTaskWithProgress::ProgressDialog::setCancellable(bool cancellable)
{
  if (cancellable == m_button_box->isVisible())
    return;

  setWindowFlag(Qt::WindowCloseButtonHint, cancellable);
  setMinimumHeight(cancellable ? MINIMUM_HEIGHT_WITH_CANCEL : MINIMUM_HEIGHT_WITHOUT_CANCEL);

  m_button_box->setVisible(cancellable);
}

void QtAsyncTaskWithProgress::ProgressDialog::closeEvent(QCloseEvent* event)
{
  cancelled();
  QDialog::closeEvent(event);
}

void QtAsyncTaskWithProgress::ProgressDialog::cancelled()
{
  m_task.m_ts_cancelled.store(true, std::memory_order_release);
}

void QtAsyncTaskWithProgress::create(QWidget* parent, std::string_view initial_title,
                                     std::string_view initial_status_text, bool cancellable, int range, int value,
                                     float show_delay, WorkCallback callback)
{
  DebugAssert(parent);

  // NOTE: Must get connected before queuing, because otherwise you risk a race.
  QtAsyncTaskWithProgress* task = new QtAsyncTaskWithProgress(
    QtUtils::StringViewToQString(initial_title), QtUtils::StringViewToQString(initial_status_text), cancellable, range,
    value, show_delay, parent, std::move(callback));
  connect(task, &QtAsyncTaskWithProgress::completed, parent,
          [task]() { std::get<CompletionCallback>(task->m_callback)(); });

  System::QueueAsyncTask([task]() {
    task->m_callback = std::get<WorkCallback>(task->m_callback)(task);
    Host::RunOnUIThread([task]() {
      emit task->completed(task);
      delete task;
    });
  });
}

void QtAsyncTaskWithProgress::create(QWidget* parent, float show_delay, WorkCallback callback)
{
  create(parent, {}, {}, false, 0, 1, show_delay, std::move(callback));
}

bool QtAsyncTaskWithProgress::IsCancelled() const
{
  return m_ts_cancelled.load(std::memory_order_acquire);
}

void QtAsyncTaskWithProgress::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  ProgressCallback::SetCancellable(cancellable);

  Host::RunOnUIThread([this, cancellable]() {
    if (m_dialog)
      m_dialog->setCancellable(cancellable);
  });
}

void QtAsyncTaskWithProgress::SetTitle(const std::string_view title)
{
  Host::RunOnUIThread([this, title = QtUtils::StringViewToQString(title)]() {
    if (m_dialog)
      m_dialog->setWindowTitle(title);
  });
}

void QtAsyncTaskWithProgress::SetStatusText(const std::string_view text)
{
  if (m_status_text == text)
    return;

  ProgressCallback::SetStatusText(text);
  if (m_shown)
  {
    Host::RunOnUIThread([this, text = QtUtils::StringViewToQString(text)]() {
      if (m_dialog)
        m_dialog->m_status_label->setText(text);
    });
  }
  else
  {
    CheckForDelayedShow();
  }
}

void QtAsyncTaskWithProgress::SetProgressRange(u32 range)
{
  const u32 prev_range = m_progress_range;
  ProgressCallback::SetProgressRange(range);
  if (m_progress_range == prev_range)
    return;

  if (m_shown)
  {
    Host::RunOnUIThread([this, range = static_cast<int>(m_progress_range)]() {
      if (m_dialog)
        m_dialog->m_progress_bar->setRange(0, range);
    });
  }
  else
  {
    CheckForDelayedShow();
  }
}

void QtAsyncTaskWithProgress::SetProgressValue(u32 value)
{
  const u32 prev_value = m_progress_value;
  ProgressCallback::SetProgressValue(value);
  if (m_progress_value == prev_value)
    return;

  if (m_shown)
  {
    Host::RunOnUIThread([this, value = static_cast<int>(m_progress_value)]() {
      if (m_dialog)
        m_dialog->m_progress_bar->setValue(value);
    });
  }
  else
  {
    CheckForDelayedShow();
  }
}

void QtAsyncTaskWithProgress::CheckForDelayedShow()
{
  DebugAssert(!m_shown);

  if (m_show_timer.GetTimeSeconds() < m_show_delay)
    return;

  m_shown = true;
  Host::RunOnUIThread([this, status_text = QtUtils::StringViewToQString(m_status_text),
                       range = static_cast<int>(m_progress_range), value = static_cast<int>(m_progress_value),
                       cancellable = m_cancellable]() {
    if (!m_dialog)
      return;

    if (!status_text.isEmpty())
      m_dialog->m_status_label->setText(status_text);

    m_dialog->m_progress_bar->setRange(0, range);
    m_dialog->m_progress_bar->setValue(value);
    m_dialog->setCancellable(cancellable);
    m_dialog->open();
  });
}
