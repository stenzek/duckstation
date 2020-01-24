#include "audiosettingswidget.h"
#include "settingwidgetbinder.h"

AudioSettingsWidget::AudioSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.audioBackend, "Audio/Backend",
                                               &Settings::ParseAudioBackend, &Settings::GetAudioBackendName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.syncToOutput, "Audio/Sync");
}

AudioSettingsWidget::~AudioSettingsWidget() = default;
