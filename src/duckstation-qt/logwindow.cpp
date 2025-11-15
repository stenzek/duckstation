// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "logwindow.h"
#include "mainwindow.h"
#include "qthost.h"
#include "settingwidgetbinder.h"

#include "util/host.h"

#include <QtCore/QLatin1StringView>
#include <QtCore/QUtf8StringView>
#include <QtGui/QIcon>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QScrollBar>

#include "moc_logwindow.cpp"

// TODO: Since log callbacks are synchronized, no mutex is needed here.
// But once I get rid of that, there will be.
LogWindow* g_log_window;

LogWindow::LogWindow(bool attach_to_main)
  : QMainWindow(), m_is_dark_theme(QtHost::IsDarkApplicationTheme()), m_attached_to_main_window(attach_to_main)
{
  restoreSize();
  createUi();

  Log::RegisterCallback(&LogWindow::logCallback, this);
}

LogWindow::~LogWindow() = default;

void LogWindow::updateSettings()
{
  const bool new_enabled = Host::GetBoolSettingValue("Logging", "LogToWindow", false);
  const bool attach_to_main = Host::GetBoolSettingValue("Logging", "AttachLogWindowToMainWindow", true);
  const bool curr_enabled = (g_log_window != nullptr);
  if (new_enabled == curr_enabled)
  {
    if (g_log_window && g_log_window->m_attached_to_main_window != attach_to_main)
    {
      g_log_window->m_attached_to_main_window = attach_to_main;
      if (attach_to_main)
        g_log_window->reattachToMainWindow();
    }

    return;
  }

  if (new_enabled)
  {
    g_log_window = new LogWindow(attach_to_main);
    if (attach_to_main && g_main_window && g_main_window->isVisible())
      g_log_window->reattachToMainWindow();

    g_log_window->show();
  }
  else if (g_log_window)
  {
    g_log_window->m_destroying = true;
    g_log_window->close();
    g_log_window->deleteLater();
    g_log_window = nullptr;
  }
}

void LogWindow::destroy()
{
  if (!g_log_window)
    return;

  g_log_window->m_destroying = true;
  g_log_window->close();
  g_log_window->deleteLater();
  g_log_window = nullptr;
}

void LogWindow::reattachToMainWindow()
{
  // Skip when maximized.
  if (g_main_window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
    return;

  resize(width(), g_main_window->height());

  const QPoint new_pos = g_main_window->pos() + QPoint(g_main_window->width() + 10, 0);
  if (pos() != new_pos)
    move(new_pos);
}

void LogWindow::updateWindowTitle()
{
  QString title;

  const QString& serial = QtHost::GetCurrentGameSerial();

  if (QtHost::IsSystemValid() && !serial.isEmpty())
  {
    const QFileInfo fi(QtHost::GetCurrentGamePath());
    title = tr("Log Window - %1 [%2]").arg(serial).arg(fi.fileName());
  }
  else
  {
    title = tr("Log Window");
  }

  setWindowTitle(title);
}

void LogWindow::createUi()
{
  QIcon icon;
  icon.addFile(QString::fromUtf8(":/icons/duck.png"), QSize(), QIcon::Normal, QIcon::Off);
  setWindowIcon(icon);
  setWindowFlag(Qt::CustomizeWindowHint, true);
  setWindowFlag(Qt::WindowCloseButtonHint, false);
  updateWindowTitle();

  QAction* action;

  QMenuBar* menu = new QMenuBar(this);
  setMenuBar(menu);

  QMenu* log_menu = menu->addMenu("&Log");
  QtUtils::StylePopupMenu(log_menu);
  action = log_menu->addAction(tr("&Clear"));
  connect(action, &QAction::triggered, this, &LogWindow::onClearTriggered);
  action = log_menu->addAction(tr("&Save..."));
  connect(action, &QAction::triggered, this, &LogWindow::onSaveTriggered);

  log_menu->addSeparator();

  action = log_menu->addAction(tr("Cl&ose"));
  QtUtils::StylePopupMenu(log_menu);
  connect(action, &QAction::triggered, this, &LogWindow::close);

  QMenu* settings_menu = menu->addMenu(tr("&Settings"));
  QtUtils::StylePopupMenu(settings_menu);

  action = settings_menu->addAction(tr("Log To &System Console"));
  action->setCheckable(true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, action, "Logging", "LogToConsole", false);

  action = settings_menu->addAction(tr("Log To &Debug Console"));
  action->setCheckable(true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, action, "Logging", "LogToDebug", false);

  action = settings_menu->addAction(tr("Log To &File"));
  action->setCheckable(true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, action, "Logging", "LogToFile", false);

  settings_menu->addSeparator();

  action = settings_menu->addAction(tr("Attach To &Main Window"));
  action->setCheckable(true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, action, "Logging", "AttachLogWindowToMainWindow", true);

  action = settings_menu->addAction(tr("Show &Timestamps"));
  action->setCheckable(true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, action, "Logging", "LogTimestamps", true);

  settings_menu->addSeparator();

  m_level_menu = settings_menu->addMenu(tr("&Log Level"));
  QtUtils::StylePopupMenu(m_level_menu);
  for (u32 i = 0; i < static_cast<u32>(Log::Level::MaxCount); i++)
  {
    action = m_level_menu->addAction(QString::fromUtf8(Settings::GetLogLevelDisplayName(static_cast<Log::Level>(i))));
    action->setCheckable(true);
    connect(action, &QAction::triggered, this, [this, i]() { setLogLevel(static_cast<Log::Level>(i)); });
  }
  updateLogLevelUi();

  QMenu* filters_menu = menu->addMenu(tr("&Channels"));
  QtUtils::StylePopupMenu(filters_menu);
  connect(filters_menu, &QMenu::aboutToShow, this, [filters_menu]() {
    filters_menu->clear();
    populateFilterMenu(filters_menu);
  });

  m_text = new QPlainTextEdit(this);
  m_text->setReadOnly(true);
  m_text->setUndoRedoEnabled(false);
  m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
  m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  m_text->setMaximumBlockCount(MAX_LINES);
  m_text->setFont(QtHost::GetFixedFont());

  setCentralWidget(m_text);
}

void LogWindow::updateLogLevelUi()
{
  const Log::Level level =
    Settings::ParseLogLevelName(Host::GetBaseStringSettingValue("Logging", "LogLevel", "").c_str())
      .value_or(Log::DEFAULT_LOG_LEVEL);

  const QList<QAction*> actions = m_level_menu->actions();
  for (u32 i = 0; i < actions.size(); i++)
    actions[i]->setChecked(static_cast<Log::Level>(i) == level);
}

void LogWindow::setLogLevel(Log::Level level)
{
  Host::SetBaseStringSettingValue("Logging", "LogLevel", Settings::GetLogLevelName(level));
  Host::CommitBaseSettingChanges();
  g_emu_thread->applySettings(false);
}

void LogWindow::populateFilterMenu(QMenu* filter_menu)
{
  const auto settings_Lock = Host::GetSettingsLock();
  const INISettingsInterface* si = QtHost::GetBaseSettingsInterface();

  for (const char* channel_name : Log::GetChannelNames())
  {
    const bool enabled = si->GetBoolValue("Logging", channel_name, true);
    QAction* const action = filter_menu->addAction(QString::fromUtf8(channel_name), [channel_name](bool checked) {
      Host::SetBaseBoolSettingValue("Logging", channel_name, checked);
      Host::CommitBaseSettingChanges();
      g_emu_thread->applySettings(false);
    });
    action->setCheckable(true);
    action->setChecked(enabled);
  }
}

void LogWindow::onClearTriggered()
{
  m_text->clear();
}

void LogWindow::onSaveTriggered()
{
  const QString path = QFileDialog::getSaveFileName(this, tr("Select Log File"), QString(), tr("Log Files (*.txt)"));
  if (path.isEmpty())
    return;

  QFile file(path);
  if (!file.open(QFile::WriteOnly | QFile::Text))
  {
    QtUtils::MessageBoxCritical(this, tr("Error"), tr("Failed to open file for writing."));
    return;
  }

  file.write(m_text->toPlainText().toUtf8());
  file.close();

  appendMessage(QLatin1StringView("LogWindow"), static_cast<u32>(Log::Level::Info),
                tr("Log was written to %1.\n").arg(path));
}

void LogWindow::logCallback(void* pUserParam, Log::MessageCategory cat, const char* functionName,
                            std::string_view message)
{
  LogWindow* this_ptr = static_cast<LogWindow*>(pUserParam);

  // TODO: Split message based on lines.
  // I don't like the memory allocations here either...

  QString qmessage;
  qmessage.reserve(message.length() + 1);
  qmessage.append(QUtf8StringView(message.data(), message.length()));
  qmessage.append(QChar('\n'));

  const QLatin1StringView qchannel(
    (Log::UnpackLevel(cat) <= Log::Level::Warning) ? functionName : Log::GetChannelName(Log::UnpackChannel(cat)));

  this_ptr->m_lines_pending.fetch_add(1, std::memory_order_acq_rel);

  if (QThread::isMainThread())
  {
    this_ptr->appendMessage(qchannel, static_cast<u32>(cat), qmessage);
  }
  else
  {
    QMetaObject::invokeMethod(this_ptr, &LogWindow::appendMessage, Qt::QueuedConnection, qchannel,
                              static_cast<quint32>(cat), qmessage);
  }
}

void LogWindow::closeEvent(QCloseEvent* event)
{
  if (!m_destroying)
  {
    event->ignore();
    return;
  }

  Log::UnregisterCallback(&LogWindow::logCallback, this);

  saveSize();

  QMainWindow::closeEvent(event);
}

void LogWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::StyleChange)
    m_is_dark_theme = QtHost::IsDarkApplicationTheme();

  QMainWindow::changeEvent(event);
}

void LogWindow::appendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message)
{
  const int num_lines_still_pending = m_lines_pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (m_lines_to_skip > 0)
  {
    m_lines_to_skip--;
    return;
  }

  if (num_lines_still_pending > MAX_LINES)
  {
    realAppendMessage(
      QLatin1StringView(Log::GetChannelName(Log::Channel::Log)),
      Log::PackCategory(Log::Channel::Log, Log::Level::Warning, Log::Color::StrongYellow),
      tr("Dropped %1 log messages, please use file or system console logging.\n").arg(num_lines_still_pending));
    m_lines_to_skip = num_lines_still_pending;
    return;
  }
  else if (num_lines_still_pending > BLOCK_UPDATES_THRESHOLD)
  {
    if (m_text->updatesEnabled())
    {
      m_text->setUpdatesEnabled(false);
      m_text->document()->blockSignals(true);
      m_text->blockSignals(true);
    }
  }
  else if (!m_text->updatesEnabled())
  {
    m_text->blockSignals(false);
    m_text->document()->blockSignals(false);
    m_text->setUpdatesEnabled(true);
  }

  realAppendMessage(channel, cat, message);
}

void LogWindow::realAppendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message)
{
  QTextCursor temp_cursor = m_text->textCursor();
  QScrollBar* scrollbar = m_text->verticalScrollBar();
  const bool cursor_at_end = temp_cursor.atEnd();
  const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

  temp_cursor.movePosition(QTextCursor::End);

  {
    static constexpr const QChar level_characters[static_cast<size_t>(Log::Level::MaxCount)] = {'X', 'E', 'W', 'I',
                                                                                                'V', 'D', 'B', 'T'};
    static constexpr const QColor message_colors[2][static_cast<size_t>(Log::Color::MaxCount)] = {
      // Light theme
      {
        QColor(0x00, 0x00, 0x00), // Default
        QColor(0x00, 0x00, 0x00), // Black
        QColor(0x70, 0x00, 0x00), // Red
        QColor(0xec, 0x5e, 0xf1), // Green
        QColor(0xe9, 0x39, 0xf3), // Blue
        QColor(0xA0, 0x00, 0xA0), // Magenta
        QColor(0xA0, 0x78, 0x00), // Orange
        QColor(0x80, 0xB4, 0xB4), // Cyan
        QColor(0xB4, 0xB4, 0x80), // Yellow
        QColor(0x70, 0x70, 0x70), // White
        QColor(0x00, 0x00, 0x00), // StrongBlack
        QColor(0x80, 0x00, 0x00), // StrongRed
        QColor(0x00, 0x80, 0x00), // StrongGreen
        QColor(0x00, 0x00, 0x80), // StrongBlue
        QColor(0xA0, 0x00, 0xA0), // StrongMagenta
        QColor(0xA0, 0x78, 0x00), // StrongOrange
        QColor(0x80, 0xB4, 0xB4), // StrongCyan
        QColor(0xb4, 0xb4, 0x00), // StrongYellow
        QColor(0x0D, 0x0d, 0x0D)  // StrongWhite
      },
      // Dark theme
      {
        QColor(0xD0, 0xD0, 0xD0), // Default
        QColor(0xFF, 0xFF, 0xFF), // Black
        QColor(0xB4, 0x00, 0x00), // Red
        QColor(0x13, 0xA1, 0x0E), // Green
        QColor(0x00, 0x37, 0xDA), // Blue
        QColor(0xA0, 0x00, 0xA0), // Magenta
        QColor(0xA0, 0x78, 0x00), // Orange
        QColor(0x80, 0xB4, 0xB4), // Cyan
        QColor(0xB4, 0xB4, 0x80), // Yellow
        QColor(0xCC, 0xCC, 0xCC), // White
        QColor(0xFF, 0xFF, 0xFF), // StrongBlack
        QColor(0xE7, 0x48, 0x56), // StrongRed
        QColor(0x16, 0xC6, 0x0C), // StrongGreen
        QColor(0x20, 0x20, 0xCC), // StrongBlue
        QColor(0xA0, 0x00, 0xA0), // StrongMagenta
        QColor(0xB4, 0x96, 0x00), // StrongOrange
        QColor(0x80, 0xB4, 0xB4), // StrongCyan
        QColor(0xF9, 0xF1, 0xA5), // StrongYellow
        QColor(0xFF, 0xFF, 0xFF), // StrongWhite
      },
    };
    static constexpr const QColor timestamp_color[2] = {QColor(0x60, 0x60, 0x60), QColor(0xcc, 0xcc, 0xcc)};
    static constexpr const QColor channel_color[2] = {QColor(0x30, 0x30, 0x30), QColor(0xf2, 0xf2, 0xf2)};

    QTextCharFormat format = temp_cursor.charFormat();
    const size_t dark = static_cast<size_t>(m_is_dark_theme);

    temp_cursor.beginEditBlock();
    if (Log::AreConsoleOutputTimestampsEnabled())
    {
      const float message_time = Log::GetCurrentMessageTime();
      const QString qtimestamp = QStringLiteral("[%1] ").arg(message_time, 10, 'f', 4);
      format.setForeground(QBrush(timestamp_color[dark]));
      temp_cursor.setCharFormat(format);
      temp_cursor.insertText(qtimestamp);
    }

    const Log::Level level = Log::UnpackLevel(static_cast<Log::MessageCategory>(cat));
    const Log::Color color = (Log::UnpackColor(static_cast<Log::MessageCategory>(cat)) == Log::Color::Default) ?
                               Log::GetColorForLevel(level) :
                               Log::UnpackColor(static_cast<Log::MessageCategory>(cat));
    const QString qchannel =
      (level <= Log::Level::Warning) ?
        QStringLiteral("%1(%2): ").arg(level_characters[static_cast<size_t>(level)]).arg(channel) :
        QStringLiteral("%1/%2: ").arg(level_characters[static_cast<size_t>(level)]).arg(channel);
    format.setForeground(QBrush(channel_color[dark]));
    temp_cursor.setCharFormat(format);
    temp_cursor.insertText(qchannel);

    // message has \n already
    format.setForeground(QBrush(message_colors[dark][static_cast<size_t>(color)]));
    temp_cursor.setCharFormat(format);
    temp_cursor.insertText(message);
    temp_cursor.endEditBlock();
  }

  if (cursor_at_end && scroll_at_end)
    m_text->centerCursor();
}

void LogWindow::saveSize()
{
  const int current_width = Host::GetBaseIntSettingValue("UI", "LogWindowWidth", DEFAULT_WIDTH);
  const int current_height = Host::GetBaseIntSettingValue("UI", "LogWindowHeight", DEFAULT_HEIGHT);
  const QSize wsize = size();

  bool changed = false;
  if (current_width != wsize.width())
  {
    Host::SetBaseIntSettingValue("UI", "LogWindowWidth", wsize.width());
    changed = true;
  }
  if (current_height != wsize.height())
  {
    Host::SetBaseIntSettingValue("UI", "LogWindowHeight", wsize.height());
    changed = true;
  }

  if (changed)
    Host::CommitBaseSettingChanges();
}

void LogWindow::restoreSize()
{
  const int width = Host::GetBaseIntSettingValue("UI", "LogWindowWidth", DEFAULT_WIDTH);
  const int height = Host::GetBaseIntSettingValue("UI", "LogWindowHeight", DEFAULT_HEIGHT);
  resize(width, height);
}
