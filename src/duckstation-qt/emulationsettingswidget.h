// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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

private:
  void fillComboBoxWithEmulationSpeeds(QComboBox* cb, float global_value);

  void onEmulationSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onTurboSpeedIndexChanged(int index);
  void onOptimalFramePacingChanged();
  void onPreFrameSleepChanged();
  void updateSkipDuplicateFramesEnabled();
  void updateRewind();

  Ui::EmulationSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
