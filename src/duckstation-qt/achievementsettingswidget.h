#pragma once
#include <QtWidgets/QWidget>
#include "ui_achievementsettingswidget.h"

class SettingsDialog;

class AchievementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AchievementSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~AchievementSettingsWidget();

private Q_SLOTS:
  void updateEnableState();
  void onLoginLogoutPressed();
  void onViewProfilePressed();
  void onAchievementsRefreshed(quint32 id, const QString& game_info_string, quint32 total, quint32 points);

private:
  void updateLoginState();

  Ui::AchievementSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
