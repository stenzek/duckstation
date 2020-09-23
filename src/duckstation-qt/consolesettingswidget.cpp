#include "consolesettingswidget.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
  {
    m_ui.region->addItem(
      qApp->translate("ConsoleRegion", Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    m_ui.cpuExecutionMode->addItem(
      qApp->translate("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console", "Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                                               Settings::DEFAULT_CONSOLE_REGION);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.cpuExecutionMode, "CPU", "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromReadThread, "CDROM", "ReadThread");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromRegionCheck, "CDROM", "RegionCheck");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromLoadImageToRAM, "CDROM", "LoadImageToRAM",
                                               false);

  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;
