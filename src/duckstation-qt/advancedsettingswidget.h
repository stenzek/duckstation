#pragma once

#include <QtWidgets/QWidget>

#include "ui_advancedsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class AdvancedSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AdvancedSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~AdvancedSettingsWidget();

private:
  Ui::AdvancedSettingsWidget m_ui;

  void onResetToDefaultClicked();

  QtHostInterface* m_host_interface;
};
