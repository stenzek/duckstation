// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "colorpickerbutton.h"

#include "ui_controllerglobalsettingswidget.h"

#include "common/types.h"

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
  void sdlHelpTextLinkClicked(const QString& link);
  void ledSettingsClicked();

  Ui::ControllerGlobalSettingsWidget m_ui;
  ControllerSettingsWindow* m_dialog;
};
