#pragma once

#include <QtWidgets/QWidget>

#include "ui_generalsettingswidget.h"

class SettingsDialog;

class GeneralSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit GeneralSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~GeneralSettingsWidget();

private Q_SLOTS:
  void onRenderToSeparateWindowChanged();

private:
  Ui::GeneralSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
