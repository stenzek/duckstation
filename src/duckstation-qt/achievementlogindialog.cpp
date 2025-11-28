// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievementlogindialog.h"
#include "qthost.h"

#include "core/achievements.h"

#include "common/error.h"

#include "moc_achievementlogindialog.cpp"

AchievementLoginDialog::AchievementLoginDialog(QWidget* parent, Achievements::LoginRequestReason reason)
  : QDialog(parent), m_reason(reason)
{
  m_ui.setupUi(this);
  m_ui.iconLabel->setPixmap(QPixmap(QtHost::GetResourceQPath("images/ra-icon.webp", true)));
  QFont title_font(m_ui.titleLabel->font());
  title_font.setBold(true);
  title_font.setPixelSize(20);
  m_ui.titleLabel->setFont(title_font);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setAttribute(Qt::WA_DeleteOnClose);

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
    QMetaObject::invokeMethod(this, &AchievementLoginDialog::processLoginResult, Qt::QueuedConnection, result, message);
  });
}

void AchievementLoginDialog::cancelClicked()
{
  // Disable hardcore mode if we cancelled reauthentication.
  if (m_reason == Achievements::LoginRequestReason::TokenInvalid && QtHost::IsSystemValid())
  {
    Host::RunOnCPUThread([]() {
      if (System::IsValid() && !Achievements::HasActiveGame())
        Achievements::DisableHardcoreMode(false, false);
    });
  }

  reject();
}

void AchievementLoginDialog::processLoginResult(bool result, const QString& message)
{
  if (!result)
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Login Error"),
      tr("Login failed.\nError: %1\n\nPlease check your username and password, and try again.").arg(message));
    m_ui.status->setText(tr("Login failed."));
    enableUI(true);
    return;
  }

  // don't ask to enable etc if we are just reauthenticating
  if (m_reason == Achievements::LoginRequestReason::TokenInvalid)
  {
    accept();
    return;
  }

  askToEnableAchievementsAndAccept();
}

void AchievementLoginDialog::askToEnableAchievementsAndAccept()
{
  if (Host::GetBaseBoolSettingValue("Cheevos", "Enabled", false))
  {
    askToEnableHardcoreModeAndAccept();
    return;
  }

  QMessageBox* const msgbox =
    QtUtils::NewMessageBox(this, QMessageBox::Question, tr("Enable Achievements"),
                           tr("Achievement tracking is not currently enabled. Your login will have no effect until "
                              "after tracking is enabled.\n\nDo you want to enable tracking now?"),
                           QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton);
  msgbox->connect(msgbox, &QMessageBox::accepted, this, [this]() {
    Host::SetBaseBoolSettingValue("Cheevos", "Enabled", true);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
    askToEnableHardcoreModeAndAccept();
  });
  msgbox->connect(msgbox, &QMessageBox::rejected, this, &AchievementLoginDialog::accept);
  msgbox->open();
}

void AchievementLoginDialog::askToEnableHardcoreModeAndAccept()
{
  if (Host::GetBaseBoolSettingValue("Cheevos", "ChallengeMode", false))
  {
    askToResetGameAndAccept();
    return;
  }

  QMessageBox* const msgbox = QtUtils::NewMessageBox(
    this, QMessageBox::Question, tr("Enable Hardcore Mode"),
    tr("Hardcore mode is not currently enabled. Enabling hardcore mode allows you to set times, scores, and "
       "participate in game-specific leaderboards.\n\nHowever, hardcore mode also prevents the usage of save "
       "states, cheats and slowdown functionality.\n\nDo you want to enable hardcore mode?"),
    QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton);
  msgbox->connect(msgbox, &QMessageBox::accepted, this, [this]() {
    Host::SetBaseBoolSettingValue("Cheevos", "ChallengeMode", true);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
    askToResetGameAndAccept();
  });
  msgbox->connect(msgbox, &QMessageBox::rejected, this, &AchievementLoginDialog::accept);
  msgbox->open();
}

void AchievementLoginDialog::askToResetGameAndAccept()
{
  if (!QtHost::IsSystemValid())
  {
    accept();
    return;
  }

  QMessageBox* const msgbox = QtUtils::NewMessageBox(
    this, QMessageBox::Question, tr("Reset System"),
    tr("Hardcore mode will not be enabled until the system is reset. Do you want to reset the system now?"),
    QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton);
  msgbox->connect(msgbox, &QMessageBox::accepted, this, [this]() {
    g_emu_thread->resetSystem(true);
    accept();
  });
  msgbox->connect(msgbox, &QMessageBox::rejected, this, &AchievementLoginDialog::accept);
  msgbox->open();
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
