#pragma once

#include <QtWidgets/QWidget>

#include "ui_generalsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class GeneralSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit GeneralSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~GeneralSettingsWidget();

private Q_SLOTS:
  void onEmulationSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onTurboSpeedIndexChanged(int index);

private:
  Ui::GeneralSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
