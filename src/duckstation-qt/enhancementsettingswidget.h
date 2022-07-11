#pragma once

#include <QtWidgets/QWidget>

#include "ui_enhancementsettingswidget.h"

class SettingsDialog;

class EnhancementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  EnhancementSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~EnhancementSettingsWidget();

private Q_SLOTS:
  void updateScaledDitheringEnabled();
  void updatePGXPSettingsEnabled();

private:
  void setupAdditionalUi();

  Ui::EnhancementSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
