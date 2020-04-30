#include "advancedsettingswidget.h"
#include "settingwidgetbinder.h"

AdvancedSettingsWidget::AdvancedSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
    m_ui.logLevel->addItem(tr(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.logLevel, QStringLiteral("Logging/LogLevel"),
                                               &Settings::ParseLogLevelName, &Settings::GetLogLevelName);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.logFilter, QStringLiteral("Logging/LogFilter"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToConsole,
                                               QStringLiteral("Logging/LogToConsole"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToDebug, QStringLiteral("Logging/LogToDebug"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToWindow,
                                               QStringLiteral("Logging/LogToWindow"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToFile, QStringLiteral("Logging/LogToFile"));
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;
