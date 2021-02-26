#pragma once
#include <QtWidgets/QWidget>
#include "ui_achievementsettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class AchievementSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AchievementSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~AchievementSettingsWidget();

private Q_SLOTS:
  void updateEnableState();
  void updateLoginState();
  void onLoginLogoutPressed();
  void onViewProfilePressed();
  void onAchievementsLoaded(quint32 id, const QString& game_info_string, quint32 total, quint32 points);

private:
  Ui::AchievementSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
