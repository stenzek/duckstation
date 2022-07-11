#pragma once

#include <QtWidgets/QWidget>

#include "ui_audiosettingswidget.h"

class SettingsDialog;

class AudioSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AudioSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~AudioSettingsWidget();

private Q_SLOTS:
  void updateBufferingLabel();
  void updateVolumeLabel();
  void onOutputVolumeChanged(int new_value);
  void onFastForwardVolumeChanged(int new_value);
  void onOutputMutedChanged(int new_state);

private:
  Ui::AudioSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
