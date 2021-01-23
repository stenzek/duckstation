#pragma once

#include <QtWidgets/QWidget>

#include "ui_emulationsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class EmulationSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit EmulationSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~EmulationSettingsWidget();

private Q_SLOTS:
  void onEmulationSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onTurboSpeedIndexChanged(int index);
  void updateRewindSummaryLabel();
  void updateRunaheadFields();

private:

  Ui::EmulationSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
