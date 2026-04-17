// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "ui_advancedsettingswidget.h"

class SettingsWindow;

class AdvancedSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AdvancedSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AdvancedSettingsWidget();

private:
  void onLogChannelsButtonClicked();
  void onAnyLogSinksChanged();
  void onShowDebugOptionsStateChanged();
  void refreshWebCacheSize();
  void onClearWebCacheClicked();

  SettingsWindow* m_dialog;

  Ui::AdvancedSettingsWidget m_ui;
};
