#pragma once
#include "ui_achievementlogindialog.h"
#include <QtWidgets/QDialog>

class AchievementLoginDialog : public QDialog
{
  Q_OBJECT

public:
  AchievementLoginDialog(QWidget* parent);
  ~AchievementLoginDialog();

private Q_SLOTS:
  void loginClicked();
  void cancelClicked();

private:
  void connectUi();
  void enableUI(bool enabled);
  bool canEnableLoginButton() const;

  Ui::AchievementLoginDialog m_ui;
};
