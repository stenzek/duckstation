#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~ConsoleSettingsWidget();

Q_SIGNALS:
  void multitapModeChanged();

private Q_SLOTS:
  void onEnableCPUClockSpeedControlChecked(int state);
  void onCPUClockSpeedValueChanged(int value);
  void updateCPUClockSpeedLabel();
  void onCDROMReadSpeedupValueChanged(int value);
  void updateCdromPreCacheCHDEnabled();

private:
  void calculateCPUClockValue();

  Ui::ConsoleSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
