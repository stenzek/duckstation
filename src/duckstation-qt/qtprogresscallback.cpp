#include "qtprogresscallback.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtWidgets/QMessageBox>
#include <array>

QtProgressCallback::QtProgressCallback(QWidget* parent_widget, float show_delay)
  : QObject(parent_widget), m_dialog(QString(), QString(), 0, 1, parent_widget), m_show_delay(show_delay)
{
  m_dialog.setWindowTitle(tr("DuckStation"));
  m_dialog.setMinimumSize(QSize(500, 0));
  m_dialog.setModal(parent_widget != nullptr);
  m_dialog.setAutoClose(false);
  m_dialog.setAutoReset(false);
  checkForDelayedShow();
}

QtProgressCallback::~QtProgressCallback() = default;

bool QtProgressCallback::IsCancelled() const
{
  return m_dialog.wasCanceled();
}

void QtProgressCallback::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  BaseProgressCallback::SetCancellable(cancellable);
  m_dialog.setCancelButtonText(cancellable ? tr("Cancel") : QString());
}

void QtProgressCallback::SetTitle(const char* title)
{
  m_dialog.setWindowTitle(QString::fromUtf8(title));
}

void QtProgressCallback::SetStatusText(const char* text)
{
  BaseProgressCallback::SetStatusText(text);
  checkForDelayedShow();

  if (m_dialog.isVisible())
    m_dialog.setLabelText(QString::fromUtf8(text));
}

void QtProgressCallback::SetProgressRange(u32 range)
{
  BaseProgressCallback::SetProgressRange(range);
  checkForDelayedShow();

  if (m_dialog.isVisible())
    m_dialog.setRange(0, m_progress_range);
}

void QtProgressCallback::SetProgressValue(u32 value)
{
  BaseProgressCallback::SetProgressValue(value);
  checkForDelayedShow();

  if (m_dialog.isVisible() && static_cast<u32>(m_dialog.value()) != m_progress_range)
    m_dialog.setValue(m_progress_value);

  QCoreApplication::processEvents();
}

void QtProgressCallback::DisplayError(const char* message)
{
  qWarning() << message;
}

void QtProgressCallback::DisplayWarning(const char* message)
{
  qWarning() << message;
}

void QtProgressCallback::DisplayInformation(const char* message)
{
  qWarning() << message;
}

void QtProgressCallback::DisplayDebugMessage(const char* message)
{
  qWarning() << message;
}

void QtProgressCallback::ModalError(const char* message)
{
  QMessageBox::critical(&m_dialog, tr("Error"), QString::fromUtf8(message));
}

bool QtProgressCallback::ModalConfirmation(const char* message)
{
  return (QMessageBox::question(&m_dialog, tr("Question"), QString::fromUtf8(message), QMessageBox::Yes,
                                QMessageBox::No) == QMessageBox::Yes);
}

void QtProgressCallback::ModalInformation(const char* message)
{
  QMessageBox::information(&m_dialog, tr("Information"), QString::fromUtf8(message));
}

void QtProgressCallback::checkForDelayedShow()
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
