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

  static void populateLanguageDropdown(QComboBox* cb);

Q_SIGNALS:
  void themeChanged();

private Q_SLOTS:
  void onRenderToSeparateWindowChanged();
  void onLanguageChanged();

private:
  Ui::InterfaceSettingsWidget m_ui;

  SettingsWindow* m_dialog;

public:
  static const char* THEME_NAMES[];
  static const char* THEME_VALUES[];
  static const char* DEFAULT_THEME_NAME;
};
