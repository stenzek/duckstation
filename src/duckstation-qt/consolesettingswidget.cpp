#include "consolesettingswidget.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QFileDialog>

static constexpr char BIOS_IMAGE_FILTER[] = "Binary Images (*.bin);;All Files (*.*)";

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
    m_ui.region->addItem(tr(Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(i))));

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    m_ui.cpuExecutionMode->addItem(tr(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console/Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.biosPath, "BIOS/Path");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableTTYOutput, "BIOS/PatchTTYEnable");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fastBoot, "BIOS/PatchFastBoot");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter,
                                               "General/SpeedLimiterEnabled");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.increaseTimerResolution,
                                               "General/IncreaseTimerResolution");
  SettingWidgetBinder::BindWidgetToNormalizedSetting(m_host_interface, m_ui.emulationSpeed, "General/EmulationSpeed",
                                                     100.0f);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "General/StartPaused");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.saveStateOnExit, "General/SaveStateOnExit");
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.cpuExecutionMode, "CPU/ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName);

  connect(m_ui.biosPathBrowse, &QPushButton::pressed, this, &ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked);

  connect(m_ui.enableSpeedLimiter, &QCheckBox::stateChanged, this,
          &ConsoleSettingsWidget::onEnableSpeedLimiterStateChanged);
  connect(m_ui.emulationSpeed, &QSlider::valueChanged, this, &ConsoleSettingsWidget::onEmulationSpeedValueChanged);

  onEnableSpeedLimiterStateChanged();
  onEmulationSpeedValueChanged(m_ui.emulationSpeed->value());
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked()
{
  QString path = QFileDialog::getOpenFileName(this, tr("Select BIOS Image"), QString(), tr(BIOS_IMAGE_FILTER));
  if (path.isEmpty())
    return;

  m_ui.biosPath->setText(path);

  m_host_interface->putSettingValue("BIOS/Path", path);
  m_host_interface->applySettings();
}

void ConsoleSettingsWidget::onEnableSpeedLimiterStateChanged()
{
  m_ui.emulationSpeed->setDisabled(!m_ui.enableSpeedLimiter->isChecked());
}

void ConsoleSettingsWidget::onEmulationSpeedValueChanged(int value)
{
  m_ui.emulationSpeedLabel->setText(tr("%1%").arg(value));
}
