// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"

#include <QtCore/QMap>
#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

#include "colorpickerbutton.h"

#include "ui_controllerglobalsettingswidget.h"
#include "ui_controllerledsettingsdialog.h"

class ControllerSettingsWindow;

class ControllerGlobalSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  ControllerGlobalSettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog);
  ~ControllerGlobalSettingsWidget();

Q_SIGNALS:
  void bindingSetupChanged();

private:
  void updateSDLOptionsEnabled();
  void ledSettingsClicked();
  void sdlHelpTextLinkClicked(const QString& link);

  Ui::ControllerGlobalSettingsWidget m_ui;
  ControllerSettingsWindow* m_dialog;
};

class ControllerLEDSettingsDialog : public QDialog
{
public:
  ControllerLEDSettingsDialog(QWidget* parent, ControllerSettingsWindow* dialog);
  ~ControllerLEDSettingsDialog();

private:
  void linkButton(ColorPickerButton* button, u32 player_id);

  Ui::ControllerLEDSettingsDialog m_ui;
  ControllerSettingsWindow* m_dialog;
};
