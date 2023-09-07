// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "achievementlogindialog.h"
#include "qthost.h"

#include "core/achievements.h"

#include "common/error.h"

#include <QtWidgets/QMessageBox>

AchievementLoginDialog::AchievementLoginDialog(QWidget* parent, Achievements::LoginRequestReason reason)
  : QDialog(parent)
{
  m_ui.setupUi(this);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  // Adjust text if needed based on reason.
  if (reason == Achievements::LoginRequestReason::TokenInvalid)
  {
    m_ui.instructionText->setText(tr("<strong>Your RetroAchievements login token is no longer valid.</strong> You must "
                                     "re-enter your credentials for achievements to be tracked. Your password will not "
                                     "be saved in DuckStation, an access token will be generated and used instead."));
  }

  m_login = m_ui.buttonBox->addButton(tr("&Login"), QDialogButtonBox::AcceptRole);
  m_login->setEnabled(false);
  connectUi();
}

AchievementLoginDialog::~AchievementLoginDialog() = default;

void AchievementLoginDialog::loginClicked()
{
  const QString username(m_ui.userName->text());
  const QString password(m_ui.password->text());

  // TODO: Make cancellable.
  m_ui.status->setText(tr("Logging in..."));
  enableUI(false);

  Host::RunOnCPUThread([this, username, password]() {
    Error error;
    const bool result = Achievements::Login(username.toUtf8().constData(), password.toUtf8().constData(), &error);
    const QString message = QString::fromStdString(error.GetDescription());
    QMetaObject::invokeMethod(this, "processLoginResult", Qt::QueuedConnection, Q_ARG(bool, result),
                              Q_ARG(const QString&, message));
  });
}

void AchievementLoginDialog::cancelClicked()
{
  done(1);
}

void AchievementLoginDialog::processLoginResult(bool result, const QString& message)
{
  if (!result)
  {
    QMessageBox::critical(
      this, tr("Login Error"),
      tr("Login failed.\nError: %1\n\nPlease check your username and password, and try again.").arg(message));
    m_ui.status->setText(tr("Login failed."));
    enableUI(true);
    return;
  }

  done(0);
}

void AchievementLoginDialog::connectUi()
{
  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &AchievementLoginDialog::loginClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &AchievementLoginDialog::cancelClicked);

  auto enableLoginButton = [this](const QString&) { m_login->setEnabled(canEnableLoginButton()); };
  connect(m_ui.userName, &QLineEdit::textChanged, enableLoginButton);
  connect(m_ui.password, &QLineEdit::textChanged, enableLoginButton);
}

void AchievementLoginDialog::enableUI(bool enabled)
{
  m_ui.userName->setEnabled(enabled);
  m_ui.password->setEnabled(enabled);
  m_ui.buttonBox->button(QDialogButtonBox::Cancel)->setEnabled(enabled);
  m_login->setEnabled(enabled && canEnableLoginButton());
}

bool AchievementLoginDialog::canEnableLoginButton() const
{
  return !m_ui.userName->text().isEmpty() && !m_ui.password->text().isEmpty();
}
