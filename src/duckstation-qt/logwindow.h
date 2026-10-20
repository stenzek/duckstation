// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/log.h"

#include <QtGui/QColor>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>

#include <array>
#include <atomic>
#include <span>

class ALIGN_TO_CACHE_LINE LogWidget : public QPlainTextEdit
{
  Q_OBJECT

public:
  // Only messages from channel_filter are shown, without the channel prefix or colours. MaxCount shows all channels.
  explicit LogWidget(QWidget* parent, Log::Channel channel_filter = Log::Channel::MaxCount);
  ~LogWidget();

  void appendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);

protected:
  void changeEvent(QEvent* event) override;

private:
  static constexpr int MAX_LINES = 1000;
  static constexpr int BLOCK_UPDATES_THRESHOLD = 100;

  void updateColors();
  void realAppendMessage(const QLatin1StringView& channel, quint32 cat, const QString& message);

  static void logCallback(void* pUserParam, Log::MessageCategory cat, const char* functionName,
                          std::string_view message);

  Log::Channel m_channel_filter;
  int m_lines_to_skip = 0;

  std::array<QColor, static_cast<size_t>(Log::Color::MaxCount)> m_message_colors;
  QColor m_timestamp_color;
  QColor m_channel_color;

  ALIGN_TO_CACHE_LINE std::atomic_int m_lines_pending{0};
};

class LogWindowBase : public QMainWindow
{
  Q_OBJECT

protected:
  LogWindowBase(const char* size_key_prefix, const QIcon& icon, Log::Channel channel_filter = Log::Channel::MaxCount);
  ~LogWindowBase();

  void setWindowTitleForGame(const QString& title, const QString& game_title_format);
  void destroyWindow();

  void closeEvent(QCloseEvent* event) override;

  bool m_destroying = false;

private:
  static constexpr int DEFAULT_WIDTH = 750;
  static constexpr int DEFAULT_HEIGHT = 400;

  void onSaveTriggered();

  void saveSize();
  void restoreSize();

  LogWidget* m_log_widget;
  const char* m_size_key_prefix;
};

class LogWindow : public LogWindowBase
{
  Q_OBJECT

public:
  LogWindow(bool attach_to_main);
  ~LogWindow();

  static void updateSettings(bool defer_show);
  static bool deferredShow();
  static void destroy();

  ALWAYS_INLINE bool isAttachedToMainWindow() const { return m_attached_to_main_window; }
  void reattachToMainWindow();

  void updateWindowTitle();

  static void populateFilterMenu(QMenu* menu);

private:
  void createUi();
  void updateLogLevelUi();
  void setLogLevel(Log::Level level);

  QMenu* m_level_menu;

  bool m_attached_to_main_window = true;
};

class TTYWindow : public LogWindowBase
{
  Q_OBJECT

public:
  TTYWindow();
  ~TTYWindow();

  static void updateSettings(bool defer_show);
  static bool deferredShow();
  static void destroy();

  void updateWindowTitle();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void createUi();
};

extern LogWindow* g_log_window;
extern TTYWindow* g_tty_window;
