// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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

// TODO: Since log callbacks are synchronized, no mutex is needed here.
// But once I get rid of that, there will be.
LogWindow* g_log_window;

LogWindow::LogWindow(bool attach_to_main)
  : QMainWindow(), m_filter_names(Settings::GetLogFilters()), m_attached_to_main_window(attach_to_main)
{
  restoreSize();
  createUi();

  Log::RegisterCallback(&LogWindow::logCallback, this);
}

LogWindow::~LogWindow()
{
  Log::UnregisterCallback(&LogWindow::logCallback, this);
}

void LogWindow::updateSettings()
{
  const bool new_enabled = Host::GetBaseBoolSettingValue("Logging", "LogToWindow", false);
  const bool attach_to_main = Host::GetBaseBoolSettingValue("Logging", "AttachLogWindowToMainWindow", true);
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
    g_log_window->close();
    g_log_window->deleteLater();
    g_log_window = nullptr;
  }
}

void LogWindow::destroy()
{
  if (!g_log_window)
    return;

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
  setWindowFlag(Qt::WindowCloseButtonHint, false);
  updateWindowTitle();

  QAction* action;

  QMenuBar* menu = new QMenuBar(this);
  setMenuBar(menu);

  QMenu* log_menu = menu->addMenu("&Log");
  action = log_menu->addAction(tr("&Clear"));
  connect(action, &QAction::triggered, this, &LogWindow::onClearTriggered);
  action = log_menu->addAction(tr("&Save..."));
  connect(action, &QAction::triggered, this, &LogWindow::onSaveTriggered);

  log_menu->addSeparator();

  action = log_menu->addAction(tr("Cl&ose"));
  connect(action, &QAction::triggered, this, &LogWindow::close);

  QMenu* settings_menu = menu->addMenu(tr("&Settings"));

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
  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
  {
    action = m_level_menu->addAction(QString::fromUtf8(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));
    action->setCheckable(true);
    connect(action, &QAction::triggered, this, [this, i]() { setLogLevel(static_cast<LOGLEVEL>(i)); });
  }
  updateLogLevelUi();

  QMenu* filters_menu = menu->addMenu(tr("&Filters"));
  populateFilters(filters_menu);

  m_text = new QPlainTextEdit(this);
  m_text->setReadOnly(true);
  m_text->setUndoRedoEnabled(false);
  m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard);
  m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

#if defined(_WIN32)
  QFont font("Consolas");
  font.setPointSize(10);
#elif defined(__APPLE__)
  QFont font("Monaco");
  font.setPointSize(11);
#else
  QFont font("Monospace");
  font.setStyleHint(QFont::TypeWriter);
#endif
  m_text->setFont(font);

  setCentralWidget(m_text);
}

void LogWindow::updateLogLevelUi()
{
  const u32 level = Settings::ParseLogLevelName(Host::GetBaseStringSettingValue("Logging", "LogLevel", "").c_str())
                      .value_or(Settings::DEFAULT_LOG_LEVEL);

  const QList<QAction*> actions = m_level_menu->actions();
  for (u32 i = 0; i < actions.size(); i++)
    actions[i]->setChecked(i == level);
}

void LogWindow::setLogLevel(LOGLEVEL level)
{
  Host::SetBaseStringSettingValue("Logging", "LogLevel", Settings::GetLogLevelName(level));
  Host::CommitBaseSettingChanges();
  g_emu_thread->applySettings(false);
}

void LogWindow::populateFilters(QMenu* filter_menu)
{
  const std::string filters = Host::GetBaseStringSettingValue("Logging", "LogFilter", "");
  for (size_t i = 0; i < m_filter_names.size(); i++)
  {
    const char* filter = m_filter_names[i];
    const bool is_currently_filtered = (filters.find(filter) == std::string::npos);
    QAction* action = filter_menu->addAction(QString::fromUtf8(filter));
    action->setCheckable(action);
    action->setChecked(is_currently_filtered);
    connect(action, &QAction::triggered, this, [this, i](bool checked) { setChannelFiltered(i, !checked); });
  }
}

void LogWindow::setChannelFiltered(size_t index, bool enabled)
{
  const char* filter = m_filter_names[index];
  const size_t filter_len = std::strlen(filter);

  std::string filters = Host::GetBaseStringSettingValue("Logging", "LogFilter", "");
  const std::string::size_type pos = filters.find(filter);

  if (!enabled)
  {
    if (pos == std::string::npos)
      return;

    const size_t erase_count =
      filter_len + (((pos + filter_len) < filters.length() && filters[pos + filter_len] == ' ') ? 1 : 0);
    filters.erase(pos, erase_count);
  }
  else
  {
    if (pos != std::string::npos)
      return;

    if (!filters.empty() && filters.back() != ' ')
      filters.push_back(' ');
    filters.append(filter);
  }

  Host::SetBaseStringSettingValue("Logging", "LogFilter", filters.c_str());
  Host::CommitBaseSettingChanges();
  g_emu_thread->applySettings(false);
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
    QMessageBox::critical(this, tr("Error"), tr("Failed to open file for writing."));
    return;
  }

  file.write(m_text->toPlainText().toUtf8());
  file.close();

  appendMessage(QLatin1StringView("LogWindow"), LOGLEVEL_INFO, tr("Log was written to %1.\n").arg(path));
}

void LogWindow::logCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                            std::string_view message)
{
  LogWindow* this_ptr = static_cast<LogWindow*>(pUserParam);

  // TODO: Split message based on lines.
  // I don't like the memory allocations here either...

  QString qmessage;
  qmessage.reserve(message.length() + 1);
  qmessage.append(QUtf8StringView(message.data(), message.length()));
  qmessage.append(QChar('\n'));

  const QLatin1StringView qchannel((level <= LOGLEVEL_PERF) ? functionName : channelName);

  if (g_emu_thread->isOnUIThread())
  {
    this_ptr->appendMessage(qchannel, level, qmessage);
  }
  else
  {
    QMetaObject::invokeMethod(this_ptr, "appendMessage", Qt::QueuedConnection,
                              Q_ARG(const QLatin1StringView&, qchannel), Q_ARG(quint32, static_cast<u32>(level)),
                              Q_ARG(const QString&, qmessage));
  }
}

void LogWindow::closeEvent(QCloseEvent* event)
{
  Log::UnregisterCallback(&LogWindow::logCallback, this);

  saveSize();

  QMainWindow::closeEvent(event);
}

void LogWindow::appendMessage(const QLatin1StringView& channel, quint32 level, const QString& message)
{
  QTextCursor temp_cursor = m_text->textCursor();
  QScrollBar* scrollbar = m_text->verticalScrollBar();
  const bool cursor_at_end = temp_cursor.atEnd();
  const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

  temp_cursor.movePosition(QTextCursor::End);

  {
    static constexpr const QChar level_characters[LOGLEVEL_COUNT] = {'X', 'E', 'W', 'P', 'I', 'V', 'D', 'R', 'B', 'T'};
    static constexpr const QColor level_colors[LOGLEVEL_COUNT] = {
      QColor(255, 255, 255),    // NONE
      QColor(0xE7, 0x48, 0x56), // ERROR, Red Intensity
      QColor(0xF9, 0xF1, 0xA5), // WARNING, Yellow Intensity
      QColor(0xB4, 0x00, 0x9E), // PERF, Purple Intensity
      QColor(0xF2, 0xF2, 0xF2), // INFO, White Intensity
      QColor(0x16, 0xC6, 0x0C), // VERBOSE, Green Intensity
      QColor(0xCC, 0xCC, 0xCC), // DEV, White
      QColor(0x61, 0xD6, 0xD6), // PROFILE, Cyan Intensity
      QColor(0x13, 0xA1, 0x0E), // DEBUG, Green
      QColor(0x00, 0x37, 0xDA), // TRACE, Blue
    };
    static constexpr const QColor timestamp_color = QColor(0xcc, 0xcc, 0xcc);
    static constexpr const QColor channel_color = QColor(0xf2, 0xf2, 0xf2);

    QTextCharFormat format = temp_cursor.charFormat();

    if (g_settings.log_timestamps)
    {
      const float message_time = Log::GetCurrentMessageTime();
      const QString qtimestamp = QStringLiteral("[%1] ").arg(message_time, 10, 'f', 4);
      format.setForeground(QBrush(timestamp_color));
      temp_cursor.setCharFormat(format);
      temp_cursor.insertText(qtimestamp);
    }

    const QString qchannel = (level <= LOGLEVEL_PERF) ?
                               QStringLiteral("%1(%2): ").arg(level_characters[level]).arg(channel) :
                               QStringLiteral("%1/%2: ").arg(level_characters[level]).arg(channel);
    format.setForeground(QBrush(channel_color));
    temp_cursor.setCharFormat(format);
    temp_cursor.insertText(qchannel);

    // message has \n already
    format.setForeground(QBrush(level_colors[level]));
    temp_cursor.setCharFormat(format);
    temp_cursor.insertText(message);
  }

  if (cursor_at_end)
  {
    if (scroll_at_end)
    {
      m_text->setTextCursor(temp_cursor);
      scrollbar->setSliderPosition(scrollbar->maximum());
    }
    else
    {
      // Can't let changing the cursor affect the scroll bar...
      const int pos = scrollbar->sliderPosition();
      m_text->setTextCursor(temp_cursor);
      scrollbar->setSliderPosition(pos);
    }
  }
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
