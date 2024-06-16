// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/log.h"

#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <span>

class ScriptConsole : public QMainWindow
{
  Q_OBJECT

public:
  ScriptConsole();
  ~ScriptConsole();

  static void updateSettings();
  static void destroy();

private:
  void createUi();

  static void outputCallback(std::string_view message, void* userdata);

protected:
  void closeEvent(QCloseEvent* event);

private Q_SLOTS:
  void onClearTriggered();
  void onSaveTriggered();
  void appendMessage(const QString& message);
  void commandChanged(const QString& text);
  void executeClicked();

private:
  static constexpr int DEFAULT_WIDTH = 750;
  static constexpr int DEFAULT_HEIGHT = 400;

  void saveSize();
  void restoreSize();

  QPlainTextEdit* m_text;
  QLineEdit* m_command;
  QPushButton* m_execute;

  bool m_destroying = false;
};

extern ScriptConsole* g_script_console;
