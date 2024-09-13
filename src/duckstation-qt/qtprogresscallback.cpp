// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtprogresscallback.h"
#include "qtutils.h"

#include "common/assert.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtWidgets/QMessageBox>
#include <array>

QtModalProgressCallback::QtModalProgressCallback(QWidget* parent_widget, float show_delay)
  : QObject(parent_widget), m_dialog(QString(), QString(), 0, 1, parent_widget), m_show_delay(show_delay)
{
  m_dialog.setWindowTitle(tr("DuckStation"));
  m_dialog.setMinimumSize(QSize(500, 0));
  m_dialog.setModal(parent_widget != nullptr);
  m_dialog.setAutoClose(false);
  m_dialog.setAutoReset(false);
  connect(&m_dialog, &QProgressDialog::canceled, this, &QtModalProgressCallback::dialogCancelled);
  checkForDelayedShow();
}

QtModalProgressCallback::~QtModalProgressCallback() = default;

void QtModalProgressCallback::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  ProgressCallback::SetCancellable(cancellable);
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

  if (m_dialog.isVisible())
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

void QtModalProgressCallback::ModalError(const std::string_view message)
{
  QMessageBox::critical(&m_dialog, tr("Error"), QtUtils::StringViewToQString(message));
}

bool QtModalProgressCallback::ModalConfirmation(const std::string_view message)
{
  return (QMessageBox::question(&m_dialog, tr("Question"), QtUtils::StringViewToQString(message), QMessageBox::Yes,
                                QMessageBox::No) == QMessageBox::Yes);
}

void QtModalProgressCallback::ModalInformation(const std::string_view message)
{
  QMessageBox::information(&m_dialog, tr("Information"), QtUtils::StringViewToQString(message));
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
  {
    m_dialog.setRange(0, m_progress_range);
    m_dialog.setValue(m_progress_value);
    m_dialog.show();
  }
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

void QtAsyncProgressThread::ModalError(const std::string_view message)
{
  QMessageBox::critical(parentWidget(), tr("Error"), QtUtils::StringViewToQString(message));
}

bool QtAsyncProgressThread::ModalConfirmation(const std::string_view message)
{
  return (QMessageBox::question(parentWidget(), tr("Question"), QtUtils::StringViewToQString(message), QMessageBox::Yes,
                                QMessageBox::No) == QMessageBox::Yes);
}

void QtAsyncProgressThread::ModalInformation(const std::string_view message)
{
  QMessageBox::information(parentWidget(), tr("Information"), QtUtils::StringViewToQString(message));
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
