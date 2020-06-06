#include "audiosettingswidget.h"
#include "common/audio_stream.h"
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
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.bufferSize, "Audio/BufferSize");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.volume, "Audio/OutputVolume");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.muted, "Audio/OutputMuted");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startDumpingOnBoot, "Audio/DumpOnBoot");

  connect(m_ui.bufferSize, &QSlider::valueChanged, this, &AudioSettingsWidget::updateBufferingLabel);
  connect(m_ui.volume, &QSlider::valueChanged, this, &AudioSettingsWidget::updateVolumeLabel);

  updateBufferingLabel();
  updateVolumeLabel();
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::updateBufferingLabel()
{
  const u32 buffer_size = static_cast<u32>(m_ui.bufferSize->value());
  const float max_latency = AudioStream::GetMaxLatency(HostInterface::AUDIO_SAMPLE_RATE, buffer_size);
  m_ui.bufferingLabel->setText(
    tr("Maximum latency: %1 frames (%2ms)").arg(buffer_size).arg(max_latency * 1000.0f, 0, 'f', 2));
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
}
