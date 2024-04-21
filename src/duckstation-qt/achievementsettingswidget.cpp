// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "achievementsettingswidget.h"
#include "achievementlogindialog.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/system.h"

#include "common/string_util.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QMessageBox>

AchievementSettingsWidget::AchievementSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enable, "Cheevos", "Enabled", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.hardcoreMode, "Cheevos", "ChallengeMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.achievementNotifications, "Cheevos", "Notifications", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.leaderboardNotifications, "Cheevos",
                                               "LeaderboardNotifications", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.soundEffects, "Cheevos", "SoundEffects", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.overlays, "Cheevos", "Overlays", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.encoreMode, "Cheevos", "EncoreMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.spectatorMode, "Cheevos", "SpectatorMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.unofficialAchievements, "Cheevos", "UnofficialTestMode",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useFirstDiscFromPlaylist, "Cheevos",
                                               "UseFirstDiscFromPlaylist", true);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.achievementNotificationsDuration, "Cheevos",
                                                "NotificationsDuration",
                                                Settings::DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.leaderboardNotificationsDuration, "Cheevos",
                                                "LeaderboardsDuration",
                                                Settings::DEFAULT_LEADERBOARD_NOTIFICATION_TIME);

  dialog->registerWidgetHelp(m_ui.enable, tr("Enable Achievements"), tr("Unchecked"),
                             tr("When enabled and logged in, DuckStation will scan for achievements on startup."));
  dialog->registerWidgetHelp(m_ui.hardcoreMode, tr("Enable Hardcore Mode"), tr("Unchecked"),
                             tr("\"Challenge\" mode for achievements, including leaderboard tracking. Disables save "
                                "state, cheats, and slowdown functions."));
  dialog->registerWidgetHelp(m_ui.achievementNotifications, tr("Show Achievement Notifications"), tr("Checked"),
                             tr("Displays popup messages on events such as achievement unlocks and game completion."));
  dialog->registerWidgetHelp(
    m_ui.leaderboardNotifications, tr("Show Leaderboard Notifications"), tr("Checked"),
    tr("Displays popup messages when starting, submitting, or failing a leaderboard challenge."));
  dialog->registerWidgetHelp(
    m_ui.soundEffects, tr("Enable Sound Effects"), tr("Checked"),
    tr("Plays sound effects for events such as achievement unlocks and leaderboard submissions."));
  dialog->registerWidgetHelp(
    m_ui.overlays, tr("Enable In-Game Overlays"), tr("Checked"),
    tr("Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active."));
  dialog->registerWidgetHelp(m_ui.encoreMode, tr("Enable Encore Mode"), tr("Unchecked"),
                             tr("When enabled, each session will behave as if no achievements have been unlocked."));
  dialog->registerWidgetHelp(m_ui.spectatorMode, tr("Enable Spectator Mode"), tr("Unchecked"),
                             tr("When enabled, DuckStation will assume all achievements are locked and not send any "
                                "unlock notifications to the server."));
  dialog->registerWidgetHelp(
    m_ui.unofficialAchievements, tr("Test Unofficial Achievements"), tr("Unchecked"),
    tr("When enabled, DuckStation will list achievements from unofficial sets. Please note that these achievements are "
       "not tracked by RetroAchievements, so they unlock every time."));
  dialog->registerWidgetHelp(
    m_ui.useFirstDiscFromPlaylist, tr("Use First Disc From Playlist"), tr("Unchecked"),
    tr(
      "When enabled, the first disc in a playlist will be used for achievements, regardless of which disc is active."));

  connect(m_ui.enable, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.hardcoreMode, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.hardcoreMode, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::onHardcoreModeStateChanged);
  connect(m_ui.achievementNotifications, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.leaderboardNotifications, &QCheckBox::checkStateChanged, this, &AchievementSettingsWidget::updateEnableState);
  connect(m_ui.achievementNotificationsDuration, &QSlider::valueChanged, this,
          &AchievementSettingsWidget::onAchievementsNotificationDurationSliderChanged);
  connect(m_ui.leaderboardNotificationsDuration, &QSlider::valueChanged, this,
          &AchievementSettingsWidget::onLeaderboardsNotificationDurationSliderChanged);

  if (!m_dialog->isPerGameSettings())
  {
    connect(m_ui.loginButton, &QPushButton::clicked, this, &AchievementSettingsWidget::onLoginLogoutPressed);
    connect(m_ui.viewProfile, &QPushButton::clicked, this, &AchievementSettingsWidget::onViewProfilePressed);
    connect(g_emu_thread, &EmuThread::achievementsRefreshed, this, &AchievementSettingsWidget::onAchievementsRefreshed);
    updateLoginState();

    // force a refresh of game info
    Host::RunOnCPUThread(Host::OnAchievementsRefreshed);
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
  m_ui.soundEffects->setEnabled(enabled);
  m_ui.overlays->setEnabled(enabled);
  m_ui.encoreMode->setEnabled(enabled);
  m_ui.spectatorMode->setEnabled(enabled);
  m_ui.unofficialAchievements->setEnabled(enabled);
  m_ui.useFirstDiscFromPlaylist->setEnabled(enabled);
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
  auto lock = Achievements::GetLock();
  if (!Achievements::HasActiveGame())
    return;

  if (QMessageBox::question(
        QtUtils::GetRootWidget(this), tr("Reset System"),
        tr("Hardcore mode will not be enabled until the system is reset. Do you want to reset the system now?")) !=
      QMessageBox::Yes)
  {
    return;
  }

  g_emu_thread->resetSystem(true);
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
  const std::string username(Host::GetBaseStringSettingValue("Cheevos", "Username"));
  const bool logged_in = !username.empty();

  if (logged_in)
  {
    const u64 login_unix_timestamp =
      StringUtil::FromChars<u64>(Host::GetBaseStringSettingValue("Cheevos", "LoginTimestamp", "0")).value_or(0);
    const QDateTime login_timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(login_unix_timestamp)));
    m_ui.loginStatus->setText(tr("Username: %1\nLogin token generated on %2.")
                                .arg(QString::fromStdString(username))
                                .arg(login_timestamp.toString(Qt::TextDate)));
    m_ui.loginButton->setText(tr("Logout"));
  }
  else
  {
    m_ui.loginStatus->setText(tr("Not Logged In."));
    m_ui.loginButton->setText(tr("Login..."));
  }

  m_ui.viewProfile->setEnabled(logged_in);
}

void AchievementSettingsWidget::onLoginLogoutPressed()
{
  if (!Host::GetBaseStringSettingValue("Cheevos", "Username").empty())
  {
    Host::RunOnCPUThread([]() { Achievements::Logout(); }, true);
    updateLoginState();
    return;
  }

  AchievementLoginDialog login(this, Achievements::LoginRequestReason::UserInitiated);
  int res = login.exec();
  if (res != 0)
    return;

  updateLoginState();

  // Login can enable achievements/hardcore.
  if (!m_ui.enable->isChecked() && Host::GetBaseBoolSettingValue("Cheevos", "Enabled", false))
  {
    QSignalBlocker sb(m_ui.enable);
    m_ui.enable->setChecked(true);
    updateEnableState();
  }
  if (!m_ui.hardcoreMode->isChecked() && Host::GetBaseBoolSettingValue("Cheevos", "ChallengeMode", false))
  {
    QSignalBlocker sb(m_ui.hardcoreMode);
    m_ui.hardcoreMode->setChecked(true);
  }
}

void AchievementSettingsWidget::onViewProfilePressed()
{
  const std::string username(Host::GetBaseStringSettingValue("Cheevos", "Username"));
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
