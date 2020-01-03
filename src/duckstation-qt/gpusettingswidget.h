#pragma once

#include <QtWidgets/QWidget>

#include "ui_gpusettingswidget.h"

class QtHostInterface;

class GPUSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GPUSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~GPUSettingsWidget();

private:
  void setupAdditionalUi();

  Ui::GPUSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
