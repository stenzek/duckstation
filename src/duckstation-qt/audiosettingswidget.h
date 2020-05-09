#pragma once

#include <QtWidgets/QWidget>

#include "ui_audiosettingswidget.h"

class QtHostInterface;

class AudioSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AudioSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~AudioSettingsWidget();

private Q_SLOTS:
  void updateBufferingLabel();
  void updateVolumeLabel();

private:
  Ui::AudioSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
