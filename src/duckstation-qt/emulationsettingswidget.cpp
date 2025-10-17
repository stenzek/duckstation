// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "emulationsettingswidget.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include <QtWidgets/QMessageBox>
#include <limits>

#include "moc_emulationsettingswidget.cpp"

EmulationSettingsWidget::EmulationSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vsync, "Display", "VSync", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.syncToHostRefreshRate, "Main", "SyncToHostRefreshRate", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.optimalFramePacing, "Display", "OptimalFramePacing", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preFrameSleep, "Display", "PreFrameSleep", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.skipPresentingDuplicateFrames, "Display",
                                               "SkipPresentingDuplicateFrames", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.preFrameSleepBuffer, "Display", "PreFrameSleepBuffer",
                                                Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.rewindEnable, "Main", "RewindEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.rewindUseSaveStates, "Main", "RewindUseSaveStates", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.rewindSaveFrequency, "Main", "RewindFrequency", 10.0f);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.rewindSaveSlots, "Main", "RewindSaveSlots", 10);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.runaheadFrames, "Main", "RunaheadFrameCount", 0);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.runaheadForAnalogInput, "Main", "RunaheadForAnalogInput",
                                               false);

  const float effective_emulation_speed = m_dialog->getEffectiveFloatValue("Main", "EmulationSpeed", 1.0f);
  fillComboBoxWithEmulationSpeeds(m_ui.emulationSpeed, effective_emulation_speed);
  if (m_dialog->isPerGameSettings() && !m_dialog->getFloatValue("Main", "EmulationSpeed", std::nullopt).has_value())
  {
    m_ui.emulationSpeed->setCurrentIndex(0);
  }
  else
  {
    const int emulation_speed_index = m_ui.emulationSpeed->findData(QVariant(effective_emulation_speed));
    if (emulation_speed_index >= 0)
      m_ui.emulationSpeed->setCurrentIndex(emulation_speed_index);
  }
  connect(m_ui.emulationSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onEmulationSpeedIndexChanged);

  const float effective_fast_forward_speed = m_dialog->getEffectiveFloatValue("Main", "FastForwardSpeed", 0.0f);
  fillComboBoxWithEmulationSpeeds(m_ui.fastForwardSpeed, effective_fast_forward_speed);
  if (m_dialog->isPerGameSettings() && !m_dialog->getFloatValue("Main", "FastForwardSpeed", std::nullopt).has_value())
  {
    m_ui.fastForwardSpeed->setCurrentIndex(0);
  }
  else
  {
    const int fast_forward_speed_index = m_ui.fastForwardSpeed->findData(QVariant(effective_fast_forward_speed));
    if (fast_forward_speed_index >= 0)
      m_ui.fastForwardSpeed->setCurrentIndex(fast_forward_speed_index);
  }
  connect(m_ui.fastForwardSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onFastForwardSpeedIndexChanged);

  const float effective_turbo_speed = m_dialog->getEffectiveFloatValue("Main", "TurboSpeed", 0.0f);
  fillComboBoxWithEmulationSpeeds(m_ui.turboSpeed, effective_turbo_speed);
  if (m_dialog->isPerGameSettings() && !m_dialog->getFloatValue("Main", "TurboSpeed", std::nullopt).has_value())
  {
    m_ui.turboSpeed->setCurrentIndex(0);
  }
  else
  {
    const int turbo_speed_index = m_ui.turboSpeed->findData(QVariant(effective_turbo_speed));
    if (turbo_speed_index >= 0)
      m_ui.turboSpeed->setCurrentIndex(turbo_speed_index);
  }
  connect(m_ui.turboSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onTurboSpeedIndexChanged);
  connect(m_ui.vsync, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.syncToHostRefreshRate, &QCheckBox::checkStateChanged, this,
          &EmulationSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.optimalFramePacing, &QCheckBox::checkStateChanged, this,
          &EmulationSettingsWidget::onOptimalFramePacingChanged);
  connect(m_ui.preFrameSleep, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::onPreFrameSleepChanged);

  connect(m_ui.rewindEnable, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::updateRewind);
  connect(m_ui.rewindUseSaveStates, &QCheckBox::checkStateChanged, this, &EmulationSettingsWidget::updateRewind);
  connect(m_ui.rewindSaveFrequency, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          &EmulationSettingsWidget::updateRewind);
  connect(m_ui.rewindSaveSlots, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &EmulationSettingsWidget::updateRewind);
  connect(m_ui.runaheadFrames, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::updateRewind);

  dialog->registerWidgetHelp(
    m_ui.emulationSpeed, tr("Emulation Speed"), "100%",
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
    m_ui.optimalFramePacing, tr("Optimal Frame Pacing"), tr("Unchecked"),
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
  dialog->registerWidgetHelp(
    m_ui.rewindUseSaveStates, tr("Rewind using Save States"), tr("Unchecked"),
    tr("Uses save state files instead of keeping rewind states in memory. Reduces RAM usage but uses disk space and may "
       "be slightly slower. Opens a menu when the rewind hotkey is pressed, allowing you to choose which state to load. "));
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

void EmulationSettingsWidget::fillComboBoxWithEmulationSpeeds(QComboBox* cb, float global_value)
{
  if (m_dialog->isPerGameSettings())
  {
    if (global_value == 0.0f)
      cb->addItem(tr("Use Global Setting [Unlimited]"));
    else
      cb->addItem(tr("Use Global Setting [%1%]").arg(static_cast<u32>(global_value * 100.0f)));
  }

  cb->addItem(tr("Unlimited"), QVariant(0.0f));

  static constexpr const std::array speeds = {10,  20,  30,  40,  50,  60,  70,  80,  90,  100, 125, 150, 175,
                                              200, 250, 300, 350, 400, 450, 500, 600, 700, 800, 900, 1000};
  for (const int speed : speeds)
  {
    cb->addItem(tr("%1% [%2 FPS (NTSC) / %3 FPS (PAL)]").arg(speed).arg((60 * speed) / 100).arg((50 * speed) / 100),
                QVariant(static_cast<float>(speed) / 100.0f));
  }
}

void EmulationSettingsWidget::onEmulationSpeedIndexChanged(int index)
{
  if (m_dialog->isPerGameSettings() && index == 0)
  {
    m_dialog->removeSettingValue("Main", "EmulationSpeed");
    return;
  }

  bool okay;
  const float value = m_ui.emulationSpeed->currentData().toFloat(&okay);
  m_dialog->setFloatSettingValue("Main", "EmulationSpeed", okay ? value : 1.0f);
}

void EmulationSettingsWidget::onFastForwardSpeedIndexChanged(int index)
{
  if (m_dialog->isPerGameSettings() && index == 0)
  {
    m_dialog->removeSettingValue("Main", "FastForwardSpeed");
    return;
  }

  bool okay;
  const float value = m_ui.fastForwardSpeed->currentData().toFloat(&okay);
  m_dialog->setFloatSettingValue("Main", "FastForwardSpeed", okay ? value : 0.0f);
}

void EmulationSettingsWidget::onTurboSpeedIndexChanged(int index)
{
  if (m_dialog->isPerGameSettings() && index == 0)
  {
    m_dialog->removeSettingValue("Main", "TurboSpeed");
    return;
  }

  bool okay;
  const float value = m_ui.turboSpeed->currentData().toFloat(&okay);
  m_dialog->setFloatSettingValue("Main", "TurboSpeed", okay ? value : 0.0f);
}

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
  const bool use_save_states = m_dialog->getEffectiveBoolValue("Main", "RewindUseSaveStates", false);

  m_ui.rewindEnable->setEnabled(!runahead_enabled);
  m_ui.rewindUseSaveStates->setEnabled(rewind_enabled && !runahead_enabled);
  m_ui.runaheadForAnalogInput->setEnabled(runahead_enabled);

  if (!runahead_enabled && rewind_enabled)
  {
    if (use_save_states)
    {
      const u32 frames = static_cast<u32>(m_ui.rewindSaveSlots->value());
      const float frequency = static_cast<float>(m_ui.rewindSaveFrequency->value());
      const float duration =
        ((frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : frequency) * static_cast<float>(frames);

      m_ui.rewindSummary->setText(
        tr("Rewind for %n frame(s), lasting %1 second(s) will save state and screenshots to disk, compression and resolution will determine file size.", "", frames)
          .arg(duration));
      m_ui.rewindSaveFrequency->setEnabled(true);
      m_ui.rewindSaveSlots->setEnabled(true);
    }
    else
    {
      const u32 resolution_scale = static_cast<u32>(m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1));
      const u32 frames = static_cast<u32>(m_ui.rewindSaveSlots->value());
      const float frequency = static_cast<float>(m_ui.rewindSaveFrequency->value());
      const float duration =
        ((frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : frequency) * static_cast<float>(frames);

      u64 ram_usage, vram_usage;
      System::CalculateRewindMemoryUsage(frames, resolution_scale, &ram_usage, &vram_usage);

      m_ui.rewindSummary->setText(
        tr("Rewind for %n frame(s), lasting %1 second(s) will require up to %2MB of RAM and %3MB of VRAM.", "", frames)
          .arg(duration)
          .arg(ram_usage / 1048576)
          .arg(vram_usage / 1048576));
      m_ui.rewindSaveFrequency->setEnabled(true);
      m_ui.rewindSaveSlots->setEnabled(true);
    }
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
