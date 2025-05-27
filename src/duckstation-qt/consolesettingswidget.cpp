// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "consolesettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/game_database.h"
#include "core/system.h"

#include "util/cd_image.h"

#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

static constexpr const int CDROM_SPEEDUP_VALUES[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0};

ConsoleSettingsWidget::ConsoleSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
  {
    m_ui.region->addItem(QtUtils::GetIconForRegion(static_cast<ConsoleRegion>(i)),
                         QString::fromUtf8(Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(ForceVideoTimingMode::Count); i++)
  {
    const ForceVideoTimingMode mode = static_cast<ForceVideoTimingMode>(i);
    const QIcon region_icon =
      QtUtils::GetIconForRegion((mode == ForceVideoTimingMode::Disabled) ?
                                  ConsoleRegion::Auto :
                                  ((mode == ForceVideoTimingMode::NTSC) ? ConsoleRegion::NTSC_U : ConsoleRegion::PAL));
    m_ui.forceVideoTiming->addItem(region_icon, QString::fromUtf8(Settings::GetForceVideoTimingDisplayName(mode)));
  }

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    m_ui.cpuExecutionMode->addItem(
      QString::fromUtf8(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.region, "Console", "Region", &Settings::ParseConsoleRegionName,
                                               &Settings::GetConsoleRegionName, Settings::DEFAULT_CONSOLE_REGION);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.forceVideoTiming, "GPU", "ForceVideoTiming",
                                               &Settings::ParseForceVideoTimingName, &Settings::GetForceVideoTimingName,
                                               Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastBoot, "BIOS", "PatchFastBoot", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastForwardBoot, "BIOS", "FastForwardBoot", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enable8MBRAM, "Console", "Enable8MBRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastForwardMemoryCardAccess, "MemoryCards",
                                               "FastForwardAccess", false);
  connect(m_ui.fastBoot, &QCheckBox::checkStateChanged, this, &ConsoleSettingsWidget::onFastBootChanged);
  onFastBootChanged();

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.cpuExecutionMode, "CPU", "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableCPUClockSpeedControl, "CPU", "OverclockEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.recompilerICache, "CPU", "RecompilerICache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImageToRAM, "CDROM", "LoadImageToRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImagePatches, "CDROM", "LoadImagePatches", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromAutoDiscChange, "CDROM", "AutoDiscChange", false);

  if (!m_dialog->isPerGameSettings())
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromIgnoreDriveSubcode, "CDROM", "IgnoreHostSubcode",
                                                 false);
  }
  else
  {

    m_ui.cdromIgnoreDriveSubcode->setEnabled(false);
  }

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromSeekSpeedup, "CDROM", "SeekSpeedup", 1,
                                              CDROM_SPEEDUP_VALUES);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromReadSpeedup, "CDROM", "ReadSpeedup", 1,
                                              CDROM_SPEEDUP_VALUES);

  dialog->registerWidgetHelp(m_ui.region, tr("Region"), tr("Auto-Detect"),
                             tr("Determines the emulated hardware type."));
  dialog->registerWidgetHelp(
    m_ui.forceVideoTiming, tr("Force Video Timing"), tr("Disabled"),
    tr("Utilizes the chosen frame timing regardless of the active region. This feature can be used to force PAL games "
       "to run at 60Hz and NTSC games to run at 50Hz. For most games which have a speed tied to the framerate, this "
       "will result in the game running approximately 17% faster or slower. For variable frame rate games, it may not "
       "affect the speed."));
  m_dialog->registerWidgetHelp(m_ui.fastBoot, tr("Fast Boot"), tr("Unchecked"),
                               tr("Skips the boot animation. Safe to enable."));
  m_dialog->registerWidgetHelp(m_ui.fastForwardBoot, tr("Fast Forward Boot"), tr("Unchecked"),
                               tr("Fast forwards through the early loading process when fast booting, saving time. "
                                  "Results may vary between games."));
  dialog->registerWidgetHelp(
    m_ui.enable8MBRAM, tr("Enable 8MB RAM (Dev Console)"), tr("Unchecked"),
    tr("Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles. Games have "
       "to use a larger heap size for "
       "this additional RAM to be usable. Titles which rely on memory mirrors may break, so it should only be used "
       "with compatible mods."));
  m_dialog->registerWidgetHelp(m_ui.fastForwardMemoryCardAccess, tr("Fast Forward Memory Card Access"), tr("Unchecked"),
                               tr("Fast forwards through memory card access, both loading and saving. Can reduce "
                                  "waiting times in games that frequently access memory cards."));

  dialog->registerWidgetHelp(m_ui.cpuExecutionMode, tr("Execution Mode"), tr("Recompiler (Fastest)"),
                             tr("Determines how the emulated CPU executes instructions."));
  dialog->registerWidgetHelp(m_ui.enableCPUClockSpeedControl,
                             tr("Enable Clock Speed Control (Overclocking/Underclocking)"), tr("Unchecked"),
                             tr("When this option is chosen, the clock speed set below will be used."));
  dialog->registerWidgetHelp(m_ui.cpuClockSpeed, tr("Overclocking Percentage"), tr("100%"),
                             tr("Selects the percentage of the normal clock speed the emulated hardware will run at."));
  dialog->registerWidgetHelp(m_ui.recompilerICache, tr("Enable Recompiler ICache"), tr("Unchecked"),
                             tr("Simulates stalls in the recompilers when the emulated CPU would have to fetch "
                                "instructions into its cache. Makes games run closer to their console framerate, at a "
                                "small cost to performance. Interpreter mode always simulates the instruction cache."));

  dialog->registerWidgetHelp(
    m_ui.cdromReadSpeedup, tr("CD-ROM Read Speedup"), tr("None (Double Speed)"),
    tr("Speeds up CD-ROM reads by the specified factor. Only applies to double-speed reads, and is ignored when audio "
       "is playing. May improve loading speeds in some games, at the cost of breaking others."));
  dialog->registerWidgetHelp(
    m_ui.cdromSeekSpeedup, tr("CD-ROM Seek Speedup"), tr("None (Normal Speed)"),
    tr("Reduces the simulated time for the CD-ROM sled to move to different areas of the disc. Can improve loading "
       "times, but crash games which do not expect the CD-ROM to operate faster."));
  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
  dialog->registerWidgetHelp(m_ui.cdromLoadImagePatches, tr("Apply Image Patches"), tr("Unchecked"),
                             tr("Automatically applies patches to disc images when they are present in the same "
                                "directory. Currently only PPF patches are supported with this option."));
  dialog->registerWidgetHelp(
    m_ui.cdromAutoDiscChange, tr("Switch to Next Disc on Stop"), tr("Unchecked"),
    tr("Automatically switches to the next disc in the game when the game stops the CD-ROM motor. No switch will occur "
       "if the last disc in the game is already selected. <strong>Does not work for all games.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.cdromIgnoreDriveSubcode, tr("Ignore Drive Subcode"), tr("Unchecked"),
    tr("Ignores the subchannel provided by the drive when using physical discs, instead always generating subchannel "
       "data. Won't work with libcrypt games, but can improve read reliability on some drives."));

  m_ui.cpuClockSpeed->setEnabled(m_dialog->getEffectiveBoolValue("CPU", "OverclockEnable", false));

  connect(m_ui.enableCPUClockSpeedControl, &QCheckBox::checkStateChanged, this,
          &ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked);
  connect(m_ui.cpuClockSpeed, &QSlider::valueChanged, this, &ConsoleSettingsWidget::onCPUClockSpeedValueChanged);

  SettingWidgetBinder::SetAvailability(m_ui.cpuExecutionModeLabel,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterpreter));
  SettingWidgetBinder::SetAvailability(m_ui.cpuExecutionMode,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterpreter));

  calculateCPUClockValue();
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onFastBootChanged()
{
  const bool fast_boot_enabled =
    m_dialog->getEffectiveBoolValue("BIOS", "PatchFastBoot", Settings::DEFAULT_FAST_BOOT_VALUE);
  m_ui.fastForwardBoot->setEnabled(fast_boot_enabled);
}

void ConsoleSettingsWidget::updateRecompilerICacheEnabled()
{
  const CPUExecutionMode mode =
    Settings::ParseCPUExecutionMode(
      m_dialog
        ->getEffectiveStringValue("CPU", "ExecutionMode",
                                  Settings::GetCPUExecutionModeName(Settings::DEFAULT_CPU_EXECUTION_MODE))
        .c_str())
      .value_or(Settings::DEFAULT_CPU_EXECUTION_MODE);
  m_ui.recompilerICache->setEnabled(mode != CPUExecutionMode::Interpreter);
}

void ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked(int state)
{
  if (state == Qt::Checked &&
      (!m_dialog->isPerGameSettings() || !Host::GetBaseBoolSettingValue("CPU", "OverclockEnable", false)) &&
      !Host::GetBaseBoolSettingValue("UI", "CPUOverclockingWarningShown", false))
  {
    const QString message =
      tr("Enabling CPU overclocking will break games, cause bugs, reduce performance and can significantly increase "
         "system requirements.\n\nBy enabling this option you are agreeing to not create any bug reports unless you "
         "have confirmed the bug also occurs with overclocking disabled.\n\nThis warning will only be shown once.");

    QMessageBox mb(QMessageBox::Warning, tr("CPU Overclocking Warning"), message, QMessageBox::NoButton, this);
    mb.setWindowModality(Qt::WindowModal);
    const QAbstractButton* const yes_button =
      mb.addButton(tr("Yes, I will confirm bugs without overclocking before reporting."), QMessageBox::YesRole);
    mb.addButton(tr("No, take me back to safety."), QMessageBox::NoRole);
    mb.exec();

    if (mb.clickedButton() != yes_button)
    {
      QSignalBlocker sb(m_ui.enableCPUClockSpeedControl);
      if (m_dialog->isPerGameSettings())
      {
        m_ui.enableCPUClockSpeedControl->setCheckState(Qt::PartiallyChecked);
        m_dialog->removeSettingValue("CPU", "OverclockEnable");
      }
      else
      {
        m_ui.enableCPUClockSpeedControl->setCheckState(Qt::Unchecked);
        m_dialog->setBoolSettingValue("CPU", "OverclockEnable", false);
      }

      return;
    }

    Host::SetBaseBoolSettingValue("UI", "CPUOverclockingWarningShown", true);
    Host::CommitBaseSettingChanges();
  }

  m_ui.cpuClockSpeed->setEnabled(m_dialog->getEffectiveBoolValue("CPU", "OverclockEnable", false));
  updateCPUClockSpeedLabel();
}

void ConsoleSettingsWidget::onCPUClockSpeedValueChanged(int value)
{
  const u32 percent = static_cast<u32>(m_ui.cpuClockSpeed->value());
  u32 numerator, denominator;
  Settings::CPUOverclockPercentToFraction(percent, &numerator, &denominator);
  m_dialog->setIntSettingValue("CPU", "OverclockNumerator", static_cast<int>(numerator));
  m_dialog->setIntSettingValue("CPU", "OverclockDenominator", static_cast<int>(denominator));
  updateCPUClockSpeedLabel();
}

void ConsoleSettingsWidget::updateCPUClockSpeedLabel()
{
  const int percent = m_ui.enableCPUClockSpeedControl->isChecked() ? m_ui.cpuClockSpeed->value() : 100;
  const double frequency = (static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0;
  m_ui.cpuClockSpeedLabel->setText(tr("%1% (%2MHz)").arg(percent).arg(frequency / 1000000.0, 0, 'f', 2));
}

void ConsoleSettingsWidget::calculateCPUClockValue()
{
  const u32 numerator = static_cast<u32>(m_dialog->getEffectiveIntValue("CPU", "OverclockNumerator", 1));
  const u32 denominator = static_cast<u32>(m_dialog->getEffectiveIntValue("CPU", "OverclockDenominator", 1));
  const u32 percent = Settings::CPUOverclockFractionToPercent(numerator, denominator);
  QSignalBlocker sb(m_ui.cpuClockSpeed);
  m_ui.cpuClockSpeed->setValue(static_cast<int>(percent));
  updateCPUClockSpeedLabel();
}
