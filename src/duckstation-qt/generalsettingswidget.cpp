#include "generalsettingswidget.h"
#include "settingwidgetbinder.h"

static constexpr char BIOS_IMAGE_FILTER[] = "Binary Images (*.bin);;All Files (*.*)";

GeneralSettingsWidget::GeneralSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter, "Main/SpeedLimiterEnabled");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.increaseTimerResolution,
                                               "Main/IncreaseTimerResolution");
  SettingWidgetBinder::BindWidgetToNormalizedSetting(m_host_interface, m_ui.emulationSpeed, "Main/EmulationSpeed",
                                                     100.0f);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "Main/StartPaused");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startFullscreen, "Main/StartFullscreen");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.saveStateOnExit, "Main/SaveStateOnExit");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.confirmPowerOff, "Main/ConfirmPowerOff");

  connect(m_ui.enableSpeedLimiter, &QCheckBox::stateChanged, this,
          &GeneralSettingsWidget::onEnableSpeedLimiterStateChanged);
  connect(m_ui.emulationSpeed, &QSlider::valueChanged, this, &GeneralSettingsWidget::onEmulationSpeedValueChanged);

  onEnableSpeedLimiterStateChanged();
  onEmulationSpeedValueChanged(m_ui.emulationSpeed->value());
}

GeneralSettingsWidget::~GeneralSettingsWidget() = default;

void GeneralSettingsWidget::onEnableSpeedLimiterStateChanged()
{
  m_ui.emulationSpeed->setDisabled(!m_ui.enableSpeedLimiter->isChecked());
}

void GeneralSettingsWidget::onEmulationSpeedValueChanged(int value)
{
  m_ui.emulationSpeedLabel->setText(tr("%1%").arg(value));
}
