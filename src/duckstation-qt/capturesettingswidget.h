// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_capturesettingswidget.h"

class SettingsWindow;

class CaptureSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  CaptureSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~CaptureSettingsWidget();

private:
  void onMediaCaptureBackendChanged();
  void onMediaCaptureContainerChanged();
  void onMediaCaptureVideoEnabledChanged();
  void onMediaCaptureVideoAutoResolutionChanged();
  void onMediaCaptureAudioEnabledChanged();

  Ui::CaptureSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
