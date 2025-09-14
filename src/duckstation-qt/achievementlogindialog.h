// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "ui_achievementlogindialog.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>

namespace Achievements {
enum class LoginRequestReason;
}

class AchievementLoginDialog : public QDialog
{
  Q_OBJECT

public:
  AchievementLoginDialog(QWidget* parent, Achievements::LoginRequestReason reason);
  ~AchievementLoginDialog();

private:
  void connectUi();
  void enableUI(bool enabled);
  bool canEnableLoginButton() const;

  void loginClicked();
  void cancelClicked();
  void processLoginResult(bool result, const QString& message);

  Ui::AchievementLoginDialog m_ui;
  QPushButton* m_login;
  Achievements::LoginRequestReason m_reason;
};
