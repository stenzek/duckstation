#pragma once

#include <QtWidgets/QWidget>

#include "ui_enhancementsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class EnhancementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  EnhancementSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~EnhancementSettingsWidget();

private Q_SLOTS:
  void updateScaledDitheringEnabled();
  void updatePGXPSettingsEnabled();

private:
  void setupAdditionalUi();

  Ui::EnhancementSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
