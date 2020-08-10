#include "consolesettingswidget.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QFileDialog>

static constexpr char BIOS_IMAGE_FILTER[] = "Binary Images (*.bin);;All Files (*.*)";

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
    m_ui.region->addItem(tr(Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(i))));

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    m_ui.cpuExecutionMode->addItem(tr(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console", "Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                                               Settings::DEFAULT_CONSOLE_REGION);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.biosPath, "BIOS", "Path");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableTTYOutput, "BIOS", "PatchTTYEnable");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fastBoot, "BIOS", "PatchFastBoot");
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.cpuExecutionMode, "CPU", "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromReadThread, "CDROM", "ReadThread");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromRegionCheck, "CDROM", "RegionCheck");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromLoadImageToRAM, "CDROM", "LoadImageToRAM",
                                               false);

  connect(m_ui.biosPathBrowse, &QPushButton::pressed, this, &ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked);

  dialog->registerWidgetHelp(m_ui.fastBoot, tr("Fast Boot"), tr("Unchecked"),
                             tr("Patches the BIOS to skip the console's boot animation. Does not work with all games, "
                                "but usually safe to enabled."));
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked()
{
  QString path = QFileDialog::getOpenFileName(this, tr("Select BIOS Image"), QString(), tr(BIOS_IMAGE_FILTER));
  if (path.isEmpty())
    return;

  m_ui.biosPath->setText(path);

  m_host_interface->SetStringSettingValue("BIOS", "Path", path.toUtf8().constData());
  m_host_interface->applySettings();
}
