// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class SettingsWindow;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~ConsoleSettingsWidget();

private:
  void calculateCPUClockValue();

  void onFastBootChanged();
  void updateRecompilerICacheEnabled();
  void onEnableCPUClockSpeedControlChecked(int state);
  void onCPUClockSpeedValueChanged(int value);
  void updateCPUClockSpeedLabel();

  Ui::ConsoleSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
