// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtprogresscallback.h"
#include "qthost.h"
#include "qtutils.h"

#include "common/assert.h"
#include "common/small_string.h"
#include "common/log.h"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>
#include <array>

#include "moc_qtprogresscallback.cpp"

QtProgressCallback::QtProgressCallback(QObject* parent /* = nullptr */) : QObject(parent)
{
}

QtProgressCallback::~QtProgressCallback() = default;

bool QtProgressCallback::IsCancelled() const
{
  return m_ts_cancelled.load(std::memory_order_acquire);
}

void QtProgressCallback::SetTitle(const std::string_view title)
{
  emit titleUpdated(QtUtils::StringViewToQString(title));
}

void QtProgressCallback::StateChanged(StateChange changed)
{
  if (changed & STATE_CHANGE_STATUS_TEXT)
    emit statusTextUpdated(QtUtils::StringViewToQString(m_status_text));
  if (changed & STATE_CHANGE_PROGRESS)
    emit progressUpdated(static_cast<int>(m_progress_value), static_cast<int>(m_progress_range));
}

void QtProgressCallback::connectWidgets(QLabel* const status_label, QProgressBar* const progress_bar,
                                        QAbstractButton* const cancel_button)
{
  if (status_label)
    connect(this, &QtProgressCallback::statusTextUpdated, status_label, &QLabel::setText);
  if (progress_bar)
  {
    connect(this, &QtProgressCallback::progressUpdated, progress_bar, [progress_bar](int value, int range) {
      // qt checks if the value has changed
      progress_bar->setMaximum(range);
      progress_bar->setValue(value);
    });
  }
  if (cancel_button)
  {
    // force direct connection so it executes on the calling thread
    connect(
      cancel_button, &QAbstractButton::clicked, this,
      [this]() { m_ts_cancelled.store(true, std::memory_order_release); }, Qt::DirectConnection);
  }
}

QtAsyncTaskWithProgress::QtAsyncTaskWithProgress() : QObject()
{
}

QtAsyncTaskWithProgress::~QtAsyncTaskWithProgress() = default;

bool QtAsyncTaskWithProgress::IsCancelled() const
{
  return m_ts_cancelled.load(std::memory_order_acquire);
}

void QtAsyncTaskWithProgress::SetTitle(const std::string_view title)
{
  emit titleUpdated(QtUtils::StringViewToQString(title));
}

void QtAsyncTaskWithProgress::StateChanged(StateChange changed)
{
  if (changed & STATE_CHANGE_STATUS_TEXT)
    emit statusTextUpdated(QtUtils::StringViewToQString(m_status_text));
  if (changed & STATE_CHANGE_PROGRESS)
    emit progressUpdated(static_cast<int>(m_progress_value), static_cast<int>(m_progress_range));
}

void QtAsyncTaskWithProgress::connectWidgets(QLabel* const status_label, QProgressBar* const progress_bar,
                                             QAbstractButton* const cancel_button)
{
  if (status_label)
    connect(this, &QtAsyncTaskWithProgress::statusTextUpdated, status_label, &QLabel::setText);
  if (progress_bar)
  {
    connect(this, &QtAsyncTaskWithProgress::progressUpdated, progress_bar, [progress_bar](int value, int range) {
      // qt checks if the value has changed
      progress_bar->setMaximum(range);
      progress_bar->setValue(value);
    });
  }
  if (cancel_button)
  {
    // force direct connection so it executes on the calling thread
    connect(
      cancel_button, &QAbstractButton::clicked, this,
      [this]() { m_ts_cancelled.store(true, std::memory_order_release); }, Qt::DirectConnection);
  }
}

QtAsyncTaskWithProgress* QtAsyncTaskWithProgress::create(QWidget* const callback_parent, WorkCallback callback)
{
  QtAsyncTaskWithProgress* self = new QtAsyncTaskWithProgress();
  self->m_callback = std::move(callback);

  connect(self, &QtAsyncTaskWithProgress::completed, callback_parent, [self]() {
    CompletionCallback& cb = std::get<CompletionCallback>(self->m_callback);
    if (cb)
      cb();
  });

  return self;
}

void QtAsyncTaskWithProgress::start()
{
  // Disconnect from the calling thread, so it can be pulled by the async task.
  moveToThread(nullptr);

  Host::QueueAsyncTask([this]() mutable {
    QThread* const worker_thread = QThread::currentThread();
    moveToThread(worker_thread);

    m_callback = std::get<WorkCallback>(m_callback)(this);
    moveToThread(nullptr);

    Host::RunOnUIThread([self = this]() {
      self->moveToThread(QThread::currentThread());
      emit self->completed();
      delete self;
    });
  });
}

void QtAsyncTaskWithProgress::cancel()
{
  m_ts_cancelled.store(true, std::memory_order_release);
}

QtAsyncTaskWithProgressDialog::QtAsyncTaskWithProgressDialog(const QString& initial_title,
                                                             const QString& initial_status_text,
                                                             bool initial_message_log, bool initial_cancellable,
                                                             int initial_range, int initial_value, float show_delay,
                                                             bool auto_close, QWidget* dialog_parent,
                                                             WorkCallback callback)
  : m_callback(std::move(callback)), m_show_delay(show_delay), m_auto_close(auto_close)
{
  m_dialog = new ProgressDialog(initial_title, initial_status_text, initial_message_log, initial_cancellable,
                                initial_range, initial_value, this, dialog_parent);
  m_cancellable = initial_cancellable;
  m_progress_range = initial_range;
  m_progress_value = initial_value;

  if (show_delay <= 0.0f)
  {
    m_shown = true;
    m_dialog->open();
  }
}

QtAsyncTaskWithProgressDialog::~QtAsyncTaskWithProgressDialog()
{
  if (m_dialog)
  {
    if (m_auto_close)
    {
      // should null out itself
      delete m_dialog;
      DebugAssert(!m_dialog);
    }
    else
    {
      m_dialog->taskFinished();
    }
  }
}

QtAsyncTaskWithProgressDialog::ProgressDialog::ProgressDialog(const QString& initial_title,
                                                              const QString& initial_status_text,
                                                              bool initial_message_log, bool initial_cancellable,
                                                              int initial_range, int initial_value,
                                                              QtAsyncTaskWithProgressDialog* task, QWidget* parent)
  : QDialog(parent), m_task(task)
{
  if (!initial_title.isEmpty())
    setWindowTitle(initial_title);
  else
    setWindowTitle(QStringLiteral("DuckStation"));

  setWindowFlag(Qt::CustomizeWindowHint, true);
  setWindowFlag(Qt::WindowCloseButtonHint, initial_cancellable);
  setWindowModality(Qt::WindowModal);
  setMinimumWidth(MINIMUM_WIDTH);

  m_progress_bar = new QProgressBar(this);
  m_progress_bar->setRange(0, initial_range);
  m_progress_bar->setValue(initial_value);

  m_status_label = new QLabel(this);
  m_status_label->setAlignment(Qt::AlignCenter);
  if (!initial_status_text.isEmpty())
    m_status_label->setText(initial_status_text);

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::close);
  m_button_box->setVisible(initial_cancellable);

  m_layout = new QVBoxLayout(this);
  m_layout->setSpacing(8);
  m_layout->addWidget(m_status_label);
  m_layout->addWidget(m_progress_bar);
  m_layout->addWidget(m_button_box);

  if (initial_message_log)
    addMessageLog();

  updateMinimumHeight();
}

QtAsyncTaskWithProgressDialog::ProgressDialog::~ProgressDialog()
{
  if (m_task)
  {
    DebugAssert(m_task->m_dialog == this);
    m_task->m_dialog = nullptr;
  }
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::setCancellable(bool cancellable)
{
  if (cancellable == m_button_box->isVisible())
    return;

  setWindowFlag(Qt::WindowCloseButtonHint, cancellable);
  updateMinimumHeight();

  m_button_box->setVisible(cancellable);
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::closeEvent(QCloseEvent* event)
{
  cancelled();
  QDialog::closeEvent(event);
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::updateMinimumHeight()
{
  setMinimumHeight((m_button_box->isVisible() ? MINIMUM_HEIGHT_WITH_CANCEL : MINIMUM_HEIGHT_WITHOUT_CANCEL) +
                   (m_message_log ? MESSAGE_LOG_HEIGHT : 0));
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::cancelled()
{
  if (m_task)
    m_task->m_ts_cancelled.store(true, std::memory_order_release);
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::taskFinished()
{
  DebugAssert(m_task);
  m_task = nullptr;

  m_button_box->setStandardButtons(QDialogButtonBox::Close);
  if (!m_button_box->isVisible())
  {
    m_button_box->setVisible(true);
    updateMinimumHeight();
  }
}

void QtAsyncTaskWithProgressDialog::ProgressDialog::addMessageLog()
{
  DebugAssert(!m_message_log);

  m_message_log = new QPlainTextEdit(this);
  m_message_log->setReadOnly(true);
  m_message_log->setMinimumHeight(100);
  m_layout->insertWidget(2, m_message_log);
}

QtAsyncTaskWithProgressDialog* QtAsyncTaskWithProgressDialog::create(QWidget* parent, std::string_view initial_title,
                                                                     std::string_view initial_status_text,
                                                                     bool initial_message_log, bool initial_cancellable,
                                                                     int initial_range, int initial_value,
                                                                     float show_delay, bool auto_close,
                                                                     WorkCallback callback)
{
  DebugAssert(parent);

  // NOTE: Must get connected before queuing, because otherwise you risk a race.
  QtAsyncTaskWithProgressDialog* task = new QtAsyncTaskWithProgressDialog(
    QtUtils::StringViewToQString(initial_title), QtUtils::StringViewToQString(initial_status_text), initial_message_log,
    initial_cancellable, initial_range, initial_value, show_delay, auto_close, parent, std::move(callback));
  connect(task, &QtAsyncTaskWithProgressDialog::completed, parent, [task]() {
    CompletionCallback& cb = std::get<CompletionCallback>(task->m_callback);
    if (cb)
      cb();
  });

  Host::QueueAsyncTask([task]() {
    task->m_callback = std::get<WorkCallback>(task->m_callback)(task);
    Host::RunOnUIThread([task]() {
      emit task->completed(task);
      delete task;
    });
  });

  return task;
}

QtAsyncTaskWithProgressDialog* QtAsyncTaskWithProgressDialog::create(QWidget* parent, float show_delay,
                                                                     WorkCallback callback)
{
  return create(parent, {}, {}, false, false, 0, 1, show_delay, true, std::move(callback));
}

void QtAsyncTaskWithProgressDialog::cancel()
{
  m_ts_cancelled.store(true, std::memory_order_release);
}

bool QtAsyncTaskWithProgressDialog::IsCancelled() const
{
  return m_ts_cancelled.load(std::memory_order_acquire);
}

void QtAsyncTaskWithProgressDialog::SetTitle(const std::string_view title)
{
  Host::RunOnUIThread([this, title = QtUtils::StringViewToQString(title)]() {
    if (m_dialog)
      m_dialog->setWindowTitle(title);
  });
}

void QtAsyncTaskWithProgressDialog::StateChanged(StateChange changed)
{
  if (changed & (STATE_CHANGE_STATUS_TEXT | STATE_CHANGE_PROGRESS))
  {
    if (m_shown)
    {
      if (changed & STATE_CHANGE_STATUS_TEXT)
      {
        Host::RunOnUIThread([this, text = QtUtils::StringViewToQString(m_status_text)]() {
          if (m_dialog)
            m_dialog->m_status_label->setText(text);
        });
      }
      if (changed & STATE_CHANGE_PROGRESS)
      {
        Host::RunOnUIThread(
          [this, value = static_cast<int>(m_progress_value), range = static_cast<int>(m_progress_range)]() {
            if (m_dialog)
              m_dialog->m_progress_bar->setRange(0, range);
            if (m_dialog)
              m_dialog->m_progress_bar->setValue(value);
          });
      }
    }
    else
    {
      CheckForDelayedShow();
    }
  }
  if (changed & STATE_CHANGE_CANCELLABLE)
  {
    Host::RunOnUIThread([this, cancellable = m_cancellable]() {
      if (m_dialog)
        m_dialog->setCancellable(cancellable);
    });
  }
}

static QMessageBox::Icon ConvertPromptIcon(ProgressCallbackWithPrompt::PromptIcon icon)
{
  switch (icon)
  {
    case ProgressCallbackWithPrompt::PromptIcon::Error:
      return QMessageBox::Critical;
    case ProgressCallbackWithPrompt::PromptIcon::Warning:
      return QMessageBox::Warning;
    case ProgressCallbackWithPrompt::PromptIcon::Question:
      return QMessageBox::Question;
    case ProgressCallbackWithPrompt::PromptIcon::Information:
    default:
      return QMessageBox::Information;
  }
}

void QtAsyncTaskWithProgressDialog::AlertPrompt(PromptIcon icon, std::string_view message)
{
  m_prompt_waiting.test_and_set(std::memory_order_release);

  Host::RunOnUIThread([this, icon, message = QtUtils::StringViewToQString(message)]() {
    if (!m_dialog)
    {
      // dialog closed :(
      m_prompt_waiting.clear(std::memory_order_release);
      m_prompt_waiting.notify_one();
      return;
    }

    EnsureShown();

    QMessageBox* msgbox =
      QtUtils::NewMessageBox(m_dialog, ConvertPromptIcon(icon), m_dialog->windowTitle(), message, QMessageBox::Ok);
    connect(msgbox, &QMessageBox::finished, [this]() {
      m_prompt_waiting.clear(std::memory_order_release);
      m_prompt_waiting.notify_one();
    });
    msgbox->open();
  });

  m_prompt_waiting.wait(true, std::memory_order_acquire);
}

bool QtAsyncTaskWithProgressDialog::ConfirmPrompt(PromptIcon icon, std::string_view message,
                                                  std::string_view yes_text /*= {}*/, std::string_view no_text /*= {}*/)
{
  m_prompt_result.store(false, std::memory_order_relaxed);
  m_prompt_waiting.test_and_set(std::memory_order_release);

  Host::RunOnUIThread([this, icon, message = QtUtils::StringViewToQString(message),
                       yes_text = QtUtils::StringViewToQString(yes_text),
                       no_text = QtUtils::StringViewToQString(no_text)]() {
    if (!m_dialog)
    {
      // dialog closed :(
      m_prompt_waiting.clear(std::memory_order_release);
      m_prompt_waiting.notify_one();
      return;
    }

    EnsureShown();

    QMessageBox* msgbox = QtUtils::NewMessageBox(m_dialog, ConvertPromptIcon(icon), m_dialog->windowTitle(), message,
                                                 QMessageBox::NoButton);
    QAbstractButton* yes_button;
    if (!yes_text.isEmpty())
      yes_button = msgbox->addButton(yes_text, QMessageBox::YesRole);
    else
      yes_button = msgbox->addButton(QMessageBox::Yes);
    if (!no_text.isEmpty())
      msgbox->addButton(no_text, QMessageBox::NoRole);
    else
      msgbox->addButton(QMessageBox::No);
    connect(msgbox, &QMessageBox::finished, [this, msgbox, yes_button]() {
      m_prompt_result.store((msgbox->clickedButton() == yes_button), std::memory_order_relaxed);
      m_prompt_waiting.clear(std::memory_order_release);
      m_prompt_waiting.notify_one();
    });
    msgbox->open();
  });

  m_prompt_waiting.wait(true, std::memory_order_acquire);
  return m_prompt_result.load(std::memory_order_relaxed);
}

void QtAsyncTaskWithProgressDialog::AppendMessage(std::string_view message)
{
  Log::Write(Log::PackCategory(Log::Channel::Host, Log::Level::Info, Log::Color::StrongOrange), message);

  EnsureShown();

  Host::RunOnUIThread([this, message = QtUtils::StringViewToQString(message)]() {
    if (!m_dialog)
      return;

    if (!m_dialog->m_message_log)
      m_dialog->addMessageLog();

    m_dialog->m_message_log->appendPlainText(message);

    QScrollBar* const scrollbar = m_dialog->m_message_log->verticalScrollBar();
    const bool cursor_at_end = m_dialog->m_message_log->textCursor().atEnd();
    const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();
    if (cursor_at_end && scroll_at_end)
      m_dialog->m_message_log->centerCursor();
  });
}

void QtAsyncTaskWithProgressDialog::SetAutoClose(bool enabled)
{
  m_auto_close = enabled;
}

void QtAsyncTaskWithProgressDialog::EnsureShown()
{
  if (!m_shown)
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

void QtAsyncTaskWithProgressDialog::CheckForDelayedShow()
{
  DebugAssert(!m_shown);

  if (m_show_timer.GetTimeSeconds() < m_show_delay)
    return;

  EnsureShown();
}
