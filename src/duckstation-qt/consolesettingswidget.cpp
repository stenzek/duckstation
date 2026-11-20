// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "consolesettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/core.h"
#include "core/game_database.h"
#include "core/system.h"

#include "util/cd_image.h"

#include <QtWidgets/QPushButton>

#include "moc_consolesettingswidget.cpp"

static constexpr const u8 CDROM_SPEEDUP_VALUES[] = {1, 2, 3, 4, 5, 6, 0};

ConsoleSettingsWidget::ConsoleSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.region, Settings::CONSOLE_SECTION_NAME, "Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                                               &Settings::GetConsoleRegionDisplayName, Settings::DEFAULT_CONSOLE_REGION,
                                               ConsoleRegion::Count, &QtUtils::GetIconForRegion);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.forceVideoTiming, Settings::GPU_SECTION_NAME, "ForceVideoTiming", &Settings::ParseForceVideoTimingName,
    &Settings::GetForceVideoTimingName, &Settings::GetForceVideoTimingDisplayName,
    Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE, ForceVideoTimingMode::Count, +[](ForceVideoTimingMode mode) {
      return QtUtils::GetIconForRegion(
        (mode == ForceVideoTimingMode::Disabled) ?
          ConsoleRegion::Auto :
          ((mode == ForceVideoTimingMode::NTSC) ? ConsoleRegion::NTSC_U : ConsoleRegion::PAL));
    });
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastBoot, Settings::BIOS_SECTION_NAME, "PatchFastBoot",
                                               Settings::DEFAULT_FAST_BOOT_VALUE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastForwardBoot, Settings::BIOS_SECTION_NAME,
                                               "FastForwardBoot", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.ramSize, Settings::CPU_SECTION_NAME, "RAMSize", &Settings::ParseCPURAMSize, &Settings::GetCPURAMSizeName,
    &Settings::GetCPURAMSizeDisplayName, Settings::DEFAULT_CPU_RAM_SIZE, CPURAMSize::Count, +[](CPURAMSize size) {
      switch (size)
      {
        case CPURAMSize::Default:
          return QIcon(QStringLiteral(":/icons/monochrome/svg/emulation-line.svg"));
        case CPURAMSize::Expanded4MB:
        case CPURAMSize::Expanded4MBForced:
          return QIcon(QStringLiteral(":/icons/monochrome/svg/arcade-line.svg"));
        case CPURAMSize::Expanded8MB:
        case CPURAMSize::Expanded8MBForced:
          return QIcon(QStringLiteral(":/icons/monochrome/svg/code-box-line.svg"));
        default:
          return QIcon(QStringLiteral(":/icons/monochrome/svg/tools-line.svg"));
      }
    });
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.vramSize, Settings::CONSOLE_SECTION_NAME, "VRAMSize", &Settings::ParseGPUVRAMSize,
    &Settings::GetGPUVRAMSizeName, &Settings::GetGPUVRAMSizeDisplayName, Settings::DEFAULT_GPU_VRAM_SIZE,
    GPUVRAMSize::Count, +[](GPUVRAMSize size) {
      return QIcon((size == GPUVRAMSize::Default) ? QStringLiteral(":/icons/monochrome/svg/emulation-line.svg") :
                                                    QStringLiteral(":/icons/monochrome/svg/arcade-line.svg"));
    });
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastForwardMemoryCardAccess,
                                               Settings::MEMORY_CARDS_SECTION_NAME, "FastForwardAccess", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.fastForwardLoading, Settings::INTERFACE_SECTION_NAME,
                                               "FastForwardLoading", false);
  connect(m_ui.fastBoot, &QCheckBox::checkStateChanged, this, &ConsoleSettingsWidget::onFastBootChanged);
  onFastBootChanged();

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.cpuExecutionMode, Settings::CPU_SECTION_NAME, "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               &Settings::GetCPUExecutionModeDisplayName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE, CPUExecutionMode::Count);
  connect(m_ui.cpuExecutionMode, &QComboBox::currentIndexChanged, this, &ConsoleSettingsWidget::onExecutionModeChanged);
  onExecutionModeChanged();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableCPUClockSpeedControl, Settings::CPU_SECTION_NAME,
                                               "OverclockEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.recompilerICache, Settings::CPU_SECTION_NAME,
                                               "RecompilerICache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.idleTimeoutScale, Settings::CPU_SECTION_NAME,
                                               "IdleTimeoutScale", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImageToRAM, Settings::CDROM_SECTION_NAME,
                                               "LoadImageToRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromAutoDiscChange, Settings::CDROM_SECTION_NAME,
                                               "AutoDiscChange", false);

  if (!m_dialog->isPerGameSettings())
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImagePatches, Settings::CDROM_SECTION_NAME,
                                                 "LoadImagePatches", false);
  }
  else
  {
    m_ui.cdromGridLayout->removeWidget(m_ui.cdromLoadImagePatches);
    delete m_ui.cdromLoadImagePatches;
    m_ui.cdromLoadImagePatches = nullptr;
  }

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromSeekSpeedup, Settings::CDROM_SECTION_NAME, "SeekSpeedup",
                                              1, std::span<const u8>(CDROM_SPEEDUP_VALUES));
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromReadSpeedup, Settings::CDROM_SECTION_NAME, "ReadSpeedup",
                                              1, std::span<const u8>(CDROM_SPEEDUP_VALUES));

  dialog->registerWidgetHelp(m_ui.region, tr("Region"),
                             QString::fromUtf8(Settings::GetConsoleRegionDisplayName(Settings::DEFAULT_CONSOLE_REGION)),
                             tr("Determines the emulated hardware type."));
  dialog->registerWidgetHelp(
    m_ui.forceVideoTiming, tr("Frame Rate"),
    QString::fromUtf8(Settings::GetForceVideoTimingDisplayName(Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE)),
    tr("Utilizes the chosen frame timing regardless of the active region. This feature can be used to force PAL games "
       "to run at 60Hz and NTSC games to run at 50Hz. For most games which have a speed tied to the framerate, this "
       "will result in the game running approximately 17% faster or slower. For variable frame rate games, it may not "
       "affect the speed."));
  m_dialog->registerWidgetHelp(m_ui.fastBoot, tr("Fast Boot"), tr("Unchecked"),
                               tr("Skips the boot animation. Safe to enable."));
  m_dialog->registerWidgetHelp(m_ui.fastForwardBoot, tr("Fast Forward Boot"), tr("Unchecked"),
                               tr("Fast forwards through the early loading process when fast booting, saving time. "
                                  "Results may vary between games."));
  m_dialog->registerWidgetHelp(m_ui.fastForwardMemoryCardAccess, tr("Fast Forward Memory Card Access"), tr("Unchecked"),
                               tr("Fast forwards through memory card access, both loading and saving. Can reduce "
                                  "waiting times in games that frequently access memory cards."));
  m_dialog->registerWidgetHelp(m_ui.fastForwardLoading, tr("Fast Forward While Loading"), tr("Unchecked"),
                               tr("Fast forwards when the game stops updating the screen while reading from the disc. "
                                  "Does not apply to animated loading screens."));
  dialog->registerWidgetHelp(
    m_ui.ramSize, tr("Installed RAM"),
    QString::fromUtf8(Settings::GetCPURAMSizeDisplayName(Settings::DEFAULT_CPU_RAM_SIZE)),
    tr("Changes the amount of RAM installed in the emulated console. Retail games do not use extra RAM. Forced maps "
       "all installed RAM regardless of game configuration, needed for 16MB with retail BIOSes."));
  dialog->registerWidgetHelp(
    m_ui.vramSize, tr("Installed VRAM"),
    QString::fromUtf8(Settings::GetGPUVRAMSizeDisplayName(Settings::DEFAULT_GPU_VRAM_SIZE)),
    tr("Changes the amount of VRAM installed in the emulated console. Retail games do not use extra VRAM. Forced makes "
       "the second VRAM bank accessible regardless of the game enabling it or not."));

  dialog->registerWidgetHelp(
    m_ui.cpuExecutionMode, tr("Execution Mode"),
    QString::fromUtf8(Settings::GetCPUExecutionModeDisplayName(Settings::DEFAULT_CPU_EXECUTION_MODE)),
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
    m_ui.idleTimeoutScale, tr("Scale Idle Timeout"), tr("Checked"),
    tr("Games give up waiting for vertical blank after a fixed number of loop iterations, which pass faster when "
       "overclocking. Scales this timeout to keep games synchronized to vertical blank."));

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

  m_ui.cpuClockSpeed->setEnabled(m_dialog->getEffectiveBoolValue(Settings::CPU_SECTION_NAME, "OverclockEnable", false));

  connect(m_ui.enableCPUClockSpeedControl, &QCheckBox::checkStateChanged, this,
          &ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked);
  connect(m_ui.cpuClockSpeed, &QSlider::valueChanged, this, &ConsoleSettingsWidget::onCPUClockSpeedValueChanged);

  SettingWidgetBinder::SetAvailability(m_ui.fastBoot, !m_dialog->hasGameTrait(GameDatabase::Trait::ForceFullBoot));
  SettingWidgetBinder::SetAvailability(m_ui.fastForwardBoot,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceFullBoot));
  SettingWidgetBinder::SetAvailability(
    m_ui.fastForwardMemoryCardAccess, !m_dialog->hasGameTrait(GameDatabase::Trait::DisableFastForwardMemoryCardAccess));
  SettingWidgetBinder::SetAvailability(m_ui.fastForwardLoading,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableFastForwardLoading));
  SettingWidgetBinder::SetAvailability(
    m_ui.cpuExecutionMode, !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterpreter), m_ui.cpuExecutionModeLabel);
  SettingWidgetBinder::SetAvailability(m_ui.cdromReadSpeedup,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableCDROMReadSpeedup),
                                       m_ui.cdromReadSpeedupLabel);
  SettingWidgetBinder::SetAvailability(m_ui.cdromSeekSpeedup,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableCDROMSeekSpeedup),
                                       m_ui.cdromSeekSpeedupLabel);
  SettingWidgetBinder::SetForceEnabled(m_ui.recompilerICache,
                                       m_dialog->hasGameTrait(GameDatabase::Trait::ForceRecompilerICache));

  calculateCPUClockValue();
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onFastBootChanged()
{
  const bool fast_boot_enabled =
    m_dialog->getEffectiveBoolValue(Settings::BIOS_SECTION_NAME, "PatchFastBoot", Settings::DEFAULT_FAST_BOOT_VALUE);
  m_ui.fastForwardBoot->setEnabled(fast_boot_enabled);
}

void ConsoleSettingsWidget::onExecutionModeChanged()
{
  const CPUExecutionMode mode =
    Settings::ParseCPUExecutionMode(m_dialog->getEffectiveStringValue(Settings::CPU_SECTION_NAME, "ExecutionMode"))
      .value_or(Settings::DEFAULT_CPU_EXECUTION_MODE);
  m_ui.recompilerICache->setEnabled(mode != CPUExecutionMode::Interpreter &&
                                    !m_dialog->hasGameTrait(GameDatabase::Trait::ForceRecompilerICache));
  m_ui.idleTimeoutScale->setEnabled(mode != CPUExecutionMode::Interpreter);
}

void ConsoleSettingsWidget::onEnableCPUClockSpeedControlChecked(int state)
{
  if (state == Qt::Checked &&
      (!m_dialog->isPerGameSettings() ||
       !Core::GetBaseBoolSettingValue(Settings::CPU_SECTION_NAME, "OverclockEnable", false)) &&
      !Core::GetBaseBoolSettingValue(Settings::UI_SECTION_NAME, "CPUOverclockingWarningShown", false))
  {
    const QString message =
      tr("Enabling CPU overclocking will break games, cause bugs, reduce performance and can significantly increase "
         "system requirements.\n\nBy enabling this option you are agreeing to not create any bug reports unless you "
         "have confirmed the bug also occurs with overclocking disabled.\n\nThis warning will only be shown once.");

    QMessageBox* const mb = QtUtils::NewMessageBox(this, QMessageBox::Warning, tr("CPU Overclocking Warning"), message,
                                                   QMessageBox::NoButton, QMessageBox::NoButton);
    const QPushButton* const yes_button =
      mb->addButton(tr("Yes, I will confirm bugs without overclocking before reporting."), QMessageBox::YesRole);
    const QPushButton* const no_button = mb->addButton(tr("No, take me back to safety."), QMessageBox::NoRole);
    connect(no_button, &QPushButton::clicked, this, [this]() {
      QSignalBlocker sb(m_ui.enableCPUClockSpeedControl);
      if (m_dialog->isPerGameSettings())
      {
        m_ui.enableCPUClockSpeedControl->setCheckState(Qt::PartiallyChecked);
        m_dialog->removeSettingValue(Settings::CPU_SECTION_NAME, "OverclockEnable");
      }
      else
      {
        m_ui.enableCPUClockSpeedControl->setCheckState(Qt::Unchecked);
        m_dialog->setBoolSettingValue(Settings::CPU_SECTION_NAME, "OverclockEnable", false);
      }

      m_ui.cpuClockSpeed->setEnabled(
        m_dialog->getEffectiveBoolValue(Settings::CPU_SECTION_NAME, "OverclockEnable", false));
      updateCPUClockSpeedLabel();
    });
    connect(yes_button, &QPushButton::clicked, this, []() {
      Core::SetBaseBoolSettingValue(Settings::UI_SECTION_NAME, "CPUOverclockingWarningShown", true);
      Host::CommitBaseSettingChanges();
    });
    mb->open();
  }

  m_ui.cpuClockSpeed->setEnabled(m_dialog->getEffectiveBoolValue(Settings::CPU_SECTION_NAME, "OverclockEnable", false));
  updateCPUClockSpeedLabel();
}

void ConsoleSettingsWidget::onCPUClockSpeedValueChanged(int value)
{
  const u32 percent = static_cast<u32>(m_ui.cpuClockSpeed->value());
  u32 numerator, denominator;
  Settings::CPUOverclockPercentToFraction(percent, &numerator, &denominator);
  m_dialog->setIntSettingValue(Settings::CPU_SECTION_NAME, "OverclockNumerator", static_cast<int>(numerator));
  m_dialog->setIntSettingValue(Settings::CPU_SECTION_NAME, "OverclockDenominator", static_cast<int>(denominator));
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
  const u32 numerator =
    static_cast<u32>(m_dialog->getEffectiveIntValue(Settings::CPU_SECTION_NAME, "OverclockNumerator", 1));
  const u32 denominator =
    static_cast<u32>(m_dialog->getEffectiveIntValue(Settings::CPU_SECTION_NAME, "OverclockDenominator", 1));
  const u32 percent = Settings::CPUOverclockFractionToPercent(numerator, denominator);
  QSignalBlocker sb(m_ui.cpuClockSpeed);
  m_ui.cpuClockSpeed->setValue(static_cast<int>(percent));
  updateCPUClockSpeedLabel();
}
