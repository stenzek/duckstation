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
  void onEnableToggled(bool checked);
  void onChallengeModeToggled(bool checked);
  void onLoginLogoutPressed();
  void onViewProfilePressed();
  void onAchievementsLoaded(quint32 id, const QString& game_info_string, quint32 total, quint32 points);

private:
  bool confirmChallengeModeEnable();
  void updateEnableState();
  void updateLoginState();

  Ui::AchievementSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
