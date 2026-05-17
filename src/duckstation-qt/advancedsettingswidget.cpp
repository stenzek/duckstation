// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "advancedsettingswidget.h"
#include "logwindow.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"

#include "util/http_cache.h"
#include "util/object_archive.h"

#include "common/error.h"

#include <QtGui/QCursor>
#include <QtWidgets/QMenu>

#include "moc_advancedsettingswidget.cpp"

using namespace Qt::StringLiterals;

AdvancedSettingsWidget::AdvancedSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(Log::Level::MaxCount); i++)
    m_ui.logLevel->addItem(QString::fromUtf8(Settings::GetLogLevelDisplayName(static_cast<Log::Level>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.logLevel, "Logging", "LogLevel", &Settings::ParseLogLevelName,
                                               &Settings::GetLogLevelName, Log::DEFAULT_LOG_LEVEL);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToConsole, "Logging", "LogToConsole", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToDebug, "Logging", "LogToDebug", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToWindow, "Logging", "LogToWindow", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToFile, "Logging", "LogToFile", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logTimestamps, "Logging", "LogTimestamps", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logFileTimestamps, "Logging", "LogFileTimestamps", false);
  connect(m_ui.logToConsole, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  connect(m_ui.logToWindow, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  connect(m_ui.logToFile, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  onAnyLogSinksChanged(); // initialize enabled/disabled state of checkboxes

  connect(m_ui.logChannels, &QAbstractButton::clicked, this, &AdvancedSettingsWidget::onLogChannelsButtonClicked);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showDebugMenu, "Main", "ShowDebugMenu", false);
  connect(m_ui.showDebugMenu, &QCheckBox::checkStateChanged, this,
          &AdvancedSettingsWidget::onShowDebugOptionsStateChanged);

  SettingWidgetBinder::BindWidgetToFolderSetting(
    sif, m_ui.cacheDirectory, m_ui.cacheDirectoryBrowse, tr("Select Cache Directory"), m_ui.cacheDirectoryOpen,
    m_ui.cacheDirectoryReset, "Folders", "Cache", Path::Combine(EmuFolders::DataRoot, "cache"));
  SettingWidgetBinder::BindWidgetToFolderSetting(
    sif, m_ui.coversDirectory, m_ui.coversDirectoryBrowse, tr("Select Covers Directory"), m_ui.coversDirectoryOpen,
    m_ui.coversDirectoryReset, "Folders", "Covers", Path::Combine(EmuFolders::DataRoot, "covers"));

  connect(m_ui.refreshWebCache, &QAbstractButton::clicked, this, &AdvancedSettingsWidget::refreshWebCacheSize);
  connect(m_ui.clearWebCache, &QAbstractButton::clicked, this, &AdvancedSettingsWidget::onClearWebCacheClicked);

  refreshWebCacheSize();

  dialog->registerWidgetHelp(m_ui.logLevel, tr("Log Level"), tr("Information"),
                             tr("Sets the verbosity of messages logged. Higher levels will log more messages."));
  dialog->registerWidgetHelp(m_ui.logToConsole, tr("Log To System Console"), tr("User Preference"),
                             tr("Logs messages to the console window."));
  dialog->registerWidgetHelp(m_ui.logToDebug, tr("Log To Debug Console"), tr("User Preference"),
                             tr("Logs messages to the debug console where supported."));
  dialog->registerWidgetHelp(m_ui.logToWindow, tr("Log To Window"), tr("User Preference"),
                             tr("Logs messages to the window."));
  dialog->registerWidgetHelp(m_ui.logToFile, tr("Log To File"), tr("User Preference"),
                             tr("Logs messages to duckstation.log in the user directory."));
  dialog->registerWidgetHelp(m_ui.logTimestamps, tr("Log Timestamps"), tr("User Preference"),
                             tr("Includes the elapsed time since the application start in window and console logs."));
  dialog->registerWidgetHelp(m_ui.logFileTimestamps, tr("Log File Timestamps"), tr("User Preference"),
                             tr("Includes the elapsed time since the application start in file logs."));
  dialog->registerWidgetHelp(m_ui.showDebugMenu, tr("Show Debug Menu"), tr("Unchecked"),
                             tr("Shows a debug menu bar with additional statistics and quick settings."));

  dialog->registerWidgetHelp(m_ui.cacheDirectory, tr("Cache Directory"), tr("Default"),
                             tr("Specifies the directory where compiled shaders and game list data will be stored."));
  dialog->registerWidgetHelp(m_ui.coversDirectory, tr("Covers Directory"), tr("Default"),
                             tr("Specifies the directory where game cover images that are used in the game grid and "
                                "Big Picture UI will be stored."));

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
  m_ui.interfaceSettingsLayout->removeWidget(m_ui.useRAIntegration);
  delete m_ui.useRAIntegration;
  m_ui.useRAIntegration = nullptr;
#endif
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;

void AdvancedSettingsWidget::onLogChannelsButtonClicked()
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);
  LogWindow::populateFilterMenu(menu);
  menu->popup(QCursor::pos());
}

void AdvancedSettingsWidget::onAnyLogSinksChanged()
{
  const bool log_to_console = m_dialog->getEffectiveBoolValue("Logging", "LogToConsole", false);
  const bool log_to_window = m_dialog->getEffectiveBoolValue("Logging", "LogToWindow", false);
  const bool log_to_file = m_dialog->getEffectiveBoolValue("Logging", "LogToFile", false);

  m_ui.logTimestamps->setEnabled(log_to_console || log_to_window);
  m_ui.logFileTimestamps->setEnabled(log_to_file);
}

void AdvancedSettingsWidget::onShowDebugOptionsStateChanged()
{
  const bool enabled = QtHost::ShouldShowDebugOptions();
  emit m_dialog->debugOptionsVisibiltyChanged(enabled);
}

void AdvancedSettingsWidget::refreshWebCacheSize()
{
  const auto cache = HTTPCache::GetCacheArchive();

  static constexpr auto to_mb = [](s64 size) { return static_cast<u32>((size + 1048575) / 1048576); };
  const u64 cache_size = cache->GetTotalSize();
  const u64 object_size = cache->GetTotalObjectSize();
  const size_t num_objects = cache->GetSize();

  m_ui.webCacheSize->setText(tr("Current Cache Size: %1 MB (%2 MB in %3 objects)")
                               .arg(to_mb(cache_size))
                               .arg(to_mb(object_size))
                               .arg(num_objects));
}

void AdvancedSettingsWidget::onClearWebCacheClicked()
{
  Error error;
  if (!HTTPCache::Clear(&error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, "Failed to clear web cache"_L1,
                             QString::fromStdString(error.GetDescription()));
  }

  refreshWebCacheSize();
}
