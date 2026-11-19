// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "advancedsettingswidget.h"
#include "logwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"

#include "util/http_cache.h"
#include "util/object_archive.h"
#include "util/translation.h"

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

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.logLevel, Settings::LOGGING_SECTION_NAME, "LogLevel",
                                               &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                                               Log::DEFAULT_LOG_LEVEL);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToConsole, Settings::LOGGING_SECTION_NAME, "LogToConsole",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToDebug, Settings::LOGGING_SECTION_NAME, "LogToDebug",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToWindow, Settings::LOGGING_SECTION_NAME, "LogToWindow",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToFile, Settings::LOGGING_SECTION_NAME, "LogToFile", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logTimestamps, Settings::LOGGING_SECTION_NAME, "LogTimestamps",
                                               true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logFileTimestamps, Settings::LOGGING_SECTION_NAME,
                                               "LogFileTimestamps", false);
  connect(m_ui.logToConsole, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  connect(m_ui.logToWindow, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  connect(m_ui.logToFile, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onAnyLogSinksChanged);
  onAnyLogSinksChanged(); // initialize enabled/disabled state of checkboxes

  QMenu* const log_channels_menu = QtUtils::NewPopupMenu(m_ui.logChannels, false);
  connect(log_channels_menu, &QMenu::aboutToShow, this, [log_channels_menu]() {
    log_channels_menu->clear();
    LogWindow::populateFilterMenu(log_channels_menu);
  });
  m_ui.logChannels->setMenu(log_channels_menu);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showDebugMenu, Settings::INTERFACE_SECTION_NAME,
                                               "ShowDebugMenu", false);
  connect(m_ui.showDebugMenu, &QCheckBox::checkStateChanged, this,
          &AdvancedSettingsWidget::onShowDebugOptionsStateChanged);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.richPresenceMonitor, Settings::ACHIEVEMENTS_SECTION_NAME,
                                               "RichPresenceMonitor", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFrameNumber, Settings::DISPLAY_SECTION_NAME,
                                               "ShowFrameNumber", false);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromReadaheadSectors, Settings::CDROM_SECTION_NAME,
                                              "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS);
  QtUtils::FixSpinBoxTextMargins(m_ui.cdromReadaheadSectors);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromHostReadSpeed, Settings::CDROM_SECTION_NAME,
                                              "HostReadSpeed", 8);
  QtUtils::FixSpinBoxTextMargins(m_ui.cdromHostReadSpeed);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromIgnoreHostSubcode, Settings::CDROM_SECTION_NAME,
                                               "IgnoreHostSubcode", false);

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
  dialog->registerWidgetHelp(
    m_ui.richPresenceMonitor, tr("Rich Presence Monitor"), tr("Unchecked"),
    tr("Always displays the current RetroAchievements rich presence string in the indicator area."));
  dialog->registerWidgetHelp(m_ui.showFrameNumber, tr("Show Frame Number"), tr("Unchecked"),
                             tr("Shows the number of the current frame in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.cdromReadaheadSectors, tr("Readahead Sectors"),
    tr("%n sectors", nullptr, Settings::DEFAULT_CDROM_READAHEAD_SECTORS),
    tr("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread."));
  dialog->registerWidgetHelp(
    m_ui.cdromHostReadSpeed, tr("Physical Disc Drive Speed"), tr("8x"),
    tr("Sets the speed of the host drive when using physical discs. Higher speeds can reduce stutter, but make the "
       "drive louder. Set to \"Don't Change\" to leave the drive at its own speed. Not all drives support this."));
  dialog->registerWidgetHelp(
    m_ui.cdromIgnoreHostSubcode, tr("Ignore Physical Disc Drive Subcode"), tr("Unchecked"),
    tr("Ignores the subchannel provided by the drive when using physical discs, instead always generating subchannel "
       "data. Won't work with libcrypt games, but can improve read reliability on some drives."));

  // RAIntegration is not available on non-win32/x64.
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (Achievements::IsRAIntegrationAvailable())
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useRAIntegration, Settings::ACHIEVEMENTS_SECTION_NAME,
                                                 "UseRAIntegration", false);
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

void AdvancedSettingsWidget::onAnyLogSinksChanged()
{
  const bool log_to_console = m_dialog->getEffectiveBoolValue(Settings::LOGGING_SECTION_NAME, "LogToConsole", false);
  const bool log_to_window = m_dialog->getEffectiveBoolValue(Settings::LOGGING_SECTION_NAME, "LogToWindow", false);
  const bool log_to_file = m_dialog->getEffectiveBoolValue(Settings::LOGGING_SECTION_NAME, "LogToFile", false);

  m_ui.logTimestamps->setEnabled(log_to_console || log_to_window);
  m_ui.logFileTimestamps->setEnabled(log_to_file);
}

void AdvancedSettingsWidget::onShowDebugOptionsStateChanged()
{
  const bool enabled = QtHost::ShouldShowDebugOptions();
  emit m_dialog->debugOptionsVisibilityChanged(enabled);
}

void AdvancedSettingsWidget::refreshWebCacheSize()
{
  const ObjectArchive& cache = HTTPCache::GetCacheArchive();

  const u64 cache_size = cache.GetTotalSize();
  const u64 object_size = cache.GetTotalObjectSize();
  const size_t num_objects = cache.GetSize();

  m_ui.webCacheSize->setText(tr("Current Cache Size: %1 (%2 in %3 objects)")
                               .arg(QtUtils::StringViewToQString(Host::FormatByteSize(cache_size)))
                               .arg(QtUtils::StringViewToQString(Host::FormatByteSize(object_size)))
                               .arg(num_objects));
}

void AdvancedSettingsWidget::onClearWebCacheClicked()
{
  Error error;
  if (!HTTPCache::Clear(&error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, u"Failed to clear web cache"_s,
                             QString::fromStdString(error.GetDescription()));
  }

  refreshWebCacheSize();
}
