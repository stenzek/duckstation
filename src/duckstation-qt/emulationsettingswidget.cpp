// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "emulationsettingswidget.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include <QtCore/QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <limits>

#include "moc_emulationsettingswidget.cpp"

namespace {
struct SpeedControl
{
  SettingsWindow* dialog;
  QSpinBox* spinbox;
  QCheckBox* checkbox;
  const char* setting_name;
  float default_value;
};
} // namespace

static void UpdateSpeedControlState(const SpeedControl& control)
{
  const std::optional<float> value = control.dialog->getFloatValue("Main", control.setting_name, std::nullopt);
  const float effective_value =
    control.dialog->getEffectiveFloatValue("Main", control.setting_name, control.default_value);
  const bool inherited = (control.dialog->isPerGameSettings() && !value.has_value());

  QSignalBlocker spinbox_blocker(control.spinbox);
  QSignalBlocker checkbox_blocker(control.checkbox);

  control.checkbox->setTristate(inherited);
  if (inherited)
  {
    const QString inherited_text =
      (effective_value == 0.0f) ?
        QCoreApplication::translate("EmulationSettingsWidget", "Use Global Setting [Unlimited]") :
        QCoreApplication::translate("EmulationSettingsWidget", "Use Global Setting [%1%]")
          .arg(static_cast<int>(std::round(effective_value * 100.0f)));

    control.checkbox->setCheckState(Qt::PartiallyChecked);
    control.spinbox->setMinimum(0);
    control.spinbox->setValue(0);
    control.spinbox->setSpecialValueText(inherited_text);
    control.spinbox->setEnabled(effective_value != 0.0f);
  }
  else
  {
    DebugAssert(value.has_value());
    if (effective_value == 0.0f)
    {
      control.checkbox->setCheckState(Qt::Checked);
      control.spinbox->setEnabled(false);
      control.spinbox->setMinimum(0);
      control.spinbox->setValue(0);
      control.spinbox->setSuffix(QString());
      control.spinbox->setSpecialValueText(QCoreApplication::translate("EmulationSettingsWidget", "N/A"));
    }
    else
    {
      control.checkbox->setCheckState(Qt::Unchecked);
      control.spinbox->setEnabled(true);
      control.spinbox->setValue(static_cast<int>(std::round(effective_value * 100.0f)));
      control.spinbox->setMinimum(1);
      control.spinbox->setSuffix((effective_value == 1.0f) ?
                                   QCoreApplication::translate("EmulationSettingsWidget", "% (Normal Speed)") :
                                   QCoreApplication::translate("EmulationSettingsWidget", "%"));
      control.spinbox->setSpecialValueText(QString());
    }
  }
}

static void OnSpeedControlValueChanged(const SpeedControl& control)
{
  const float value = static_cast<float>(control.spinbox->value()) / 100.0f;
  control.dialog->setFloatSettingValue("Main", control.setting_name, value);
  UpdateSpeedControlState(control);
}

static void OnSpeedControlUnlimitedStateChanged(const SpeedControl& control, Qt::CheckState state)
{
  if (state == Qt::PartiallyChecked)
    control.dialog->removeSettingValue("Main", control.setting_name);
  else if (state == Qt::Checked)
    control.dialog->setFloatSettingValue("Main", control.setting_name, 0.0f);
  else
    control.dialog->setFloatSettingValue("Main", control.setting_name, 1.0f);

  UpdateSpeedControlState(control);
}

static void ResetSpeedControlState(const SpeedControl& control)
{
  if (control.dialog->isPerGameSettings())
    control.dialog->removeSettingValue("Main", control.setting_name);
  else
    control.dialog->setFloatSettingValue("Main", control.setting_name, control.default_value);

  UpdateSpeedControlState(control);
}

static void InitializeSpeedControl(SettingsWindow* dialog, QSpinBox* spinbox, QCheckBox* checkbox,
                                   QPushButton* reset_button, const char* setting_name, float default_value)
{
  const SpeedControl control{dialog, spinbox, checkbox, setting_name, default_value};
  QObject::connect(spinbox, QOverload<int>::of(&QSpinBox::valueChanged), spinbox,
                   [control](int) { OnSpeedControlValueChanged(control); });
  QObject::connect(checkbox, &QCheckBox::checkStateChanged, checkbox,
                   [control](Qt::CheckState state) { OnSpeedControlUnlimitedStateChanged(control, state); });
  QObject::connect(reset_button, &QPushButton::clicked, reset_button, [control]() { ResetSpeedControlState(control); });
  UpdateSpeedControlState(control);
}

EmulationSettingsWidget::EmulationSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vsync, "Display", "VSync", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.syncToHostRefreshRate, "Main", "SyncToHostRefreshRate", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.optimalFramePacing, "Display", "OptimalFramePacing",
                                               Settings::DEFAULT_OPTIMAL_FRAME_PACING);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preFrameSleep, "Display", "PreFrameSleep", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.skipPresentingDuplicateFrames, "Display",
                                               "SkipPresentingDuplicateFrames", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.preFrameSleepBuffer, "Display", "PreFrameSleepBuffer",
                                                Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.rewindEnable, "Main", "RewindEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useSoftwareRendererForMemoryStates, "GPU",
                                               "UseSoftwareRendererForMemoryStates", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.rewindSaveFrequency, "Main", "RewindFrequency", 10.0f);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.rewindSaveSlots, "Main", "RewindSaveSlots", 10);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.runaheadFrames, "Main", "RunaheadFrameCount", 0);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.runaheadForAnalogInput, "Main", "RunaheadForAnalogInput",
                                               false);

  InitializeSpeedControl(dialog, m_ui.normalSpeed, m_ui.normalSpeedUnlimited, m_ui.resetNormalSpeed, "EmulationSpeed",
                         1.0f);
  InitializeSpeedControl(dialog, m_ui.fastForwardSpeed, m_ui.fastForwardSpeedUnlimited, m_ui.resetFastForwardSpeed,
                         "FastForwardSpeed", 0.0f);
  InitializeSpeedControl(dialog, m_ui.turboSpeed, m_ui.turboSpeedUnlimited, m_ui.resetTurboSpeed, "TurboSpeed", 0.0f);
  connect(m_ui.vsync, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.syncToHostRefreshRate, &QCheckBox::checkStateChanged, this,
          &EmulationSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.optimalFramePacing, &QCheckBox::checkStateChanged, this,
          &EmulationSettingsWidget::onOptimalFramePacingChanged);
  connect(m_ui.preFrameSleep, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::onPreFrameSleepChanged);

  connect(m_ui.rewindEnable, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::updateRewind);
  connect(m_ui.useSoftwareRendererForMemoryStates, &QCheckBox::checkStateChanged, this,
          &EmulationSettingsWidget::updateRewind);
  connect(m_ui.rewindSaveFrequency, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          &EmulationSettingsWidget::updateRewind);
  connect(m_ui.rewindSaveSlots, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &EmulationSettingsWidget::updateRewind);
  connect(m_ui.runaheadFrames, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::updateRewind);

  dialog->registerWidgetHelp(
    m_ui.normalSpeed, tr("Emulation Speed"), "100%",
    tr("Sets the target emulation speed. It is not guaranteed that this speed will be reached, "
       "and if not, the emulator will run as fast as it can manage."));
  dialog->registerWidgetHelp(
    m_ui.fastForwardSpeed, tr("Fast Forward Speed"), tr("User Preference"),
    tr("Sets the fast forward speed. This speed will be used when the fast forward hotkey is pressed/toggled."));
  dialog->registerWidgetHelp(
    m_ui.turboSpeed, tr("Turbo Speed"), tr("User Preference"),
    tr("Sets the turbo speed. This speed will be used when the turbo hotkey is pressed/toggled. Turboing will take "
       "priority over fast forwarding if both hotkeys are pressed/toggled."));
  dialog->registerWidgetHelp(
    m_ui.vsync, tr("Vertical Sync (VSync)"), tr("Unchecked"),
    tr("Synchronizes presentation of the console's frames to the host. Enabling may result in smoother animations, at "
       "the cost of increased input lag. <strong>GSync/FreeSync users should enable Optimal Frame Pacing "
       "instead.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.syncToHostRefreshRate, tr("Sync To Host Refresh Rate"), tr("Unchecked"),
    tr(
      "Adjusts the emulation speed so the console's refresh rate matches the host's refresh rate when VSync is "
      "enabled. This results in the smoothest animations possible, at the cost of potentially increasing the emulation "
      "speed by less than 1%. Sync To Host Refresh Rate will not take effect if the console's refresh rate is too far "
      "from the host's refresh rate. Users with variable refresh rate displays should disable this option."));
  dialog->registerWidgetHelp(
    m_ui.optimalFramePacing, tr("Optimal Frame Pacing"),
    Settings::DEFAULT_OPTIMAL_FRAME_PACING ? tr("Checked") : tr("Unchecked"),
    tr("Enabling this option will ensure every frame the console renders is displayed to the screen, at a consistent "
       "rate, for optimal frame pacing. If you have a GSync/FreeSync display, enable this option. If you are having "
       "difficulties maintaining full speed, or are getting audio glitches, try disabling this option."));
  dialog->registerWidgetHelp(
    m_ui.preFrameSleep, tr("Reduce Input Latency"), tr("Unchecked"),
    tr("Reduces input latency by delaying the start of frame until closer to the presentation time. This may cause "
       "dropped frames on slower systems with higher frame time variance, if the buffer size is not sufficient."));
  dialog->registerWidgetHelp(m_ui.preFrameSleepBuffer, tr("Frame Time Buffer"),
                             tr("%1 ms").arg(Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER),
                             tr("Specifies the amount of buffer time added, which reduces the additional sleep time "
                                "introduced. Higher values increase input latency, but decrease the risk of overrun, "
                                "or missed frames. Lower values require faster hardware."));
  dialog->registerWidgetHelp(
    m_ui.skipPresentingDuplicateFrames, tr("Skip Duplicate Frame Display"), tr("Unchecked"),
    tr("Skips the presentation/display of frames that are not unique. Can be combined with driver-level frame "
       "generation to increase perceptible frame rate. Can result in worse frame pacing, and is not compatible with "
       "syncing to host refresh."));
  dialog->registerWidgetHelp(
    m_ui.rewindEnable, tr("Rewinding"), tr("Unchecked"),
    tr("<b>Enable Rewinding:</b> Saves state periodically so you can rewind any mistakes while playing.<br> "
       "<b>Rewind Save Frequency:</b> How often a rewind state will be created. Higher frequencies have greater system "
       "requirements.<br> "
       "<b>Rewind Buffer Size:</b> How many saves will be kept for rewinding. Higher values have greater memory "
       "requirements."));
  dialog->registerWidgetHelp(m_ui.useSoftwareRendererForMemoryStates, tr("Use Software Renderer (Low VRAM Mode)"),
                             tr("Unchecked"),
                             tr("Uses the software renderer when creating rewind states to prevent additional VRAM "
                                "usage. Especially useful when upscaling, as this will significantly reduce the system "
                                "requirements for rewinding."));

  dialog->registerWidgetHelp(
    m_ui.runaheadFrames, tr("Runahead"), tr("Disabled"),
    tr(
      "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements."));
  dialog->registerWidgetHelp(
    m_ui.runaheadForAnalogInput, tr("Enable for Analog Input"), tr("Unchecked"),
    tr("Activates runahead when analog input changes, which significantly increases system requirements."));

  onOptimalFramePacingChanged();
  updateSkipDuplicateFramesEnabled();
  updateRewind();
}

EmulationSettingsWidget::~EmulationSettingsWidget() = default;

void EmulationSettingsWidget::onOptimalFramePacingChanged()
{
  const bool optimal_frame_pacing_enabled = m_dialog->getEffectiveBoolValue("Display", "OptimalFramePacing", false);
  m_ui.preFrameSleep->setEnabled(optimal_frame_pacing_enabled);
  onPreFrameSleepChanged();
}

void EmulationSettingsWidget::onPreFrameSleepChanged()
{
  const bool pre_frame_sleep_enabled = m_dialog->getEffectiveBoolValue("Display", "PreFrameSleep", false);
  const bool show_buffer_size = (m_ui.preFrameSleep->isEnabled() && pre_frame_sleep_enabled);
  m_ui.preFrameSleepBuffer->setVisible(show_buffer_size);
  m_ui.preFrameSleepBufferLabel->setVisible(show_buffer_size);
}

void EmulationSettingsWidget::updateSkipDuplicateFramesEnabled()
{
  const bool vsync = m_dialog->getEffectiveBoolValue("Display", "VSync", false);
  const bool sync_to_host = m_dialog->getEffectiveBoolValue("Main", "SyncToHostRefreshRate", false) && vsync;
  m_ui.skipPresentingDuplicateFrames->setEnabled(!sync_to_host);
}

void EmulationSettingsWidget::updateRewind()
{
  const bool rewind_enabled = m_dialog->getEffectiveBoolValue("Main", "RewindEnable", false);
  const bool runahead_enabled = m_dialog->getIntValue("Main", "RunaheadFrameCount", 0) > 0;
  const bool rewind_active = (!runahead_enabled && rewind_enabled);
  m_ui.rewindEnable->setEnabled(!runahead_enabled);
  m_ui.runaheadForAnalogInput->setEnabled(runahead_enabled);
  m_ui.useSoftwareRendererForMemoryStates->setEnabled(rewind_active);

  if (rewind_active)
  {
    const u32 resolution_scale = static_cast<u32>(m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1));
    const u32 multisamples = m_dialog->getEffectiveIntValue("GPU", "Multisamples", 1);
    const bool use_software_renderer =
      m_dialog->getEffectiveBoolValue("GPU", "UseSoftwareRendererForMemoryStates", false);
    const bool enable_8mb_ram = m_dialog->getEffectiveBoolValue("Console", "Enable8MBRAM", false);
    const u32 frames = static_cast<u32>(m_ui.rewindSaveSlots->value());
    const float frequency = static_cast<float>(m_ui.rewindSaveFrequency->value());
    const float duration =
      ((frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : frequency) * static_cast<float>(frames);

    u64 ram_usage, vram_usage;
    System::CalculateRewindMemoryUsage(frames, resolution_scale, multisamples, use_software_renderer, enable_8mb_ram,
                                       &ram_usage, &vram_usage);

    m_ui.rewindSummary->setText(
      (vram_usage > 0) ?
        tr("Rewind for %n frame(s), lasting %1 second(s) will require %2MB of RAM and %3MB of VRAM.", "", frames)
          .arg(duration)
          .arg(ram_usage / 1048576)
          .arg(vram_usage / 1048576) :
        tr("Rewind for %n frame(s), lasting %1 second(s) will require %2MB of RAM.", "", frames)
          .arg(duration)
          .arg(ram_usage / 1048576));
    m_ui.rewindSaveFrequency->setEnabled(true);
    m_ui.rewindSaveSlots->setEnabled(true);
  }
  else
  {
    if (runahead_enabled)
    {
      m_ui.rewindSummary->setText(tr(
        "Rewind is disabled because runahead is enabled. Runahead will significantly increase system requirements."));
    }
    else
    {
      m_ui.rewindSummary->setText(
        tr("Rewind is not enabled. Please note that enabling rewind may significantly increase system requirements."));
    }
    m_ui.rewindSaveFrequency->setEnabled(false);
    m_ui.rewindSaveSlots->setEnabled(false);
  }
}
