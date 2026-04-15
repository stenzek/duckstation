// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_debuggingsettingswidget.h"

class SettingsWindow;

class DebuggingSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit DebuggingSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~DebuggingSettingsWidget();

private:
  void addTweakOptions();
  void onResetToDefaultClicked();

  SettingsWindow* m_dialog;

  Ui::DebugSettingsWidget m_ui;
};
