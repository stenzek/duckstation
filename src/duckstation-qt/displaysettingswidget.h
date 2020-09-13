#pragma once

#include <QtWidgets/QWidget>

#include "ui_displaysettingswidget.h"

class QtHostInterface;
class PostProcessingChainConfigWidget;
class SettingsDialog;

class DisplaySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  DisplaySettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~DisplaySettingsWidget();

private Q_SLOTS:
  void populateGPUAdapters();
  void onGPUAdapterIndexChanged();

private:
  void setupAdditionalUi();

  Ui::DisplaySettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
