#include "emulationsettingswidget.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>
#include <limits>

EmulationSettingsWidget::EmulationSettingsWidget(QtHostInterface* host_interface, QWidget* parent,
                                                 SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.rewindEnable, "Main", "RewindEnable", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(m_host_interface, m_ui.rewindSaveFrequency, "Main", "RewindFrequency",
                                                10.0f);
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.rewindSaveSlots, "Main", "RewindSaveSlots", 10);
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.runaheadFrames, "Main", "RunaheadFrameCount", 0);

  QtUtils::FillComboBoxWithEmulationSpeeds(m_ui.emulationSpeed);
  const int emulation_speed_index =
    m_ui.emulationSpeed->findData(QVariant(m_host_interface->GetFloatSettingValue("Main", "EmulationSpeed", 1.0f)));
  if (emulation_speed_index >= 0)
    m_ui.emulationSpeed->setCurrentIndex(emulation_speed_index);
  connect(m_ui.emulationSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onEmulationSpeedIndexChanged);
  QtUtils::FillComboBoxWithEmulationSpeeds(m_ui.fastForwardSpeed);
  const int fast_forward_speed_index =
    m_ui.fastForwardSpeed->findData(QVariant(m_host_interface->GetFloatSettingValue("Main", "FastForwardSpeed", 0.0f)));
  if (fast_forward_speed_index >= 0)
    m_ui.fastForwardSpeed->setCurrentIndex(fast_forward_speed_index);
  connect(m_ui.fastForwardSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &EmulationSettingsWidget::onFastForwardSpeedIndexChanged);
  QtUtils::FillComboBoxWithEmulationSpeeds(m_ui.turboSpeed);
  const int turbo_speed_index =
    m_ui.turboSpeed->findData(QVariant(m_host_interface->GetFloatSettingValue("Main", "TurboSpeed", 0.0f)));
  if (turbo_speed_index >= 0)
    m_ui.turboSpeed->setCurrentIndex(turbo_speed_index);
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
	   "<b>Rewind Save Frequency:</b> How often a rewind state will be created. Higher frequencies have greater system requirements.<br> "
	   "<b>Rewind Buffer Size:</b> How many saves will be kept for rewinding. Higher values have greater memory requirements."));
  dialog->registerWidgetHelp(
    m_ui.runaheadFrames, tr("Runahead"), tr("Disabled"),
    tr("Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements."));

  updateRewind();
}

EmulationSettingsWidget::~EmulationSettingsWidget() = default;

void EmulationSettingsWidget::onEmulationSpeedIndexChanged(int index)
{
  bool okay;
  const float value = m_ui.emulationSpeed->currentData().toFloat(&okay);
  m_host_interface->SetFloatSettingValue("Main", "EmulationSpeed", okay ? value : 1.0f);
  m_host_interface->applySettings();
}

void EmulationSettingsWidget::onFastForwardSpeedIndexChanged(int index)
{
  bool okay;
  const float value = m_ui.fastForwardSpeed->currentData().toFloat(&okay);
  m_host_interface->SetFloatSettingValue("Main", "FastForwardSpeed", okay ? value : 0.0f);
  m_host_interface->applySettings();
}

void EmulationSettingsWidget::onTurboSpeedIndexChanged(int index)
{
  bool okay;
  const float value = m_ui.turboSpeed->currentData().toFloat(&okay);
  m_host_interface->SetFloatSettingValue("Main", "TurboSpeed", okay ? value : 0.0f);
  m_host_interface->applySettings();
}

void EmulationSettingsWidget::updateRewind()
{
  m_ui.rewindEnable->setEnabled(!runaheadEnabled());

  if (m_ui.rewindEnable->isEnabled() && m_ui.rewindEnable->isChecked())
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
    if (!m_ui.rewindEnable->isEnabled())
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
