// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtWidgets/QWidget>
#include "ui_achievementsettingswidget.h"

class SettingsWindow;

class AchievementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AchievementSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AchievementSettingsWidget();

private Q_SLOTS:
  void updateEnableState();
  void onHardcoreModeStateChanged();
  void onAchievementsNotificationDurationSliderChanged();
  void onLeaderboardsNotificationDurationSliderChanged();
  void onLoginLogoutPressed();
  void onViewProfilePressed();
  void onAchievementsRefreshed(quint32 id, const QString& game_info_string);

private:
  void updateLoginState();

  Ui::AchievementSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
