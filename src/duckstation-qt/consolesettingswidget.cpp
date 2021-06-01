#include "consolesettingswidget.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>

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

  for (u32 i = 0; i < static_cast<u32>(MultitapMode::Count); i++)
  {
    m_ui.multitapMode->addItem(
      qApp->translate("MultitapMode", Settings::GetMultitapModeDisplayName(static_cast<MultitapMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console", "Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                                               Settings::DEFAULT_CONSOLE_REGION);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enable8MBRAM, "Console", "Enable8MBRAM", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.cpuExecutionMode, "CPU", "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableCPUClockSpeedControl, "CPU",
                                               "OverclockEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromReadThread, "CDROM", "ReadThread", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromRegionCheck, "CDROM", "RegionCheck", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromLoadImageToRAM, "CDROM", "LoadImageToRAM",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cdromLoadImagePatches, "CDROM",
                                               "LoadImagePatches", false);
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.cdromSeekSpeedup, "CDROM", "SeekSpeedup", 1);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.multitapMode, "ControllerPorts", "MultitapMode",
                                               &Settings::ParseMultitapModeName, &Settings::GetMultitapModeName,
                                               Settings::DEFAULT_MULTITAP_MODE);

  dialog->registerWidgetHelp(m_ui.region, tr("Region"), tr("Auto-Detect"),
                             tr("Determines the emulated hardware type."));
  dialog->registerWidgetHelp(m_ui.cpuExecutionMode, tr("Execution Mode"), tr("Recompiler (Fastest)"),
                             tr("Determines how the emulated CPU executes instructions."));
  dialog->registerWidgetHelp(m_ui.enableCPUClockSpeedControl,
                             tr("Enable Clock Speed Control (Overclocking/Underclocking)"), tr("Unchecked"),
                             tr("When this option is chosen, the clock speed set below will be used."));
  dialog->registerWidgetHelp(m_ui.cpuClockSpeed, tr("Overclocking Percentage"), tr("100%"),
                             tr("Selects the percentage of the normal clock speed the emulated hardware will run at."));
  dialog->registerWidgetHelp(
    m_ui.enable8MBRAM, tr("Enable 8MB RAM (Dev Console)"), tr("Unchecked"),
    tr("Enables an additional 6MB of RAM, usually present on dev consoles. Games have to use a larger heap size for "
       "this additional RAM to be usable, and may break games which rely on memory mirrors, so it should only be used "
       "with compatible mods."));
  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
  dialog->registerWidgetHelp(
    m_ui.cdromReadSpeedup, tr("CD-ROM Read Speedup"), tr("None (Double Speed)"),
    tr("Speeds up CD-ROM reads by the specified factor. Only applies to double-speed reads, and is ignored when audio "
       "is playing. May improve loading speeds in some games, at the cost of breaking others."));
  dialog->registerWidgetHelp(
    m_ui.cdromSeekSpeedup, tr("CD-ROM Seek Speedup"), tr("None (Normal Speed)"),
    tr("Reduces the simulated time for the CD-ROM sled to move to different areas of the disc. Can improve loading "
       "times, but crash games which do not expect the CD-ROM to operate faster."));
  dialog->registerWidgetHelp(
    m_ui.cdromReadThread, tr("Use Read Thread (Asynchronous)"), tr("Checked"),
    tr("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread."));
  dialog->registerWidgetHelp(m_ui.cdromRegionCheck, tr("Enable Region Check"), tr("Checked"),
                             tr("Simulates the region check present in original, unmodified consoles."));
  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
  dialog->registerWidgetHelp(m_ui.cdromLoadImagePatches, tr("Apply Image Patches"), tr("Unchecked"),
                             tr("Automatically applies patches to disc images when they are present in the same "
                                "directory. Currently only PPF patches are supported with this option."));
  dialog->registerWidgetHelp(
    m_ui.multitapMode, tr("Multitap"), tr("Disabled"),
    tr("Enables multitap support on specified controller ports. Leave disabled for games that do "
       "not support multitap input."));

  m_ui.cpuClockSpeed->setEnabled(m_ui.enableCPUClockSpeedControl->checkState() == Qt::Checked);
  m_ui.cdromReadSpeedup->setCurrentIndex(m_host_interface->GetIntSettingValue("CDROM", "ReadSpeedup", 1) - 1);

  connect(m_ui.enableCPUClockSpeedControl, &QCheckBox::stateChanged, this,
          &ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked);
  connect(m_ui.cpuClockSpeed, &QSlider::valueChanged, this, &ConsoleSettingsWidget::onCPUClockSpeedValueChanged);
  connect(m_ui.cdromReadSpeedup, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ConsoleSettingsWidget::onCDROMReadSpeedupValueChanged);
  connect(m_ui.multitapMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { emit multitapModeChanged(); });

  calculateCPUClockValue();
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked(int state)
{
  if (state == Qt::Checked && !m_host_interface->GetBoolSettingValue("UI", "CPUOverclockingWarningShown", false))
  {
    const QString message =
      tr("Enabling CPU overclocking will break games, cause bugs, reduce performance and can significantly increase "
         "system requirements.\n\nBy enabling this option you are agreeing to not create any bug reports unless you "
         "have confirmed the bug also occurs with overclocking disabled.\n\nThis warning will only be shown once.");
    const QString yes_button = tr("Yes, I will confirm bugs without overclocking before reporting.");
    const QString no_button = tr("No, take me back to safety.");

    if (QMessageBox::question(QtUtils::GetRootWidget(this), tr("CPU Overclocking Warning"), message, yes_button,
                              no_button) != 0)
    {
      QSignalBlocker sb(m_ui.enableCPUClockSpeedControl);
      m_ui.enableCPUClockSpeedControl->setChecked(Qt::Unchecked);
      m_host_interface->SetBoolSettingValue("CPU", "OverclockEnable", false);
      return;
    }

    m_host_interface->SetBoolSettingValue("UI", "CPUOverclockingWarningShown", true);
  }

  m_ui.cpuClockSpeed->setEnabled(state == Qt::Checked);
  updateCPUClockSpeedLabel();
}

void ConsoleSettingsWidget::onCPUClockSpeedValueChanged(int value)
{
  const u32 percent = static_cast<u32>(m_ui.cpuClockSpeed->value());
  u32 numerator, denominator;
  Settings::CPUOverclockPercentToFraction(percent, &numerator, &denominator);
  m_host_interface->SetIntSettingValue("CPU", "OverclockNumerator", static_cast<int>(numerator));
  m_host_interface->SetIntSettingValue("CPU", "OverclockDenominator", static_cast<int>(denominator));
  updateCPUClockSpeedLabel();
  m_host_interface->applySettings();
}

void ConsoleSettingsWidget::updateCPUClockSpeedLabel()
{
  const int percent = m_ui.enableCPUClockSpeedControl->isChecked() ? m_ui.cpuClockSpeed->value() : 100;
  const double frequency = (static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0;
  m_ui.cpuClockSpeedLabel->setText(tr("%1% (%2MHz)").arg(percent).arg(frequency / 1000000.0, 0, 'f', 2));
}

void ConsoleSettingsWidget::onCDROMReadSpeedupValueChanged(int value)
{
  m_host_interface->SetIntSettingValue("CDROM", "ReadSpeedup", value + 1);
  m_host_interface->applySettings();
}

void ConsoleSettingsWidget::calculateCPUClockValue()
{
  const u32 numerator = static_cast<u32>(m_host_interface->GetIntSettingValue("CPU", "OverclockNumerator", 1));
  const u32 denominator = static_cast<u32>(m_host_interface->GetIntSettingValue("CPU", "OverclockDenominator", 1));
  const u32 percent = Settings::CPUOverclockFractionToPercent(numerator, denominator);
  QSignalBlocker sb(m_ui.cpuClockSpeed);
  m_ui.cpuClockSpeed->setValue(static_cast<int>(percent));
  updateCPUClockSpeedLabel();
}
