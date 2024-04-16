// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_audiosettingswidget.h"

class SettingsWindow;

class AudioSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AudioSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AudioSettingsWidget();

private Q_SLOTS:
  void onExpansionModeChanged();
  void onStretchModeChanged();

  void updateDriverNames();
  void updateLatencyLabel();
  void updateVolumeLabel();
  void onMinimalOutputLatencyChecked(bool new_value);
  void onOutputVolumeChanged(int new_value);
  void onFastForwardVolumeChanged(int new_value);
  void onOutputMutedChanged(int new_state);

  void onExpansionSettingsClicked();
  void onStretchSettingsClicked();

private:
  Ui::AudioSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
