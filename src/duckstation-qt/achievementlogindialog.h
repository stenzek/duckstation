// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "ui_achievementlogindialog.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>

class AchievementLoginDialog : public QDialog
{
  Q_OBJECT

public:
  AchievementLoginDialog(QWidget* parent);
  ~AchievementLoginDialog();

private Q_SLOTS:
  void loginClicked();
  void cancelClicked();
  void processLoginResult(bool result);

private:
  void connectUi();
  void enableUI(bool enabled);
  bool canEnableLoginButton() const;

  Ui::AchievementLoginDialog m_ui;
  QPushButton* m_login;
};
