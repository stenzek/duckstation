// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_achievementsettingswidget.h"

#include <QtWidgets/QWidget>

class SettingsWindow;

class AchievementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AchievementSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AchievementSettingsWidget();

private:
  void setupAdditionalUi();

  void updateLoginState();

  void updateEnableState();
  void onHardcoreModeStateChanged();
  void onAchievementsNotificationDurationSliderChanged();
  void onLeaderboardsNotificationDurationSliderChanged();
  void onLoginLogoutPressed();
  void onLoginCompleted();
  void onViewProfilePressed();

  Ui::AchievementSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
