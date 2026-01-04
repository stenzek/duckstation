// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievementsettingswidget.h"
#include "achievementlogindialog.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/core.h"
#include "core/system.h"

#include "util/translation.h"

#include "common/string_util.h"

#include <QtCore/QDateTime>

#include "moc_achievementsettingswidget.cpp"

AchievementSettingsWidget::AchievementSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enable, "Cheevos", "Enabled", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.hardcoreMode, "Cheevos", "ChallengeMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.encoreMode, "Cheevos", "EncoreMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.spectatorMode, "Cheevos", "SpectatorMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.unofficialAchievements, "Cheevos", "UnofficialTestMode",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.achievementNotifications, "Cheevos", "Notifications", true);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.achievementNotificationsDuration, "Cheevos",
                                                "NotificationsDuration",
                                                Settings::DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.leaderboardNotifications, "Cheevos",
                                               "LeaderboardNotifications", true);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.leaderboardNotificationsDuration, "Cheevos",
                                                "LeaderboardsDuration",
                                                Settings::DEFAULT_LEADERBOARD_NOTIFICATION_TIME);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.leaderboardTrackers, "Cheevos", "LeaderboardTrackers", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.soundEffects, "Cheevos", "SoundEffects", true);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.challengeIndicatorMode, "Cheevos", "ChallengeIndicatorMode",
    &Settings::ParseAchievementChallengeIndicatorMode, &Settings::GetAchievementChallengeIndicatorModeName,
    &Settings::GetAchievementChallengeIndicatorModeDisplayName, Settings::DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE,
    AchievementChallengeIndicatorMode::MaxCount);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.progressIndicators, "Cheevos", "ProgressIndicators", true);

  dialog->registerWidgetHelp(m_ui.enable, tr("Enable Achievements"), tr("Unchecked"),
                             tr("When enabled and logged in, DuckStation will scan for achievements on startup."));
  dialog->registerWidgetHelp(m_ui.hardcoreMode, tr("Enable Hardcore Mode"), tr("Unchecked"),
                             tr("\"Challenge\" mode for achievements, including leaderboard tracking. Disables save "
                                "state, cheats, and slowdown functions."));
  dialog->registerWidgetHelp(m_ui.encoreMode, tr("Enable Encore Mode"), tr("Unchecked"),
                             tr("When enabled, each session will behave as if no achievements have been unlocked."));
  dialog->registerWidgetHelp(m_ui.spectatorMode, tr("Enable Spectator Mode"), tr("Unchecked"),
                             tr("When enabled, DuckStation will assume all achievements are locked and not send any "
                                "unlock notifications to the server."));
  dialog->registerWidgetHelp(
    m_ui.unofficialAchievements, tr("Test Unofficial Achievements"), tr("Unchecked"),
    tr("When enabled, DuckStation will list achievements from unofficial sets. Please note that these achievements are "
       "not tracked by RetroAchievements, so they unlock every time."));
  dialog->registerWidgetHelp(m_ui.achievementNotifications, tr("Show Achievement Notifications"), tr("Checked"),
                             tr("Displays popup messages on events such as achievement unlocks and game completion."));
  dialog->registerWidgetHelp(
    m_ui.leaderboardNotifications, tr("Show Leaderboard Notifications"), tr("Checked"),
    tr("Displays popup messages when starting, submitting, or failing a leaderboard challenge."));
  dialog->registerWidgetHelp(
    m_ui.leaderboardTrackers, tr("Show Leaderboard Trackers"), tr("Checked"),
    tr("Shows a timer in the bottom-right corner of the screen when leaderboard challenges are active."));
  dialog->registerWidgetHelp(
    m_ui.soundEffects, tr("Enable Sound Effects"), tr("Checked"),
    tr("Plays sound effects for events such as achievement unlocks and leaderboard submissions."));
  dialog->registerWidgetHelp(m_ui.challengeIndicatorMode, tr("Challenge Indicators"), tr("Show Notifications"),
                             tr("Shows a notification or icons in the lower-right corner of the screen when a "
                                "challenge/primed achievement is active."));
  dialog->registerWidgetHelp(
    m_ui.progressIndicators, tr("Show Progress Indicators"), tr("Checked"),
    tr("Shows a popup in the lower-right corner of the screen when progress towards a measured achievement changes."));

  connect(m_ui.enable, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.hardcoreMode, &QCheckBox::checkStateChanged, this,
          &AchievementSettingsWidget::onHardcoreModeStateChanged);
  connect(m_ui.achievementNotifications, &QCheckBox::checkStateChanged, this,
          &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.leaderboardNotifications, &QCheckBox::checkStateChanged, this,
          &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.achievementNotificationsDuration, &QSlider::valueChanged, this,
          &AchievementSettingsWidget::onAchievementsNotificationDurationSliderChanged);
  connect(m_ui.leaderboardNotificationsDuration, &QSlider::valueChanged, this,
          &AchievementSettingsWidget::onLeaderboardsNotificationDurationSliderChanged);

  if (!m_dialog->isPerGameSettings())
  {
    connect(m_ui.loginButton, &QPushButton::clicked, this, &AchievementSettingsWidget::onLoginLogoutPressed);
    connect(m_ui.viewProfile, &QPushButton::clicked, this, &AchievementSettingsWidget::onViewProfilePressed);
    connect(m_ui.refreshProgress, &QPushButton::clicked, g_main_window, &MainWindow::refreshAchievementProgress);
    connect(g_core_thread, &CoreThread::achievementsRefreshed, this, &AchievementSettingsWidget::onAchievementsRefreshed);
    updateLoginState();

    // force a refresh of game info
    Host::RunOnCoreThread(Host::OnAchievementsRefreshed);
  }
  else
  {
    // remove login and game info, not relevant for per-game
    m_ui.verticalLayout->removeWidget(m_ui.gameInfoBox);
    m_ui.gameInfoBox->deleteLater();
    m_ui.gameInfoBox = nullptr;
    m_ui.verticalLayout->removeWidget(m_ui.loginBox);
    m_ui.loginBox->deleteLater();
    m_ui.loginBox = nullptr;
  }

  // RAIntegration is not available on non-win32/x64.
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (Achievements::IsRAIntegrationAvailable())
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useRAIntegration, "Cheevos", "UseRAIntegration", false);
  else
    m_ui.useRAIntegration->setEnabled(false);

  dialog->registerWidgetHelp(
    m_ui.useRAIntegration, tr("Enable RAIntegration (Development Only)"), tr("Unchecked"),
    tr("When enabled, DuckStation will load the RAIntegration DLL which allows for achievement development.<br>The "
       "RA_Integration.dll file must be placed in the same directory as the DuckStation executable."));
#else
  m_ui.settingsLayout->removeWidget(m_ui.useRAIntegration);
  delete m_ui.useRAIntegration;
  m_ui.useRAIntegration = nullptr;
#endif

  updateEnableState();
  onAchievementsNotificationDurationSliderChanged();
  onLeaderboardsNotificationDurationSliderChanged();
}

AchievementSettingsWidget::~AchievementSettingsWidget() = default;

void AchievementSettingsWidget::updateEnableState()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("Cheevos", "Enabled", false);
  const bool notifications = enabled && m_dialog->getEffectiveBoolValue("Cheevos", "Notifications", true);
  const bool lb_notifications = enabled && m_dialog->getEffectiveBoolValue("Cheevos", "LeaderboardNotifications", true);
  m_ui.hardcoreMode->setEnabled(enabled);
  m_ui.achievementNotifications->setEnabled(enabled);
  m_ui.leaderboardNotifications->setEnabled(enabled);
  m_ui.achievementNotificationsDuration->setEnabled(notifications);
  m_ui.achievementNotificationsDurationLabel->setEnabled(notifications);
  m_ui.leaderboardNotificationsDuration->setEnabled(lb_notifications);
  m_ui.leaderboardNotificationsDurationLabel->setEnabled(lb_notifications);
  m_ui.leaderboardTrackers->setEnabled(enabled);
  m_ui.soundEffects->setEnabled(enabled);
  m_ui.challengeIndicatorMode->setEnabled(enabled);
  m_ui.challengeIndicatorModeLabel->setEnabled(enabled);
  m_ui.progressIndicators->setEnabled(enabled);
  m_ui.encoreMode->setEnabled(enabled);
  m_ui.spectatorMode->setEnabled(enabled);
  m_ui.unofficialAchievements->setEnabled(enabled);
  if (!m_dialog->isPerGameSettings())
    m_ui.refreshProgress->setEnabled(enabled && m_ui.viewProfile->isEnabled());
}

void AchievementSettingsWidget::onHardcoreModeStateChanged()
{
  if (!QtHost::IsSystemValid())
    return;

  const bool enabled = m_dialog->getEffectiveBoolValue("Cheevos", "Enabled", false);
  const bool challenge = m_dialog->getEffectiveBoolValue("Cheevos", "ChallengeMode", false);
  if (!enabled || !challenge)
    return;

  // don't bother prompting if the game doesn't have achievements
  {
    auto lock = Achievements::GetLock();
    if (!Achievements::HasActiveGame())
      return;
  }

  QMessageBox* const msgbox = QtUtils::NewMessageBox(
    this, QMessageBox::Question, tr("Reset System"),
    tr("Hardcore mode will not be enabled until the system is reset. Do you want to reset the system now?"),
    QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton);
  msgbox->connect(msgbox, &QMessageBox::accepted, this, []() { g_core_thread->resetSystem(true); });
  msgbox->open();
}

void AchievementSettingsWidget::onAchievementsNotificationDurationSliderChanged()
{
  const int duration =
    m_dialog->getEffectiveIntValue("Cheevos", "NotificationsDuration", Settings::DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  m_ui.achievementNotificationsDurationLabel->setText(tr("%n seconds", nullptr, duration));
}

void AchievementSettingsWidget::onLeaderboardsNotificationDurationSliderChanged()
{
  const int duration =
    m_dialog->getEffectiveIntValue("Cheevos", "LeaderboardsDuration", Settings::DEFAULT_LEADERBOARD_NOTIFICATION_TIME);
  m_ui.leaderboardNotificationsDurationLabel->setText(tr("%n seconds", nullptr, duration));
}

void AchievementSettingsWidget::updateLoginState()
{
  const std::string username(Core::GetBaseStringSettingValue("Cheevos", "Username"));
  const bool logged_in = !username.empty();

  if (logged_in)
  {
    const u64 login_unix_timestamp =
      StringUtil::FromChars<u64>(Core::GetBaseStringSettingValue("Cheevos", "LoginTimestamp", "0")).value_or(0);
    const QString login_timestamp =
      QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime, static_cast<s64>(login_unix_timestamp));
    m_ui.loginStatus->setText(
      tr("Username: %1\nLogin token generated on %2.").arg(QString::fromStdString(username)).arg(login_timestamp));
    m_ui.loginButton->setText(tr("Logout"));
  }
  else
  {
    m_ui.loginStatus->setText(tr("Not Logged In."));
    m_ui.loginButton->setText(tr("Login..."));
  }

  m_ui.viewProfile->setEnabled(logged_in);
  m_ui.refreshProgress->setEnabled(logged_in && Core::GetBaseBoolSettingValue("Cheevos", "Enabled", false));
}

void AchievementSettingsWidget::onLoginLogoutPressed()
{
  if (!Core::GetBaseStringSettingValue("Cheevos", "Username").empty())
  {
    Host::RunOnCoreThread([]() { Achievements::Logout(); }, true);
    updateLoginState();
    return;
  }

  AchievementLoginDialog* login = new AchievementLoginDialog(this, Achievements::LoginRequestReason::UserInitiated);
  connect(login, &AchievementLoginDialog::accepted, this, &AchievementSettingsWidget::onLoginCompleted);
  login->open();
}

void AchievementSettingsWidget::onLoginCompleted()
{
  updateLoginState();

  // Login can enable achievements/hardcore.
  if (!m_ui.enable->isChecked() && Core::GetBaseBoolSettingValue("Cheevos", "Enabled", false))
  {
    m_ui.enable->setChecked(true);
    updateEnableState();
  }
  if (!m_ui.hardcoreMode->isChecked() && Core::GetBaseBoolSettingValue("Cheevos", "ChallengeMode", false))
    m_ui.hardcoreMode->setChecked(true);
}

void AchievementSettingsWidget::onViewProfilePressed()
{
  const std::string username(Core::GetBaseStringSettingValue("Cheevos", "Username"));
  if (username.empty())
    return;

  const QByteArray encoded_username(QUrl::toPercentEncoding(QString::fromStdString(username)));
  QtUtils::OpenURL(
    QtUtils::GetRootWidget(this),
    QUrl(QStringLiteral("https://retroachievements.org/user/%1").arg(QString::fromUtf8(encoded_username))));
}

void AchievementSettingsWidget::onAchievementsRefreshed(quint32 id, const QString& game_info_string)
{
  m_ui.gameInfo->setText(game_info_string);
}
