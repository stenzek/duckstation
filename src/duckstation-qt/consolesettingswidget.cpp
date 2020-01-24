#include "consolesettingswidget.h"
#include "settingwidgetbinder.h"

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console/Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.biosPath, "BIOS/Path");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableTTYOutput, "BIOS/PatchTTYEnable");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fastBoot, "BIOS/PatchFastBoot");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter,
                                               "General/SpeedLimiterEnabled");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "General/StartPaused");
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;
