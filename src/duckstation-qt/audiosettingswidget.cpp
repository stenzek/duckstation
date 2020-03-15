#include "audiosettingswidget.h"
#include "settingwidgetbinder.h"

AudioSettingsWidget::AudioSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
    m_ui.audioBackend->addItem(tr(Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.audioBackend, "Audio/Backend",
                                               &Settings::ParseAudioBackend, &Settings::GetAudioBackendName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.syncToOutput, "Audio/Sync");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startDumpingOnBoot, "Audio/DumpOnBoot");
}

AudioSettingsWidget::~AudioSettingsWidget() = default;
