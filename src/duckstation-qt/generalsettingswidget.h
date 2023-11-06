// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_generalsettingswidget.h"

class SettingsWindow;

class GeneralSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit GeneralSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GeneralSettingsWidget();

private Q_SLOTS:
  void onRenderToSeparateWindowChanged();

private:
  Ui::GeneralSettingsWidget m_ui;

  SettingsWindow* m_dialog;

public:
  static const char* THEME_NAMES[];
  static const char* THEME_VALUES[];
  static const char* DEFAULT_THEME_NAME;
};
