// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_osdsettingswidget.h"

class SettingsWindow;

class OSDSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  OSDSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~OSDSettingsWidget();

private:
  void setupAdditionalUi();

  void onOSDShowMessagesChanged();

  SettingsWindow* m_dialog;
  Ui::OSDSettingsWidget m_ui;
};
