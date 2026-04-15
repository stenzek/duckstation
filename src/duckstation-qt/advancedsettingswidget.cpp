// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "advancedsettingswidget.h"
#include "logwindow.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

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
