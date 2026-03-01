// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_interfacesettingswidget.h"

class SettingsWindow;

class InterfaceSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit InterfaceSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~InterfaceSettingsWidget();

  static void setupLanguageCombo(QComboBox* const cb);
  static void setupThemeCombo(QComboBox* const cb);

private:
  void updateRenderToSeparateWindowOptions();
  void onLanguageChanged();
  void updateDisableStyleSheetsEnabled();
  void checkForUpdates();

  Ui::InterfaceSettingsWidget m_ui;

  SettingsWindow* m_dialog;
  QCheckBox* m_disable_style_sheets = nullptr;
};
