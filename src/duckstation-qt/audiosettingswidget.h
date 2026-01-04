// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_audiosettingswidget.h"

#include "common/types.h"

#include <QtWidgets/QWidget>

enum class AudioBackend : u8;

class SettingsWindow;

class AudioSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  AudioSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AudioSettingsWidget();

private:
  AudioBackend getEffectiveBackend() const;
  void resetVolume(bool fast_forward);

  void onStretchModeChanged();

  void updateDriverNames();
  void queueUpdateDeviceNames();
  void updateLatencyLabel();
  void updateMinimumLatencyLabel();
  void updateVolumeLabel();
  void onMinimalOutputLatencyChecked(Qt::CheckState state);
  void onOutputVolumeChanged(int new_value);
  void onFastForwardVolumeChanged(int new_value);
  void onOutputMutedChanged(int new_state);

  void onStretchSettingsClicked();

  Ui::AudioSettingsWidget m_ui;
  SettingsWindow* m_dialog;
  u32 m_output_device_latency = 0;
};
