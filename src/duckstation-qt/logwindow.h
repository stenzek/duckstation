// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/log.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <span>

class LogWindow : public QMainWindow
{
  Q_OBJECT

public:
  LogWindow(bool attach_to_main);
  ~LogWindow();

  static void updateSettings();
  static void destroy();

  ALWAYS_INLINE bool isAttachedToMainWindow() const { return m_attached_to_main_window; }
  void reattachToMainWindow();

  void updateWindowTitle();

private:
  void createUi();
  void updateLogLevelUi();
  void setLogLevel(LOGLEVEL level);
  void populateFilters(QMenu* filter_menu);
  void setChannelFiltered(size_t index, bool state);

  static void logCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                          std::string_view message);

protected:
  void closeEvent(QCloseEvent* event);

private Q_SLOTS:
  void onClearTriggered();
  void onSaveTriggered();
  void appendMessage(const QLatin1StringView& channel, quint32 level, const QString& message);

private:
  static constexpr int DEFAULT_WIDTH = 750;
  static constexpr int DEFAULT_HEIGHT = 400;

  void saveSize();
  void restoreSize();

  QPlainTextEdit* m_text;
  QMenu* m_level_menu;
  std::span<const char*> m_filter_names;

  bool m_attached_to_main_window = true;
};

extern LogWindow* g_log_window;
