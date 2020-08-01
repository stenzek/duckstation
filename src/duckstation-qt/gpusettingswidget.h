#pragma once

#include <QtWidgets/QWidget>

#include "ui_gpusettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class GPUSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GPUSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~GPUSettingsWidget();

private Q_SLOTS:
  void updateScaledDitheringEnabled();
  void populateGPUAdapters();
  void onGPUAdapterIndexChanged();
  void updatePGXPSettingsEnabled();

private:
  void setupAdditionalUi();

  Ui::GPUSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
