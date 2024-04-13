// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_emulationsettingswidget.h"

class SettingsWindow;

class EmulationSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit EmulationSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~EmulationSettingsWidget();

private Q_SLOTS:
  void onEmulationSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onTurboSpeedIndexChanged(int index);
  void onVSyncChanged();
  void onOptimalFramePacingChanged();
  void onPreFrameSleepChanged();
  void updateRewind();

private:
  void fillComboBoxWithEmulationSpeeds(QComboBox* cb, float global_value);

  Ui::EmulationSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
