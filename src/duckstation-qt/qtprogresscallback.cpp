#include "qtprogresscallback.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtWidgets/QMessageBox>
#include <array>

QtProgressCallback::QtProgressCallback(QWidget* parent_widget)
  : QObject(parent_widget), m_dialog(QString(), QString(), 0, 1, parent_widget)
{
  m_dialog.setWindowTitle(tr("DuckStation"));
  m_dialog.setMinimumSize(QSize(500, 0));
  m_dialog.setModal(parent_widget != nullptr);
  m_dialog.show();
}

QtProgressCallback::~QtProgressCallback() = default;

bool QtProgressCallback::IsCancelled() const
{
  return m_dialog.wasCanceled();
}

void QtProgressCallback::SetCancellable(bool cancellable)
{
  BaseProgressCallback::SetCancellable(cancellable);
  m_dialog.setCancelButtonText(cancellable ? tr("Cancel") : QString());
}

void QtProgressCallback::SetStatusText(const char* text)
{
  BaseProgressCallback::SetStatusText(text);
  m_dialog.setLabelText(QString::fromUtf8(text));
}

void QtProgressCallback::SetProgressRange(u32 range)
{
  BaseProgressCallback::SetProgressRange(range);
  m_dialog.setRange(0, static_cast<int>(range));
}

void QtProgressCallback::SetProgressValue(u32 value)
{
  BaseProgressCallback::SetProgressValue(value);

  if (m_dialog.value() == static_cast<int>(value))
    return;

  m_dialog.setValue(value);
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

u32 QtProgressCallback::ModalPrompt(const char* message, u32 num_options, ...)
{
  enum : u32
  {
    MAX_OPTIONS = 3,
  };

  std::array<QString, MAX_OPTIONS> options;

  std::va_list ap;
  va_start(ap, num_options);

  for (u32 i = 0; i < num_options && i < MAX_OPTIONS; i++)
    options[i] = QString::fromUtf8(va_arg(ap, const char*));

  va_end(ap);

  return static_cast<u32>(QMessageBox::question(&m_dialog, tr("Question"), QString::fromUtf8(message), options[0],
                                                options[1], options[2], 0, 0));
}
