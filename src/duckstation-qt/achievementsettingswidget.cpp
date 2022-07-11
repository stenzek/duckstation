#include "achievementsettingswidget.h"
#include "achievementlogindialog.h"
#include "common/string_util.h"
#include "core/system.h"
#include "frontend-common/achievements.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtCore/QDateTime>
#include <QtWidgets/QMessageBox>

AchievementSettingsWidget::AchievementSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enable, "Cheevos", "Enabled", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.richPresence, "Cheevos", "RichPresence", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.challengeMode, "Cheevos", "ChallengeMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.testMode, "Cheevos", "TestMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.unofficialTestMode, "Cheevos", "UnofficialTestMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useFirstDiscFromPlaylist, "Cheevos",
                                               "UseFirstDiscFromPlaylist", true);

  dialog->registerWidgetHelp(m_ui.enable, tr("Enable Achievements"), tr("Unchecked"),
                             tr("When enabled and logged in, DuckStation will scan for achievements on startup."));
  dialog->registerWidgetHelp(m_ui.testMode, tr("Enable Test Mode"), tr("Unchecked"),
                             tr("When enabled, DuckStation will assume all achievements are locked and not send any "
                                "unlock notifications to the server."));
  dialog->registerWidgetHelp(
    m_ui.unofficialTestMode, tr("Test Unofficial Achievements"), tr("Unchecked"),
    tr("When enabled, DuckStation will list achievements from unofficial sets. Please note that these achievements are "
       "not tracked by RetroAchievements, so they unlock every time."));
  dialog->registerWidgetHelp(
    m_ui.richPresence, tr("Enable Rich Presence"), tr("Unchecked"),
    tr("When enabled, rich presence information will be collected and sent to the server where supported."));
  dialog->registerWidgetHelp(
    m_ui.useFirstDiscFromPlaylist, tr("Use First Disc From Playlist"), tr("Unchecked"),
    tr(
      "When enabled, the first disc in a playlist will be used for achievements, regardless of which disc is active."));
  dialog->registerWidgetHelp(m_ui.challengeMode, tr("Enable Hardcore Mode"), tr("Unchecked"),
                             tr("\"Challenge\" mode for achievements. Disables save state, cheats, and slowdown "
                                "functions, but you receive double the achievement points."));

  connect(m_ui.enable, &QCheckBox::stateChanged, this, &AchievementSettingsWidget::updateEnableState);

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
}

AchievementSettingsWidget::~AchievementSettingsWidget() = default;

void AchievementSettingsWidget::updateEnableState()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("Cheevos", "Enabled", false);
  m_ui.testMode->setEnabled(enabled);
  m_ui.useFirstDiscFromPlaylist->setEnabled(enabled);
  m_ui.richPresence->setEnabled(enabled);
  m_ui.challengeMode->setEnabled(enabled);
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

  AchievementLoginDialog login(this);
  int res = login.exec();
  if (res != 0)
    return;

  updateLoginState();
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

void AchievementSettingsWidget::onAchievementsRefreshed(quint32 id, const QString& game_info_string, quint32 total,
                                                        quint32 points)
{
  m_ui.gameInfo->setText(game_info_string);
}
