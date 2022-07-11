#include "emulationsettingswidget.h"
#include "common/make_array.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>
#include <limits>

EmulationSettingsWidget::EmulationSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.rewindEnable, "Main", "RewindEnable", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.rewindSaveFrequency, "Main", "RewindFrequency", 10.0f);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.rewindSaveSlots, "Main", "RewindSaveSlots", 10);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.runaheadFrames, "Main", "RunaheadFrameCount", 0);

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
    m_ui.emulationSpeed->setCurrentIndex(0);
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
    m_ui.emulationSpeed->setCurrentIndex(0);
  }
  else
  {
    const int turbo_speed_index = m_ui.turboSpeed->findData(QVariant(effective_turbo_speed));
    if (turbo_speed_index >= 0)
      m_ui.turboSpeed->setCurrentIndex(turbo_speed_index);
  }
  connect(m_ui.turboSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onTurboSpeedIndexChanged);

  connect(m_ui.rewindEnable, &QCheckBox::stateChanged, this, &EmulationSettingsWidget::updateRewind);
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
    m_ui.rewindEnable, tr("Rewinding"), tr("Unchecked"),
    tr("<b>Enable Rewinding:</b> Saves state periodically so you can rewind any mistakes while playing.<br> "
       "<b>Rewind Save Frequency:</b> How often a rewind state will be created. Higher frequencies have greater system "
       "requirements.<br> "
       "<b>Rewind Buffer Size:</b> How many saves will be kept for rewinding. Higher values have greater memory "
       "requirements."));
  dialog->registerWidgetHelp(
    m_ui.runaheadFrames, tr("Runahead"), tr("Disabled"),
    tr(
      "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements."));

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

  static constexpr auto speeds = make_array(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 125, 150, 175, 200, 250, 300, 350,
                                            400, 450, 500, 600, 700, 800, 900, 1000);
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

void EmulationSettingsWidget::updateRewind()
{
  const bool rewind_enabled = m_dialog->getEffectiveBoolValue("Main", "RewindEnable", false);
  const bool runahead_enabled = m_dialog->getIntValue("Main", "RunaheadFrameCount", 0) > 0;
  m_ui.rewindEnable->setEnabled(!runahead_enabled);

  if (!runahead_enabled && rewind_enabled)
  {
    const u32 frames = static_cast<u32>(m_ui.rewindSaveSlots->value());
    const float frequency = static_cast<float>(m_ui.rewindSaveFrequency->value());
    const float duration =
      ((frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : frequency) * static_cast<float>(frames);

    u64 ram_usage, vram_usage;
    System::CalculateRewindMemoryUsage(frames, &ram_usage, &vram_usage);

    m_ui.rewindSummary->setText(
      tr("Rewind for %n frame(s), lasting %1 second(s) will require up to %2MB of RAM and %3MB of VRAM.", "", frames)
        .arg(duration)
        .arg(ram_usage / 1048576)
        .arg(vram_usage / 1048576));
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
