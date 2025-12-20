// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/log.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>

#include <atomic>
#include <span>

class ALIGN_TO_CACHE_LINE LogWidget : public QPlainTextEdit
{
  Q_OBJECT

public:
  explicit LogWidget(QWidget* parent);
  ~LogWidget();

  void appendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);

protected:
  void changeEvent(QEvent* event) override;

private:
  static constexpr int MAX_LINES = 1000;
  static constexpr int BLOCK_UPDATES_THRESHOLD = 100;

  void realAppendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);

  static void logCallback(void* pUserParam, Log::MessageCategory cat, const char* functionName,
                          std::string_view message);

  int m_lines_to_skip = 0;

  bool m_is_dark_theme = false;

  ALIGN_TO_CACHE_LINE std::atomic_int m_lines_pending{0};
};

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

  static void populateFilterMenu(QMenu* menu);

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  static constexpr int DEFAULT_WIDTH = 750;
  static constexpr int DEFAULT_HEIGHT = 400;
  static constexpr int MAX_LINES = 1000;
  static constexpr int BLOCK_UPDATES_THRESHOLD = 100;

  void createUi();
  void updateLogLevelUi();
  void setLogLevel(Log::Level level);

  void onSaveTriggered();

  void saveSize();
  void restoreSize();

  LogWidget* m_log_widget;
  QMenu* m_level_menu;

  bool m_attached_to_main_window = true;
  bool m_destroying = false;
};

extern LogWindow* g_log_window;
