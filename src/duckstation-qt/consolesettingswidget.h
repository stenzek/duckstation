#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class SettingsDialog;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~ConsoleSettingsWidget();

private Q_SLOTS:
  void onEnableCPUClockSpeedControlChecked(int state);
  void onCPUClockSpeedValueChanged(int value);
  void updateCPUClockSpeedLabel();

private:
  void calculateCPUClockValue();

  Ui::ConsoleSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
