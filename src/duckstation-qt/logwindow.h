// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/log.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>

#include <atomic>
#include <span>

class ALIGN_TO_CACHE_LINE LogWindow : public QMainWindow
{
public:
  LogWindow(bool attach_to_main);
  ~LogWindow();

  static void updateSettings();
  static void destroy();

  ALWAYS_INLINE bool isAttachedToMainWindow() const { return m_attached_to_main_window; }
  void reattachToMainWindow();

  void updateWindowTitle();

  static void populateFilterMenu(QMenu* menu);

protected:
  void closeEvent(QCloseEvent* event);
  void changeEvent(QEvent* event);

private:
  static constexpr int DEFAULT_WIDTH = 750;
  static constexpr int DEFAULT_HEIGHT = 400;
  static constexpr int MAX_LINES = 1000;
  static constexpr int BLOCK_UPDATES_THRESHOLD = 100;

  void createUi();
  void updateLogLevelUi();
  void setLogLevel(Log::Level level);

  void onClearTriggered();
  void onSaveTriggered();
  void appendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);
  void realAppendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);

  void saveSize();
  void restoreSize();

  static void logCallback(void* pUserParam, Log::MessageCategory cat, const char* functionName,
                          std::string_view message);

  QPlainTextEdit* m_text;
  QMenu* m_level_menu;

  int m_lines_to_skip = 0;

  bool m_is_dark_theme = false;
  bool m_attached_to_main_window = true;
  bool m_destroying = false;

  ALIGN_TO_CACHE_LINE std::atomic_int m_lines_pending{0};
};

extern LogWindow* g_log_window;
