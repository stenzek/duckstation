#include "consolesettingswidget.h"
#include "settingwidgetbinder.h"

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.region, &Settings::region);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.biosPath, &Settings::bios_path);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.enableTTYOutput, &Settings::bios_patch_tty_enable);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.fastBoot, &Settings::bios_patch_fast_boot);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.enableSpeedLimiter, &Settings::speed_limiter_enabled);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.pauseOnStart, &Settings::start_paused);
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;
