// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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
