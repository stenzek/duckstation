#include "emulationsettingswidget.h"
#include "core/system.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>

EmulationSettingsWidget::EmulationSettingsWidget(QtHostInterface* host_interface, QWidget* parent,
                                                 SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.syncToHostRefreshRate, "Main",
                                               "SyncToHostRefreshRate", false);

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
    m_ui.syncToHostRefreshRate, tr("Sync To Host Refresh Rate"), tr("Unchecked"),
    tr("Adjusts the emulation speed so the console's refresh rate matches the host's refresh rate when both VSync and "
       "Audio Resampling settings are enabled. This results in the smoothest animations possible, at the cost of "
       "potentially increasing the emulation speed by less than 1%. Sync To Host Refresh Rate will not take effect if "
       "the console's refresh rate is too far from the host's refresh rate. Users with variable refresh rate displays "
       "should disable this option."));
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
